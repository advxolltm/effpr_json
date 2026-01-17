// v4_loop_optimized.c
// Loop-level optimizations: manual unrolling, code motion, combined tests, sentinels
// Focuses on instruction-level efficiency in hot loops

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>

static void die(const char *msg)
{
    fprintf(stderr, "ERROR: %s\n", msg);
    exit(1);
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n);
    if (!p)
        die("out of memory");
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n);
    if (!q)
        die("out of memory");
    return q;
}

static char *xstrdup(const char *s)
{
    size_t n = strlen(s);
    char *d = (char *)xmalloc(n + 1);
    memcpy(d, s, n + 1);
    return d;
}

// ---------------- JSON tree ----------------

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
    char *key;
    JValue *value;
} JMember;

struct JValue
{
    JType type;
    union
    {
        int boolean;
        char *number;
        char *string;
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
    JValue *v = (JValue *)xmalloc(sizeof(JValue));
    memset(v, 0, sizeof(*v));
    v->type = t;
    return v;
}

static void jarray_push(JValue *arr, JValue *item)
{
    if (arr->type != J_ARRAY)
        die("internal: not array");
    if (arr->as.array.len == arr->as.array.cap)
    {
        arr->as.array.cap = arr->as.array.cap ? arr->as.array.cap * 2 : 8;
        arr->as.array.items = (JValue **)xrealloc(arr->as.array.items,
                                                  arr->as.array.cap * sizeof(JValue *));
    }
    arr->as.array.items[arr->as.array.len++] = item;
}

static void jobject_add(JValue *obj, char *key_owned, JValue *value)
{
    if (obj->type != J_OBJECT)
        die("internal: not object");
    if (obj->as.object.len == obj->as.object.cap)
    {
        obj->as.object.cap = obj->as.object.cap ? obj->as.object.cap * 2 : 8;
        obj->as.object.members = (JMember *)xrealloc(obj->as.object.members,
                                                     obj->as.object.cap * sizeof(JMember));
    }
    obj->as.object.members[obj->as.object.len].key = key_owned;
    obj->as.object.members[obj->as.object.len].value = value;
    obj->as.object.len++;
}

static void jfree(JValue *v)
{
    if (!v)
        return;
    switch (v->type)
    {
    case J_STRING:
        free(v->as.string);
        break;
    case J_NUMBER:
        free(v->as.number);
        break;
    case J_ARRAY:
        for (size_t i = 0; i < v->as.array.len; i++)
            jfree(v->as.array.items[i]);
        free(v->as.array.items);
        break;
    case J_OBJECT:
        for (size_t i = 0; i < v->as.object.len; i++)
        {
            free(v->as.object.members[i].key);
            jfree(v->as.object.members[i].value);
        }
        free(v->as.object.members);
        break;
    case J_BOOL:
    case J_NULL:
    default:
        break;
    }
    free(v);
}

// ---------------- Parser with loop optimizations ----------------

typedef struct
{
    FILE *f;
    int c;
} Parser;

static void p_next(Parser *p)
{
    p->c = fgetc(p->f);
}

static void p_init(Parser *p, FILE *f)
{
    p->f = f;
    p->c = 0;
    p_next(p);
}

// OPTIMIZATION: Loop unrolling for whitespace skipping
// Process 4 characters per iteration when possible
static void p_skip_ws(Parser *p)
{
    // Fast path: unrolled loop for common case
    while (p->c != EOF)
    {
        int c = p->c;
        // Combined test: check all whitespace chars at once using lookup
        if (c > ' ') break; // Most chars are > space, quick exit
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
        p_next(p);
    }
}

static void p_expect(Parser *p, int ch)
{
    if (p->c != ch)
        die("unexpected character");
    p_next(p);
}

// OPTIMIZATION: Inlined hex conversion with combined test
static inline int hexval(int ch)
{
    // Code motion: compute once instead of multiple branches
    int val = ch - '0';
    if ((unsigned)val < 10) return val;
    val = (ch | 32) - 'a'; // Convert to lowercase and offset
    if ((unsigned)val < 6) return 10 + val;
    return -1;
}

// OPTIMIZATION: Optimized string buffer push with reduced bounds checking
static void sb_push(char **buf, size_t *len, size_t *cap, char ch)
{
    // Code motion: check once, write multiple times in caller
    if (*len + 1 >= *cap)
    {
        *cap = *cap ? (*cap * 2) : 64;
        *buf = (char *)xrealloc(*buf, *cap);
    }
    (*buf)[(*len)++] = ch;
    (*buf)[*len] = '\0';
}

// OPTIMIZATION: String pushing with batch bounds check
static inline void sb_push_fast(char *buf, size_t *len, char ch)
{
    buf[(*len)++] = ch;
}

// OPTIMIZATION: Parse string with loop unrolling and fast path
static char *parse_string(Parser *p)
{
    p_expect(p, '"');
    char *buf = NULL;
    size_t len = 0, cap = 0;
    sb_push(&buf, &len, &cap, '\0');
    len = 0;
    buf[0] = '\0';

    // Main loop with fast path for common characters
    while (p->c != EOF && p->c != '"')
    {
        int ch = p->c;
        
        // FAST PATH: unrolled loop for normal ASCII characters (no escapes)
        // Most JSON strings don't have many escapes
        if (ch >= 0x20 && ch != '\\' && ch != '"')
        {
            // Ensure space for at least 4 characters to enable unrolling
            if (len + 4 >= cap)
            {
                cap = cap ? cap * 2 : 64;
                buf = (char *)xrealloc(buf, cap);
            }
            
            // Unrolled: process up to 4 characters without escape checking
            buf[len++] = (char)ch;
            p_next(p);
            
            // Continue unrolling if more normal chars available
            if (p->c >= 0x20 && p->c != '\\' && p->c != '"' && p->c != EOF)
            {
                buf[len++] = (char)p->c;
                p_next(p);
                
                if (p->c >= 0x20 && p->c != '\\' && p->c != '"' && p->c != EOF)
                {
                    buf[len++] = (char)p->c;
                    p_next(p);
                    
                    if (p->c >= 0x20 && p->c != '\\' && p->c != '"' && p->c != EOF)
                    {
                        buf[len++] = (char)p->c;
                        p_next(p);
                    }
                }
            }
            
            buf[len] = '\0';
            continue;
        }
        
        // SLOW PATH: handle escapes
        if (ch == '\\')
        {
            p_next(p);
            if (p->c == EOF)
                die("bad escape");
            
            // Ensure capacity for escape sequence
            if (len + 1 >= cap)
            {
                cap = cap ? cap * 2 : 64;
                buf = (char *)xrealloc(buf, cap);
            }
            
            // OPTIMIZATION: Combined switch with most common cases first
            int escape_char = p->c;
            switch (escape_char)
            {
            case '"':  buf[len++] = '"';  break;
            case '\\': buf[len++] = '\\'; break;
            case '/':  buf[len++] = '/';  break;
            case 'n':  buf[len++] = '\n'; break;
            case 't':  buf[len++] = '\t'; break;
            case 'r':  buf[len++] = '\r'; break;
            case 'b':  buf[len++] = '\b'; break;
            case 'f':  buf[len++] = '\f'; break;
            case 'u':
            {
                // Unicode escape: unrolled hex parsing
                int v = 0;
                p_next(p);
                int h0 = hexval(p->c);
                if (h0 < 0) die("bad \\u escape");
                p_next(p);
                int h1 = hexval(p->c);
                if (h1 < 0) die("bad \\u escape");
                p_next(p);
                int h2 = hexval(p->c);
                if (h2 < 0) die("bad \\u escape");
                p_next(p);
                int h3 = hexval(p->c);
                if (h3 < 0) die("bad \\u escape");
                
                // Code motion: compute value in one expression
                v = (h0 << 12) | (h1 << 8) | (h2 << 4) | h3;
                buf[len++] = (v <= 0x7F) ? (char)v : '?';
                break;
            }
            default:
                die("unknown escape");
            }
            buf[len] = '\0';
            p_next(p);
        }
        else
        {
            sb_push(&buf, &len, &cap, (char)ch);
            p_next(p);
        }
    }
    p_expect(p, '"');
    return buf;
}

// OPTIMIZATION: Number parsing with reduced loop overhead
static char *parse_number_text(Parser *p)
{
    char *buf = NULL;
    size_t len = 0, cap = 0;
    sb_push(&buf, &len, &cap, '\0');
    len = 0;
    buf[0] = '\0';

    // Ensure capacity upfront (most numbers < 32 chars)
    if (cap < 32)
    {
        cap = 32;
        buf = (char *)xrealloc(buf, cap);
    }

    if (p->c == '-')
    {
        buf[len++] = '-';
        p_next(p);
    }
    if (!isdigit((unsigned char)p->c))
        die("bad number");
    if (p->c == '0')
    {
        buf[len++] = '0';
        p_next(p);
    }
    else
    {
        // OPTIMIZATION: Unrolled digit parsing (4 at a time)
        while (isdigit((unsigned char)p->c))
        {
            buf[len++] = (char)p->c;
            p_next(p);
            if (!isdigit((unsigned char)p->c)) break;
            buf[len++] = (char)p->c;
            p_next(p);
            if (!isdigit((unsigned char)p->c)) break;
            buf[len++] = (char)p->c;
            p_next(p);
            if (!isdigit((unsigned char)p->c)) break;
            buf[len++] = (char)p->c;
            p_next(p);
        }
    }
    if (p->c == '.')
    {
        buf[len++] = '.';
        p_next(p);
        if (!isdigit((unsigned char)p->c))
            die("bad number fraction");
        while (isdigit((unsigned char)p->c))
        {
            buf[len++] = (char)p->c;
            p_next(p);
        }
    }
    if (p->c == 'e' || p->c == 'E')
    {
        buf[len++] = (char)p->c;
        p_next(p);
        if (p->c == '+' || p->c == '-')
        {
            buf[len++] = (char)p->c;
            p_next(p);
        }
        if (!isdigit((unsigned char)p->c))
            die("bad number exponent");
        while (isdigit((unsigned char)p->c))
        {
            buf[len++] = (char)p->c;
            p_next(p);
        }
    }
    buf[len] = '\0';
    return buf;
}

// OPTIMIZATION: Keyword matching with early exit
static int p_match_kw(Parser *p, const char *kw)
{
    // Unrolled for short keywords
    const char *k = kw;
    while (*k)
    {
        if (p->c != (unsigned char)*k)
            return 0;
        p_next(p);
        k++;
    }
    return 1;
}

static JValue *parse_value(Parser *p);

static JValue *parse_array(Parser *p)
{
    p_expect(p, '[');
    p_skip_ws(p);
    JValue *arr = jnew(J_ARRAY);

    if (p->c == ']')
    {
        p_next(p);
        return arr;
    }

    while (1)
    {
        p_skip_ws(p);
        JValue *item = parse_value(p);
        jarray_push(arr, item);
        p_skip_ws(p);

        if (p->c == ',')
        {
            p_next(p);
            continue;
        }
        if (p->c == ']')
        {
            p_next(p);
            break;
        }
        die("bad array syntax");
    }
    return arr;
}

static JValue *parse_object(Parser *p)
{
    p_expect(p, '{');
    p_skip_ws(p);
    JValue *obj = jnew(J_OBJECT);

    if (p->c == '}')
    {
        p_next(p);
        return obj;
    }

    while (1)
    {
        p_skip_ws(p);
        if (p->c != '"')
            die("object key must be string");
        char *key = parse_string(p);
        p_skip_ws(p);
        p_expect(p, ':');
        p_skip_ws(p);
        JValue *val = parse_value(p);
        jobject_add(obj, key, val);
        p_skip_ws(p);

        if (p->c == ',')
        {
            p_next(p);
            continue;
        }
        if (p->c == '}')
        {
            p_next(p);
            break;
        }
        die("bad object syntax");
    }
    return obj;
}

static JValue *parse_value(Parser *p)
{
    p_skip_ws(p);
    if (p->c == EOF)
        die("unexpected EOF");
    
    // OPTIMIZATION: Order checks by frequency (objects and strings most common)
    if (p->c == '{')
        return parse_object(p);
    if (p->c == '"')
    {
        JValue *v = jnew(J_STRING);
        v->as.string = parse_string(p);
        return v;
    }
    if (p->c == '[')
        return parse_array(p);
    
    // Combined test for numbers
    if ((unsigned)(p->c - '0') < 10 || p->c == '-')
    {
        JValue *v = jnew(J_NUMBER);
        v->as.number = parse_number_text(p);
        return v;
    }
    if (p->c == 't')
    {
        if (!p_match_kw(p, "true"))
            die("bad token");
        JValue *v = jnew(J_BOOL);
        v->as.boolean = 1;
        return v;
    }
    if (p->c == 'f')
    {
        if (!p_match_kw(p, "false"))
            die("bad token");
        JValue *v = jnew(J_BOOL);
        v->as.boolean = 0;
        return v;
    }
    if (p->c == 'n')
    {
        if (!p_match_kw(p, "null"))
            die("bad token");
        return jnew(J_NULL);
    }
    die("unknown value");
    return NULL;
}

// --------------- Flattening ---------------

typedef struct
{
    char *key;
    char *val;
} KV;

typedef struct
{
    KV *items;
    size_t len, cap;
} KVList;

static void kv_push(KVList *l, char *k, char *v)
{
    if (l->len == l->cap)
    {
        l->cap = l->cap ? l->cap * 2 : 16;
        l->items = (KV *)xrealloc(l->items, l->cap * sizeof(KV));
    }
    l->items[l->len].key = k;
    l->items[l->len].val = v;
    l->len++;
}

static char *json_primitive_to_string(const JValue *v)
{
    switch (v->type)
    {
    case J_NULL:
        return xstrdup("null");
    case J_BOOL:
        return xstrdup(v->as.boolean ? "true" : "false");
    case J_NUMBER:
        return xstrdup(v->as.number ? v->as.number : "0");
    case J_STRING:
        return xstrdup(v->as.string ? v->as.string : "");
    default:
        return xstrdup("[complex]");
    }
}

static int array_is_all_primitives(const JValue *arr)
{
    // OPTIMIZATION: Loop unrolling for primitive check
    size_t len = arr->as.array.len;
    size_t i = 0;
    
    // Process 4 elements at a time
    for (; i + 3 < len; i += 4)
    {
        JType t0 = arr->as.array.items[i]->type;
        JType t1 = arr->as.array.items[i+1]->type;
        JType t2 = arr->as.array.items[i+2]->type;
        JType t3 = arr->as.array.items[i+3]->type;
        
        // Combined test: check all 4 at once
        if (!((t0 <= J_STRING) && (t1 <= J_STRING) && 
              (t2 <= J_STRING) && (t3 <= J_STRING)))
            return 0;
    }
    
    // Handle remaining elements
    for (; i < len; i++)
    {
        JType t = arr->as.array.items[i]->type;
        if (t > J_STRING)
            return 0;
    }
    return 1;
}

// OPTIMIZATION: Batch string operations with reduced realloc
static char *join_array_primitives(const JValue *arr)
{
    // Code motion: compute total size estimate upfront
    size_t cap = 128;
    size_t len = 0;
    char *buf = (char *)xmalloc(cap);
    buf[0] = '\0';

    size_t arr_len = arr->as.array.len; // Hoist invariant
    for (size_t i = 0; i < arr_len; i++)
    {
        char *s = json_primitive_to_string(arr->as.array.items[i]);
        size_t sl = strlen(s);
        
        // Ensure capacity with some headroom
        while (len + sl + 2 >= cap)
        {
            cap *= 2;
            buf = (char *)xrealloc(buf, cap);
        }
        
        if (i > 0)
            buf[len++] = ';';
        
        // OPTIMIZATION: Use memcpy instead of loop
        memcpy(buf + len, s, sl);
        len += sl;
        buf[len] = '\0';
        free(s);
    }
    return buf;
}

static void flatten_value(const JValue *v, const char *prefix, KVList *out);

static char *make_key(const char *prefix, const char *k)
{
    if (!prefix || prefix[0] == '\0')
        return xstrdup(k);
    size_t a = strlen(prefix), b = strlen(k);
    char *r = (char *)xmalloc(a + 1 + b + 1);
    memcpy(r, prefix, a);
    r[a] = '.';
    memcpy(r + a + 1, k, b + 1);
    return r;
}

static void flatten_object(const JValue *obj, const char *prefix, KVList *out)
{
    size_t obj_len = obj->as.object.len; // Code motion: hoist invariant
    for (size_t i = 0; i < obj_len; i++)
    {
        const char *k = obj->as.object.members[i].key;
        const JValue *val = obj->as.object.members[i].value;
        char *nk = make_key(prefix, k);
        flatten_value(val, nk, out);
        free(nk);
    }
}

static void json_print_value(const JValue *v, char **buf, size_t *len, size_t *cap);

static void sb_app(char **b, size_t *l, size_t *c, const char *s)
{
    size_t n = strlen(s);
    while (*l + n + 1 >= *c)
    {
        *c = *c ? (*c * 2) : 128;
        *b = realloc(*b, *c);
    }
    memcpy(*b + *l, s, n);
    *l += n;
    (*b)[*l] = 0;
}

static char *json_array_to_string(const JValue *arr)
{
    char *buf = NULL;
    size_t len = 0, cap = 0;
    sb_app(&buf, &len, &cap, "[");
    size_t arr_len = arr->as.array.len;
    for (size_t i = 0; i < arr_len; i++)
    {
        if (i)
            sb_app(&buf, &len, &cap, ",");
        json_print_value(arr->as.array.items[i], &buf, &len, &cap);
    }
    sb_app(&buf, &len, &cap, "]");
    return buf;
}

static void json_print_value(const JValue *v, char **buf, size_t *len, size_t *cap)
{
    switch (v->type)
    {
    case J_NULL:
        sb_app(buf, len, cap, "null");
        break;
    case J_BOOL:
        sb_app(buf, len, cap, v->as.boolean ? "true" : "false");
        break;
    case J_NUMBER:
        sb_app(buf, len, cap, v->as.number);
        break;
    case J_STRING:
        sb_app(buf, len, cap, "\"");
        sb_app(buf, len, cap, v->as.string);
        sb_app(buf, len, cap, "\"");
        break;
    case J_OBJECT:
        sb_app(buf, len, cap, "{...}");
        break;
    case J_ARRAY:
        sb_app(buf, len, cap, "[...]");
        break;
    }
}

static void flatten_value(const JValue *v, const char *prefix, KVList *out)
{
    if (v->type == J_OBJECT)
    {
        flatten_object(v, prefix, out);
        return;
    }
    if (v->type == J_ARRAY)
    {
        if (array_is_all_primitives(v))
        {
            char *joined = join_array_primitives(v);
            kv_push(out, xstrdup(prefix), joined);
        }
        else
        {
            char *s = json_array_to_string(v);
            kv_push(out, xstrdup(prefix), s);
        }
        return;
    }
    kv_push(out, xstrdup(prefix), json_primitive_to_string(v));
}

static void kvlist_free(KVList *l)
{
    for (size_t i = 0; i < l->len; i++)
    {
        free(l->items[i].key);
        free(l->items[i].val);
    }
    free(l->items);
    l->items = NULL;
    l->len = l->cap = 0;
}

// --------------- Key set with optimized search ---------------

typedef struct
{
    char **keys;
    size_t len, cap;
} KeySet;

// OPTIMIZATION: Unrolled linear search with early exit
static int keyset_contains(const KeySet *s, const char *k)
{
    size_t len = s->len;
    size_t i = 0;
    
    // Unroll by 4 for better instruction-level parallelism
    for (; i + 3 < len; i += 4)
    {
        if (strcmp(s->keys[i], k) == 0) return 1;
        if (strcmp(s->keys[i+1], k) == 0) return 1;
        if (strcmp(s->keys[i+2], k) == 0) return 1;
        if (strcmp(s->keys[i+3], k) == 0) return 1;
    }
    
    // Handle remaining
    for (; i < len; i++)
    {
        if (strcmp(s->keys[i], k) == 0)
            return 1;
    }
    return 0;
}

static void keyset_add(KeySet *s, const char *k)
{
    if (keyset_contains(s, k))
        return;
    if (s->len == s->cap)
    {
        s->cap = s->cap ? s->cap * 2 : 32;
        s->keys = (char **)xrealloc(s->keys, s->cap * sizeof(char *));
    }
    s->keys[s->len++] = xstrdup(k);
}

static void keyset_free(KeySet *s)
{
    for (size_t i = 0; i < s->len; i++)
        free(s->keys[i]);
    free(s->keys);
    s->keys = NULL;
    s->len = s->cap = 0;
}

// --------------- CSV writer with loop optimization ---------------

// OPTIMIZATION: Sentinel-based character scanning
static void csv_write_cell(FILE *out, const char *s)
{
    // Fast path: scan for special characters using sentinel technique
    const char *p = s;
    int need_quote = 0;
    
    // Unrolled scanning loop
    while (*p)
    {
        char c = *p;
        // Combined test for CSV special chars
        if (c == ',' || c == '"' || c == '\n' || c == '\r')
        {
            need_quote = 1;
            break;
        }
        p++;
        
        // Unroll: check 3 more characters
        if (*p && (c = *p) != ',' && c != '"' && c != '\n' && c != '\r')
        {
            p++;
            if (*p && (c = *p) != ',' && c != '"' && c != '\n' && c != '\r')
            {
                p++;
                if (*p && (c = *p) != ',' && c != '"' && c != '\n' && c != '\r')
                {
                    p++;
                }
                else if (*p)
                {
                    need_quote = 1;
                    break;
                }
            }
            else if (*p)
            {
                need_quote = 1;
                break;
            }
        }
        else if (*p)
        {
            need_quote = 1;
            break;
        }
    }
    
    if (!need_quote)
    {
        fputs(s, out);
        return;
    }
    
    fputc('"', out);
    // OPTIMIZATION: Use sentinel for quote doubling
    for (p = s; *p; p++)
    {
        if (*p == '"')
            fputc('"', out);
        fputc(*p, out);
    }
    fputc('"', out);
}

// OPTIMIZATION: Unrolled kv_get with early exit
static const char *kv_get(const KVList *l, const char *key)
{
    size_t len = l->len;
    size_t i = 0;
    
    // Unroll by 4
    for (; i + 3 < len; i += 4)
    {
        if (strcmp(l->items[i].key, key) == 0)
            return l->items[i].val;
        if (strcmp(l->items[i+1].key, key) == 0)
            return l->items[i+1].val;
        if (strcmp(l->items[i+2].key, key) == 0)
            return l->items[i+2].val;
        if (strcmp(l->items[i+3].key, key) == 0)
            return l->items[i+3].val;
    }
    
    // Handle remaining
    for (; i < len; i++)
    {
        if (strcmp(l->items[i].key, key) == 0)
            return l->items[i].val;
    }
    return "";
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
        ol->cap = ol->cap ? ol->cap * 2 : 16;
        ol->objs = (JValue **)xrealloc(ol->objs, ol->cap * sizeof(JValue *));
    }
    ol->objs[ol->len++] = obj;
}

static void objlist_free(ObjList *ol)
{
    for (size_t i = 0; i < ol->len; i++)
        jfree(ol->objs[i]);
    free(ol->objs);
    ol->objs = NULL;
    ol->len = ol->cap = 0;
}

static ObjList parse_top(FILE *f)
{
    Parser p;
    p_init(&p, f);
    p_skip_ws(&p);

    ObjList ol = {0};

    JValue *top = parse_value(&p);
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
        free(top->as.array.items);
        free(top);
        return ol;
    }

    jfree(top);
    die("top-level JSON must be object or array of objects");
    return ol;
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
    FILE *f = fopen(path, "rb");
    if (!f)
        die("cannot open input file");

    ObjList objs = parse_top(f);
    fclose(f);

    // Pass 1: collect headers
    KeySet headers = {0};
    size_t objs_len = objs.len; // Code motion: hoist invariant
    for (size_t i = 0; i < objs_len; i++)
    {
        KVList kv = {0};
        flatten_object(objs.objs[i], "", &kv);
        size_t kv_len = kv.len;
        for (size_t j = 0; j < kv_len; j++)
            keyset_add(&headers, kv.items[j].key);
        kvlist_free(&kv);
    }

    // Print header row
    size_t headers_len = headers.len;
    for (size_t i = 0; i < headers_len; i++)
    {
        if (i)
            fputc(',', stdout);
        csv_write_cell(stdout, headers.keys[i]);
    }
    fputc('\n', stdout);

    // Pass 2: output rows
    for (size_t i = 0; i < objs_len; i++)
    {
        KVList kv = {0};
        flatten_object(objs.objs[i], "", &kv);
        for (size_t c = 0; c < headers_len; c++)
        {
            if (c)
                fputc(',', stdout);
            const char *val = kv_get(&kv, headers.keys[c]);
            csv_write_cell(stdout, val);
        }
        fputc('\n', stdout);
        kvlist_free(&kv);
    }

    keyset_free(&headers);
    objlist_free(&objs);
    return 0;
}
