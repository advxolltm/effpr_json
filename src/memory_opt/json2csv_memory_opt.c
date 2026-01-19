// Memory Behavior & Invariant-Based Optimizations
// Applied Optimizations:
// [X] Arena Allocator - replaces malloc/free with bump allocator
// [X] String Slicing - zero-copy string handling
// [X] Buffer Reuse - reusable buffers for temporary operations
// [X] Input Buffer - single file read with mmap support

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

static void die(const char *msg)
{
    fprintf(stderr, "ERROR: %s\n", msg);
    exit(1);
}

// ---------------- String Slice (zero-copy) ----------------
typedef struct {
    const char *ptr;
    size_t len;
} StrSlice;

static StrSlice slice_make(const char *p, size_t len)
{
    return (StrSlice){.ptr = p, .len = len};
}

static StrSlice slice_from_cstr(const char *s)
{
    return slice_make(s, strlen(s));
}

static int slice_eq(StrSlice a, StrSlice b)
{
    return a.len == b.len && memcmp(a.ptr, b.ptr, a.len) == 0;
}

static int slice_eq_cstr(StrSlice s, const char *cstr)
{
    size_t clen = strlen(cstr);
    return s.len == clen && memcmp(s.ptr, cstr, clen) == 0;
}

// ---------------- Arena allocator ----------------
typedef struct {
    unsigned char *base;
    size_t cap;
    size_t off;
} Arena;

static size_t a_align_up(size_t x, size_t a) { return (x + a - 1) & ~(a - 1); }

static void arena_init(Arena *a, size_t cap)
{
    a->base = (unsigned char*)malloc(cap);
    if (!a->base) die("arena malloc failed");
    a->cap = cap;
    a->off = 0;
}

static void arena_destroy(Arena *a)
{
    free(a->base);
    a->base = NULL;
    a->cap = a->off = 0;
}

static void *arena_alloc(Arena *a, size_t n, size_t align)
{
    size_t off = a_align_up(a->off, align);
    if (off + n > a->cap) die("arena out of memory");
    void *p = a->base + off;
    a->off = off + n;
    return p;
}

static void *arena_alloc0(Arena *a, size_t n, size_t align)
{
    void *p = arena_alloc(a, n, align);
    memset(p, 0, n);
    return p;
}

static char *arena_strdup(Arena *a, const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char*)arena_alloc(a, n, 1);
    memcpy(p, s, n);
    return p;
}

// Allocate space for a slice and copy it (when we need permanent storage)
static char *arena_slice_dup(Arena *a, StrSlice s)
{
    char *p = (char*)arena_alloc(a, s.len + 1, 1);
    memcpy(p, s.ptr, s.len);
    p[s.len] = '\0';
    return p;
}

/* allocate new + memcpy old bytes (replacement for realloc growth) */
static void *arena_grow(Arena *a, void *old, size_t old_bytes, size_t new_bytes, size_t align)
{
    void *p = arena_alloc(a, new_bytes, align);
    if (old && old_bytes) memcpy(p, old, old_bytes);
    return p;
}

/* mark/reset for temporary allocations */
static size_t arena_mark(Arena *a) { return a->off; }
static void arena_reset(Arena *a, size_t mark) { a->off = mark; }

// Two arenas: permanent (parse tree, headers) and temporary (flattening)
static Arena A_perm;
static Arena A_tmp;

// Convenience wrappers (maintained for consistency with memory_opt)
static void *xmalloc(size_t n)
{
    return arena_alloc(&A_perm, n, 16);
}

static char *xstrdup(const char *s)
{
    return arena_strdup(&A_perm, s);
}

// ---------------- Reusable String Buffer ----------------
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} StrBuf;

static void strbuf_init(StrBuf *sb, size_t initial_cap)
{
    sb->cap = initial_cap;
    sb->data = (char*)malloc(initial_cap);
    if (!sb->data) die("strbuf malloc failed");
    sb->len = 0;
    sb->data[0] = '\0';
}

static void strbuf_destroy(StrBuf *sb)
{
    free(sb->data);
    sb->data = NULL;
    sb->len = sb->cap = 0;
}

static void strbuf_reset(StrBuf *sb)
{
    sb->len = 0;
    if (sb->data) sb->data[0] = '\0';
}

static void strbuf_ensure(StrBuf *sb, size_t needed)
{
    if (sb->len + needed + 1 < sb->cap) return;
    
    size_t newcap = sb->cap;
    while (newcap < sb->len + needed + 1) {
        newcap = newcap ? newcap * 2 : 64;
    }
    
    char *newdata = (char*)realloc(sb->data, newcap);
    if (!newdata) die("strbuf realloc failed");
    sb->data = newdata;
    sb->cap = newcap;
}

static void strbuf_push(StrBuf *sb, char ch)
{
    strbuf_ensure(sb, 1);
    sb->data[sb->len++] = ch;
    sb->data[sb->len] = '\0';
}

static void strbuf_append(StrBuf *sb, const char *s, size_t len)
{
    strbuf_ensure(sb, len);
    memcpy(sb->data + sb->len, s, len);
    sb->len += len;
    sb->data[sb->len] = '\0';
}

static void strbuf_append_cstr(StrBuf *sb, const char *s)
{
    strbuf_append(sb, s, strlen(s));
}

static void strbuf_append_slice(StrBuf *sb, StrSlice s)
{
    strbuf_append(sb, s.ptr, s.len);
}

static StrSlice strbuf_slice(const StrBuf *sb)
{
    return slice_make(sb->data, sb->len);
}

// Global reusable buffers for temporary operations
static StrBuf G_tmpbuf1;
static StrBuf G_tmpbuf2;

// ---------------- JSON tree (using StrSlice) ----------------

typedef enum
{
    J_NULL,
    J_BOOL,
    J_NUMBER,
    J_STRING,
    J_ARRAY,
    J_OBJECT
} JType;

typedef struct JValue JValue;

typedef struct
{
    StrSlice key;
    JValue *value;
} JMember;

struct JValue
{
    JType type;
    union
    {
        int boolean;
        StrSlice number; // store as slice
        StrSlice string; // store as slice
        struct
        {
            JValue **items;
            size_t len, cap;
        } array;
        struct
        {
            JMember *members;
            size_t len, cap;
        } object;
    } as;
};

static JValue *jnew(JType t)
{
    JValue *v = (JValue *)arena_alloc0(&A_perm, sizeof(JValue), _Alignof(JValue));
    v->type = t;
    return v;
}

static void jarray_push(JValue *arr, JValue *item)
{
    if (arr->type != J_ARRAY)
        die("internal: not array");

    if (arr->as.array.len == arr->as.array.cap)
    {
        size_t oldcap = arr->as.array.cap;
        size_t newcap = oldcap ? oldcap * 2 : 8;

        arr->as.array.items = (JValue **)arena_grow(
            &A_perm,
            arr->as.array.items,
            oldcap * sizeof(JValue *),
            newcap * sizeof(JValue *),
            _Alignof(JValue *)
        );
        arr->as.array.cap = newcap;
    }
    arr->as.array.items[arr->as.array.len++] = item;
}

static void jobject_add(JValue *obj, StrSlice key, JValue *value)
{
    if (obj->type != J_OBJECT)
        die("internal: not object");

    if (obj->as.object.len == obj->as.object.cap)
    {
        size_t oldcap = obj->as.object.cap;
        size_t newcap = oldcap ? oldcap * 2 : 8;

        obj->as.object.members = (JMember *)arena_grow(
            &A_perm,
            obj->as.object.members,
            oldcap * sizeof(JMember),
            newcap * sizeof(JMember),
            _Alignof(JMember)
        );
        obj->as.object.cap = newcap;
    }

    obj->as.object.members[obj->as.object.len].key = key;
    obj->as.object.members[obj->as.object.len].value = value;
    obj->as.object.len++;
}

static void jfree(JValue *v)
{
    (void)v;
}

// ---------------- Parser with string slicing ----------------

typedef struct
{
    const char *input;  // entire input buffer
    size_t pos;         // current position
    size_t len;         // total length
} Parser;

static int p_peek(Parser *p)
{
    if (p->pos >= p->len) return EOF;
    return (unsigned char)p->input[p->pos];
}

static void p_next(Parser *p)
{
    if (p->pos < p->len) p->pos++;
}

static void p_init(Parser *p, const char *input, size_t len)
{
    p->input = input;
    p->pos = 0;
    p->len = len;
}

static void p_skip_ws(Parser *p)
{
    while (p->pos < p->len && isspace((unsigned char)p->input[p->pos]))
        p->pos++;
}

static void p_expect(Parser *p, int ch)
{
    if (p_peek(p) != ch)
        die("unexpected character");
    p_next(p);
}

static int hexval(int ch)
{
    if ('0' <= ch && ch <= '9')
        return ch - '0';
    if ('a' <= ch && ch <= 'f')
        return 10 + (ch - 'a');
    if ('A' <= ch && ch <= 'F')
        return 10 + (ch - 'A');
    return -1;
}

// Parse string and return as slice (if no escapes) or allocated (if escapes)
static StrSlice parse_string(Parser *p, StrBuf *temp)
{
    p_expect(p, '"');
    
    size_t start = p->pos;
    int has_escape = 0;
    
    // Fast path: scan for end, checking for escapes
    while (p->pos < p->len && p->input[p->pos] != '"') {
        if (p->input[p->pos] == '\\') {
            has_escape = 1;
            break;
        }
        p->pos++;
    }
    
    if (!has_escape && p->pos < p->len && p->input[p->pos] == '"') {
        // No escapes: return slice directly into input
        StrSlice result = slice_make(p->input + start, p->pos - start);
        p_next(p); // skip closing "
        return result;
    }
    
    // Slow path: has escapes, need to decode into buffer
    strbuf_reset(temp);
    p->pos = start; // reset to beginning
    
    while (p_peek(p) != EOF && p_peek(p) != '"')
    {
        int ch = p_peek(p);
        if (ch == '\\')
        {
            p_next(p);
            if (p_peek(p) == EOF)
                die("bad escape");
            switch (p_peek(p))
            {
            case '"':
                strbuf_push(temp, '"');
                break;
            case '\\':
                strbuf_push(temp, '\\');
                break;
            case '/':
                strbuf_push(temp, '/');
                break;
            case 'b':
                strbuf_push(temp, '\b');
                break;
            case 'f':
                strbuf_push(temp, '\f');
                break;
            case 'n':
                strbuf_push(temp, '\n');
                break;
            case 'r':
                strbuf_push(temp, '\r');
                break;
            case 't':
                strbuf_push(temp, '\t');
                break;
            case 'u':
            {
                int v = 0;
                for (int i = 0; i < 4; i++)
                {
                    p_next(p);
                    int hv = hexval(p_peek(p));
                    if (hv < 0)
                        die("bad \\u escape");
                    v = (v << 4) | hv;
                }
                if (v <= 0x7F)
                    strbuf_push(temp, (char)v);
                else
                    strbuf_push(temp, '?');
                break;
            }
            default:
                die("unknown escape");
            }
            p_next(p);
        }
        else
        {
            strbuf_push(temp, (char)ch);
            p_next(p);
        }
    }
    p_expect(p, '"');
    
    // Copy from temp buffer to arena
    return slice_make(arena_slice_dup(&A_perm, strbuf_slice(temp)), temp->len);
}

static StrSlice parse_number(Parser *p)
{
    size_t start = p->pos;
    
    if (p_peek(p) == '-') p_next(p);
    
    if (!isdigit(p_peek(p)))
        die("bad number");
        
    if (p_peek(p) == '0')
    {
        p_next(p);
    }
    else
    {
        while (p_peek(p) != EOF && isdigit(p_peek(p)))
            p_next(p);
    }
    
    if (p_peek(p) == '.')
    {
        p_next(p);
        if (!isdigit(p_peek(p)))
            die("bad number fraction");
        while (p_peek(p) != EOF && isdigit(p_peek(p)))
            p_next(p);
    }
    
    if (p_peek(p) == 'e' || p_peek(p) == 'E')
    {
        p_next(p);
        if (p_peek(p) == '+' || p_peek(p) == '-')
            p_next(p);
        if (!isdigit(p_peek(p)))
            die("bad number exponent");
        while (p_peek(p) != EOF && isdigit(p_peek(p)))
            p_next(p);
    }
    
    // Return slice directly into input buffer
    return slice_make(p->input + start, p->pos - start);
}

static int p_match_kw(Parser *p, const char *kw, size_t kwlen)
{
    if (p->pos + kwlen > p->len)
        return 0;
    if (memcmp(p->input + p->pos, kw, kwlen) != 0)
        return 0;
    p->pos += kwlen;
    return 1;
}

static JValue *parse_value(Parser *p, StrBuf *temp);

static JValue *parse_array(Parser *p, StrBuf *temp)
{
    p_expect(p, '[');
    p_skip_ws(p);
    
    JValue *arr = jnew(J_ARRAY);
    
    if (p_peek(p) == ']')
    {
        p_next(p);
        return arr;
    }
    
    while (1)
    {
        p_skip_ws(p);
        JValue *item = parse_value(p, temp);
        jarray_push(arr, item);
        p_skip_ws(p);
        
        if (p_peek(p) == ',')
        {
            p_next(p);
            continue;
        }
        if (p_peek(p) == ']')
        {
            p_next(p);
            break;
        }
        die("bad array syntax");
    }
    
    return arr;
}

static JValue *parse_object(Parser *p, StrBuf *temp)
{
    p_expect(p, '{');
    p_skip_ws(p);
    
    JValue *obj = jnew(J_OBJECT);
    
    if (p_peek(p) == '}')
    {
        p_next(p);
        return obj;
    }
    
    while (1)
    {
        p_skip_ws(p);
        if (p_peek(p) != '"')
            die("object key must be string");
        
        StrSlice key = parse_string(p, temp);
        p_skip_ws(p);
        p_expect(p, ':');
        p_skip_ws(p);
        
        JValue *value = parse_value(p, temp);
        jobject_add(obj, key, value);
        
        p_skip_ws(p);
        if (p_peek(p) == ',')
        {
            p_next(p);
            continue;
        }
        if (p_peek(p) == '}')
        {
            p_next(p);
            break;
        }
        die("bad object syntax");
    }
    
    return obj;
}

static JValue *parse_value(Parser *p, StrBuf *temp)
{
    p_skip_ws(p);
    int c = p_peek(p);
    
    if (c == EOF)
        die("unexpected EOF");
    if (c == '"')
    {
        JValue *v = jnew(J_STRING);
        v->as.string = parse_string(p, temp);
        return v;
    }
    if (c == '{')
        return parse_object(p, temp);
    if (c == '[')
        return parse_array(p, temp);
    if (c == 't')
    {
        if (!p_match_kw(p, "true", 4))
            die("bad token");
        JValue *v = jnew(J_BOOL);
        v->as.boolean = 1;
        return v;
    }
    if (c == 'f')
    {
        if (!p_match_kw(p, "false", 5))
            die("bad token");
        JValue *v = jnew(J_BOOL);
        v->as.boolean = 0;
        return v;
    }
    if (c == 'n')
    {
        if (!p_match_kw(p, "null", 4))
            die("bad token");
        return jnew(J_NULL);
    }
    if (c == '-' || isdigit(c))
    {
        JValue *v = jnew(J_NUMBER);
        v->as.number = parse_number(p);
        return v;
    }
    
    die("unknown value");
    return NULL;
}

// --------------- Flattening to key/value pairs (using slices) ---------------

typedef struct
{
    StrSlice key;
    StrSlice val; // already stringified for CSV cell
} KV;

typedef struct
{
    KV *items;
    size_t len, cap;
} KVList;

static void kv_push(KVList *l, StrSlice k, StrSlice v)
{
    if (l->len == l->cap)
    {
        size_t oldcap = l->cap;
        size_t newcap = oldcap ? oldcap * 2 : 16;

        l->items = (KV *)arena_grow(
            &A_tmp,
            l->items,
            oldcap * sizeof(KV),
            newcap * sizeof(KV),
            _Alignof(KV)
        );
        l->cap = newcap;
    }
    l->items[l->len].key = k;
    l->items[l->len].val = v;
    l->len++;
}

static StrSlice slice_primitive(const JValue *v)
{
    switch (v->type)
    {
    case J_NULL:
        return slice_from_cstr("null");
    case J_BOOL:
        return slice_from_cstr(v->as.boolean ? "true" : "false");
    case J_NUMBER:
        return v->as.number;
    case J_STRING:
        return v->as.string;
    default:
        return slice_from_cstr("[complex]");
    }
}

static int array_is_all_primitives(const JValue *arr)
{
    for (size_t i = 0; i < arr->as.array.len; i++)
    {
        JType t = arr->as.array.items[i]->type;
        if (!(t == J_NULL || t == J_BOOL || t == J_NUMBER || t == J_STRING))
            return 0;
    }
    return 1;
}

static StrSlice join_array_primitives(const JValue *arr, StrBuf *temp)
{
    strbuf_reset(temp);
    
    for (size_t i = 0; i < arr->as.array.len; i++)
    {
        if (i > 0) strbuf_push(temp, ';');
        StrSlice s = slice_primitive(arr->as.array.items[i]);
        strbuf_append_slice(temp, s);
    }
    
    // Copy to arena for permanence
    return slice_make(arena_slice_dup(&A_tmp, strbuf_slice(temp)), temp->len);
}

static void flatten_value(const JValue *v, StrSlice prefix, KVList *out, StrBuf *temp);

static StrSlice make_key(StrSlice prefix, StrSlice k, StrBuf *temp)
{
    if (prefix.len == 0)
    {
        // Just duplicate k into arena
        return slice_make(arena_slice_dup(&A_tmp, k), k.len);
    }
    
    strbuf_reset(temp);
    strbuf_append_slice(temp, prefix);
    strbuf_push(temp, '.');
    strbuf_append_slice(temp, k);
    
    return slice_make(arena_slice_dup(&A_tmp, strbuf_slice(temp)), temp->len);
}

static void flatten_object(const JValue *obj, StrSlice prefix, KVList *out, StrBuf *temp)
{
    for (size_t i = 0; i < obj->as.object.len; i++)
    {
        StrSlice k = obj->as.object.members[i].key;
        const JValue *val = obj->as.object.members[i].value;
        StrSlice nk = make_key(prefix, k, temp);
        flatten_value(val, nk, out, temp);
    }
}

static StrSlice json_array_to_string(const JValue *arr, StrBuf *temp);

static void json_print_value(const JValue *v, StrBuf *sb)
{
    switch (v->type)
    {
    case J_NULL:
        strbuf_append_cstr(sb, "null");
        break;
    case J_BOOL:
        strbuf_append_cstr(sb, v->as.boolean ? "true" : "false");
        break;
    case J_NUMBER:
        strbuf_append_slice(sb, v->as.number);
        break;
    case J_STRING:
        strbuf_push(sb, '"');
        strbuf_append_slice(sb, v->as.string);
        strbuf_push(sb, '"');
        break;
    case J_OBJECT:
        strbuf_append_cstr(sb, "{...}");
        break;
    case J_ARRAY:
        strbuf_append_cstr(sb, "[...]");
        break;
    }
}

static StrSlice json_array_to_string(const JValue *arr, StrBuf *temp)
{
    strbuf_reset(temp);
    strbuf_push(temp, '[');
    
    for (size_t i = 0; i < arr->as.array.len; i++)
    {
        if (i) strbuf_push(temp, ',');
        json_print_value(arr->as.array.items[i], temp);
    }
    
    strbuf_push(temp, ']');
    return slice_make(arena_slice_dup(&A_tmp, strbuf_slice(temp)), temp->len);
}

static void flatten_value(const JValue *v, StrSlice prefix, KVList *out, StrBuf *temp)
{
    if (v->type == J_OBJECT)
    {
        flatten_object(v, prefix, out, temp);
        return;
    }
    if (v->type == J_ARRAY)
    {
        if (array_is_all_primitives(v))
        {
            StrSlice joined = join_array_primitives(v, temp);
            kv_push(out, prefix, joined);
        }
        else
        {
            StrSlice s = json_array_to_string(v, temp);
            kv_push(out, prefix, s);
        }
        return;
    }
    // primitive
    kv_push(out, prefix, slice_primitive(v));
}

static void kvlist_free(KVList *l)
{
    (void)l;
}

// --------------- Header collection (using slices) ---------------

typedef struct
{
    StrSlice *keys;
    size_t len, cap;
} KeySet;

static int keyset_contains(const KeySet *s, StrSlice k)
{
    for (size_t i = 0; i < s->len; i++)
    {
        if (slice_eq(s->keys[i], k))
            return 1;
    }
    return 0;
}

static void keyset_add(KeySet *s, StrSlice k)
{
    if (keyset_contains(s, k))
        return;

    if (s->len == s->cap)
    {
        size_t oldcap = s->cap;
        size_t newcap = oldcap ? oldcap * 2 : 32;

        s->keys = (StrSlice *)arena_grow(
            &A_perm,
            s->keys,
            oldcap * sizeof(StrSlice),
            newcap * sizeof(StrSlice),
            _Alignof(StrSlice)
        );
        s->cap = newcap;
    }
    // stored headers must survive to end => permanent arena
    s->keys[s->len++] = slice_make(arena_slice_dup(&A_perm, k), k.len);
}

static void keyset_free(KeySet *s)
{
    (void)s;
}

// --------------- CSV writer (using slices) ---------------

static void csv_write_slice(FILE *out, StrSlice s)
{
    // Check if needs quoting
    int need_quote = 0;
    for (size_t i = 0; i < s.len; i++)
    {
        char c = s.ptr[i];
        if (c == ',' || c == '"' || c == '\n' || c == '\r')
        {
            need_quote = 1;
            break;
        }
    }
    
    if (!need_quote)
    {
        fwrite(s.ptr, 1, s.len, out);
        return;
    }
    
    fputc('"', out);
    for (size_t i = 0; i < s.len; i++)
    {
        if (s.ptr[i] == '"')
            fputc('"', out); // escape by doubling
        fputc(s.ptr[i], out);
    }
    fputc('"', out);
}

static StrSlice kv_get(const KVList *l, StrSlice key)
{
    for (size_t i = 0; i < l->len; i++)
    {
        if (slice_eq(l->items[i].key, key))
            return l->items[i].val;
    }
    return slice_from_cstr(""); // missing becomes empty cell
}

// --------------- Top-level parsing ---------------

typedef struct
{
    JValue **objs;
    size_t len, cap;
} ObjList;

static void objlist_push(ObjList *ol, JValue *obj)
{
    if (ol->len == ol->cap)
    {
        size_t oldcap = ol->cap;
        size_t newcap = oldcap ? oldcap * 2 : 16;

        ol->objs = (JValue **)arena_grow(
            &A_perm,
            ol->objs,
            oldcap * sizeof(JValue *),
            newcap * sizeof(JValue *),
            _Alignof(JValue *)
        );
        ol->cap = newcap;
    }
    ol->objs[ol->len++] = obj;
}

static void objlist_free(ObjList *ol)
{
    (void)ol;
}

static ObjList parse_top(const char *input, size_t len, StrBuf *temp)
{
    Parser p;
    p_init(&p, input, len);
    p_skip_ws(&p);

    ObjList ol = (ObjList){0};

    JValue *top = parse_value(&p, temp);
    p_skip_ws(&p);

    if (top->type == J_OBJECT)
    {
        objlist_push(&ol, top);
        return ol;
    }
    if (top->type == J_ARRAY)
    {
        for (size_t i = 0; i < top->as.array.len; i++)
        {
            JValue *it = top->as.array.items[i];
            if (it->type != J_OBJECT)
                die("top array must contain objects");
            objlist_push(&ol, it);
        }
        return ol;
    }

    die("top-level JSON must be object or array of objects");
    return ol;
}

// --------------- File reading (single allocation) ---------------

typedef struct {
    char *data;
    size_t len;
    int is_mmap;
} FileBuffer;

static FileBuffer read_entire_file(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) die("cannot open input file");
    
    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        die("cannot stat input file");
    }
    
    FileBuffer fb = {0};
    fb.len = (size_t)st.st_size;
    
    // Try mmap first for large files
    if (fb.len > 4096) {
        fb.data = (char*)mmap(NULL, fb.len, PROT_READ, MAP_PRIVATE, fd, 0);
        if (fb.data != MAP_FAILED) {
            fb.is_mmap = 1;
            close(fd);
            return fb;
        }
    }
    
    // Fallback to read
    fb.data = (char*)malloc(fb.len + 1);
    if (!fb.data) {
        close(fd);
        die("cannot allocate file buffer");
    }
    
    size_t total = 0;
    while (total < fb.len) {
        ssize_t n = read(fd, fb.data + total, fb.len - total);
        if (n <= 0) {
            free(fb.data);
            close(fd);
            die("read failed");
        }
        total += n;
    }
    fb.data[fb.len] = '\0';
    fb.is_mmap = 0;
    close(fd);
    return fb;
}

static void file_buffer_free(FileBuffer *fb)
{
    if (fb->is_mmap) {
        munmap(fb->data, fb->len);
    } else {
        free(fb->data);
    }
    fb->data = NULL;
    fb->len = 0;
}

// --------------- Main ---------------

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        fprintf(stderr, "Usage: %s input.json > out.csv\n", argv[0]);
        return 2;
    }
    
    const char *path = argv[1];
    
    // Read entire file into memory
    FileBuffer input = read_entire_file(path);
    
    // Size arenas based on input size
    size_t perm_cap = input.len * 16 + (64u << 20);
    size_t tmp_cap  = input.len * 2 + (32u << 20);
    
    arena_init(&A_perm, perm_cap);
    arena_init(&A_tmp, tmp_cap);
    
    // Initialize reusable buffers
    strbuf_init(&G_tmpbuf1, 4096);
    strbuf_init(&G_tmpbuf2, 4096);
    
    // Parse using string slices
    ObjList objs = parse_top(input.data, input.len, &G_tmpbuf1);
    
    // Pass 1: collect headers
    KeySet headers = (KeySet){0};
    for (size_t i = 0; i < objs.len; i++)
    {
        size_t mark = arena_mark(&A_tmp);
        
        KVList kv = (KVList){0};
        flatten_object(objs.objs[i], slice_make("", 0), &kv, &G_tmpbuf1);
        for (size_t j = 0; j < kv.len; j++)
            keyset_add(&headers, kv.items[j].key);
        
        arena_reset(&A_tmp, mark);
    }
    
    // Print header row
    for (size_t i = 0; i < headers.len; i++)
    {
        if (i)
            fputc(',', stdout);
        csv_write_slice(stdout, headers.keys[i]);
    }
    fputc('\n', stdout);
    
    // Pass 2: output rows
    for (size_t i = 0; i < objs.len; i++)
    {
        size_t mark = arena_mark(&A_tmp);
        
        KVList kv = (KVList){0};
        flatten_object(objs.objs[i], slice_make("", 0), &kv, &G_tmpbuf1);
        for (size_t c = 0; c < headers.len; c++)
        {
            if (c)
                fputc(',', stdout);
            StrSlice val = kv_get(&kv, headers.keys[c]);
            csv_write_slice(stdout, val);
        }
        fputc('\n', stdout);
        
        arena_reset(&A_tmp, mark);
    }
    
    // Cleanup
    strbuf_destroy(&G_tmpbuf1);
    strbuf_destroy(&G_tmpbuf2);
    arena_destroy(&A_tmp);
    arena_destroy(&A_perm);
    file_buffer_free(&input);
    
    return 0;
}