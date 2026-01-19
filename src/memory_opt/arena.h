// arena.h - Simple arena (bump) allocator
// Memory Behavior Optimization: eliminates malloc/free overhead

#ifndef ARENA_H
#define ARENA_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

// Arena structure
typedef struct {
    char *base;
    size_t used;
    size_t capacity;
} Arena;

// Create arena with given capacity
static inline Arena* arena_create(size_t capacity) {
    Arena *arena = malloc(sizeof(Arena));
    if (!arena) {
        fprintf(stderr, "Arena allocation failed\n");
        exit(1);
    }
    
    arena->base = malloc(capacity);
    if (!arena->base) {
        fprintf(stderr, "Arena memory allocation failed\n");
        exit(1);
    }
    
    arena->used = 0;
    arena->capacity = capacity;
    return arena;
}

// Allocate memory from arena (8-byte aligned)
static inline void* arena_alloc(Arena *arena, size_t size) {
    // Align to 8 bytes
    size_t aligned = (size + 7) & ~7;
    
    if (arena->used + aligned > arena->capacity) {
        fprintf(stderr, "Arena out of memory\n");
        exit(1);
    }
    
    void *ptr = arena->base + arena->used;
    arena->used += aligned;
    return ptr;
}

// Reallocate memory (copies data, doesn't free old)
static inline void* arena_realloc(Arena *arena, void *old_ptr, size_t old_size, size_t new_size) {
    if (!old_ptr) {
        return arena_alloc(arena, new_size);
    }
    
    void *new_ptr = arena_alloc(arena, new_size);
    size_t copy_size = old_size < new_size ? old_size : new_size;
    memcpy(new_ptr, old_ptr, copy_size);
    return new_ptr;
}

// Duplicate string into arena
static inline char* arena_strdup(Arena *arena, const char *s) {
    if (!s) return NULL;
    
    size_t len = strlen(s);
    char *copy = arena_alloc(arena, len + 1);
    memcpy(copy, s, len + 1);
    return copy;
}

// Estimate arena size from file size (18x multiplier)
static inline size_t arena_estimate_size(const char *filename) {
    FILE *f = fopen(filename, "rb");
    if (!f) return 10 * 1024 * 1024; // Default 10MB
    
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fclose(f);
    
    if (file_size <= 0) return 10 * 1024 * 1024;
    
    size_t estimate = file_size * 18; // 18x file size
    
    // Min 1MB, max 2GB
    if (estimate < 1024 * 1024) estimate = 1024 * 1024;
    if (estimate > 2UL * 1024 * 1024 * 1024) estimate = 2UL * 1024 * 1024 * 1024;
    
    return estimate;
}

// Destroy arena (free all memory at once)
static inline void arena_destroy(Arena *arena) {
    if (!arena) return;
    free(arena->base);
    free(arena);
}

#endif // ARENA_H
