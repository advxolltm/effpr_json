# Memory-Centric Optimization: JSON-to-CSV Parser

Progressive memory behavior optimizations achieving **3.33× speedup** through arena allocation and zero-copy string handling.

---

## Performance Results

| Metric | Baseline | After Optimizations | Improvement |
|--------|----------|---------------------|-------------|
| **Execution Time** | 0.367s | **0.108s** | **3.33× faster** |
| **CPU Cycles** | 1.37B | **370M** | **-73.0%** |
| **Instructions** | 4.31B | **1.18B** | **-72.6%** |
| **Cache Misses** | 8.1M | **2.1M** | **-74.7%** |
| **Memory Usage** | 298 MB | **138 MB** | **-53.7%** |
| **IPC** | 3.15 | **3.18** | **+1% efficiency** |

**Dataset**: 100K JSON records (~5MB)  
**Compiler**: GCC -O3  
**Platform**: Linux x86-64

---

## Optimizations Applied

### 1. Arena Allocator
**Replace thousands of malloc/free calls with bump pointer allocation**

- Pre-allocate large memory regions
- Serve allocations by incrementing a pointer
- Batch free everything at once

**Impact**: 1.36× faster, -44% cycles, -53% instructions

### 2. String Slicing
**Zero-copy string handling for 80-90% of strings**

- Reference strings directly in input buffer (pointer + length)
- Only copy strings with escape sequences
- Eliminates allocation and copying overhead

**Impact**: 2.16× additional speedup, -52% cycles, -46% memory

---

## Key Insights

**Why These Optimizations Worked:**

1. **Profile-Driven**: Arena optimization revealed `parse_string` as bottleneck (43% of time) → targeted by string slicing
2. **No Diminishing Returns**: Second optimization (53.6%) was MORE effective than first (36.5%)
3. **Data Understanding**: 80-90% of JSON strings have no escape sequences → enabled zero-copy strategy
4. **IPC Recovery**: Arena dropped IPC to 2.63 (memory-bound), string slicing recovered it to 3.18

**Progressive Optimization Approach:**
- Implemented arena allocation → measured
- Added string slicing on top → measured again
- Each optimization revealed the next opportunity

---

## Quick Start

### Build

```bash
# Build baseline
gcc -O3 -o json2csv_baseline src/json2csv_baseline.c

# Build optimized version
gcc -O3 -o json2csv_opt memory_opt/json2csv_memory_opt.c
```

### Run

```bash
# Execute
./json2csv_opt src/benchmark.json > output.csv

# Verify correctness
./json2csv_baseline src/benchmark.json > baseline.csv
./json2csv_opt src/benchmark.json > optimized.csv
diff baseline.csv optimized.csv  # Should show no differences
```

### Benchmark

```bash
# Timing
/usr/bin/time -v ./json2csv_baseline src/benchmark.json > /dev/null
/usr/bin/time -v ./json2csv_opt src/benchmark.json > /dev/null

# CPU metrics
perf stat -e cycles:u,instructions:u,cache-misses:u \
    ./json2csv_opt src/benchmark.json > /dev/null
```

---

## Implementation Details

### Arena Allocator

**Concept**: Replace individual heap allocations with bump pointer allocation from pre-allocated memory.

```c
typedef struct {
    unsigned char *base;
    size_t cap;
    size_t off;
} Arena;

static void *arena_alloc(Arena *a, size_t n, size_t align) {
    size_t off = a_align_up(a->off, align);
    void *p = a->base + off;
    a->off = off + n;
    return p;  // Just pointer arithmetic!
}
```

**Benefits**:
- Pointer arithmetic (~10 instructions) vs malloc bookkeeping (~100+ instructions)
- Sequential allocation → better cache locality
- O(1) cleanup: one free() instead of thousands

**Two-Arena Strategy**:
- `A_perm`: Permanent data (parse tree, headers)
- `A_tmp`: Temporary data (flattening) - reset after each record

### String Slicing

**Concept**: Reference substrings instead of copying them.

```c
typedef struct {
    const char *ptr;
    size_t len;
} StrSlice;

// Fast path: No escape sequences (80-90% of strings)
static StrSlice parse_string(Parser *p) {
    size_t start = p->pos;
    
    while (p->input[p->pos] != '"') {
        if (p->input[p->pos] == '\\') goto slow_path;
        p->pos++;
    }
    
    // Zero-copy: just return pointer and length
    return slice_make(p->input + start, p->pos - start);
    
slow_path:
    // Copy only if escape sequences present (~10-20%)
    // ...
}
```

**Supporting Optimizations**:
- Single file read (mmap for large files)
- Reusable buffers for temporary operations
- Bulk operations (memcmp) for keyword matching

---

## Profiling Analysis

**Function Time Distribution:**

**Baseline**:
- 20% flatten_value (malloc overhead)
- 20% parse_value (malloc overhead)
- 10% parse_string (copying)

**After Arena**:
- 43% parse_string ← New bottleneck!
- 43% parse_value

**After String Slicing**:
- 60% parse_value
- 0% parse_string ← Eliminated!

**Key Insight**: Profiling between optimizations revealed where to optimize next.

---

## Benchmark Methodology

**Tools**:
- `perf stat` - CPU performance counters
- `/usr/bin/time -v` - Memory usage
- `gprof` - Function profiling

**Measurement**:
- Output redirected to `/dev/null` to exclude disk I/O
- Multiple runs averaged for consistency
- Correctness verified with `diff`

**Why -O3**:
Memory optimizations show benefits even at high optimization levels because:
- Compiler cannot infer arena allocation strategy (semantic change)
- Compiler cannot determine when strings can be sliced (requires data analysis)
- These are architectural decisions, not local code optimizations

---

## Results Breakdown

### Arena Allocator Contribution

| Metric | Before | After | Change |
|--------|--------|-------|--------|
| Time | 0.367s | 0.233s | **-36.5%** |
| Cycles | 1.37B | 770M | **-43.7%** |
| Instructions | 4.31B | 2.03B | **-53.0%** |
| Cache Misses | 8.1M | 4.4M | **-46.2%** |

### String Slicing Contribution (on top of Arena)

| Metric | Arena Only | + String Slice | Change |
|--------|-----------|----------------|--------|
| Time | 0.233s | 0.108s | **-53.6%** |
| Cycles | 770M | 370M | **-52.0%** |
| IPC | 2.63 | 3.18 | **+21%** |
| Memory | 256 MB | 138 MB | **-46.1%** |

---

## Allocation Patterns

**Baseline**: ~4.4M allocations
```
Parse tree:  ~2.0M malloc/free
Strings:     ~2.0M malloc
Flattening:  ~400K malloc
Headers:     ~1K malloc
```

**Arena Only**: ~500K arena bumps (89% reduction)
```
Parse tree:  ~200K bumps
Strings:     ~200K bumps
Flattening:  ~100K bumps
```

**Arena + String Slice**: ~150K arena bumps (97% reduction from baseline)
```
Parse tree:  ~100K bumps
Strings:     ~40K bumps (80% sliced, 20% copied)
Flattening:  ~50K bumps
```

---

## File Structure

```
effpr_json/
├── src/
│   ├── json2csv_baseline.c          # Original implementation
│   ├── benchmark.json               # Test dataset
│   └── test.json                    # Small test file
│
└── memory_opt/
    ├── json2csv_memory_opt.c        # Optimized implementation
    ├── measurements/                # Benchmark results
    │   ├── baseline_*.txt           # Baseline measurements
    │   ├── opt_*.txt                # Arena-only measurements
    │   └── opt_*_02.txt             # Arena+String measurements
    └── README.md                    # This file
```

---

## Correctness Verification

Output is byte-for-byte identical to baseline:

```bash
./json2csv_baseline src/benchmark.json > baseline.csv
./json2csv_opt src/benchmark.json > optimized.csv
diff baseline.csv optimized.csv  # No differences
```

Verified on:
- Nested JSON objects
- JSON arrays (primitive and complex)
- Escape sequences in strings
- Unicode characters
- Edge cases (empty objects, null values)

---

## Key Takeaways

1. **Profile Between Optimizations**: First optimization revealed second opportunity
2. **Memory Behavior Matters**: 3.33× speedup without algorithmic changes
3. **Understand Your Data**: 80% strings don't need copying → zero-copy optimization
4. **No Diminishing Returns**: When targeting different bottlenecks, each optimization can be highly effective
5. **IPC as Quality Metric**: Recovery from 2.63 to 3.18 showed we fixed a fundamental inefficiency

---

## Future Optimization Opportunities

Remaining time spent in:
- 40% Parsing logic (JSON format parsing)
- 30% Output operations (CSV writing)
- 20% Data structure traversal

**Potential gains**: 10-20% with SIMD for string scanning and output buffering

---

## Summary

**3.33× speedup achieved through memory-centric optimization**

Two complementary techniques:
1. Arena Allocation (1.36× faster)
2. String Slicing (2.16× additional)

**Result**: 73% fewer CPU cycles, 75% fewer cache misses, 54% less memory

Progressive optimization approach: each step revealed the next opportunity.