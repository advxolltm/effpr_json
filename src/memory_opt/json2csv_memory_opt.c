// json2csv_memory_opt.c
// Memory Behavior & Invariant-Based Optimizations
// Based on json2csv_baseline.c with progressive optimization techniques
//
// Applied Optimizations:
// [X] Arena Allocator - replaces malloc/free with bump allocator
// [ ] String Slicing - zero-copy string handling
// [ ] Linearized Structures - improved cache locality
// [ ] restrict Keyword - enable compiler optimizations
//
// Current Status: Arena allocator implemented
// Expected improvement: -30-40% runtime, -90% page faults, -60% peak memory

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include "arena.h"

// ARENA OPTIMIZATION: Global arena pointer
static Arena *g_arena = NULL;

static void die(const char *msg)
{
    fprintf(stderr, "ERROR: %s\n", msg);
    exit(1);
}

// ARENA OPTIMIZATION: xmalloc now uses arena
static void *xmalloc(size_t n)
{
    return arena_alloc(g_arena, n);
}

// ARENA OPTIMIZATION: xrealloc now uses arena (added old_size parameter)
static void *xrealloc(void *p, size_t old_size, size_t n)
{
    return arena_realloc(g_arena, p, old_size, n);
}

// ARENA OPTIMIZATION: xstrdup now uses arena
static char *xstrdup(const char *s)
{
    return arena_strdup(g_arena, s);
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
        char *number; // store as text (baseline)
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
        size_t old_cap = arr->as.array.cap; // ARENA: track old size
        arr->as.array.cap = arr->as.array.cap ? arr->as.array.cap * 2 : 8;
        arr->as.array.items = (JValue **)xrealloc(arr->as.array.items,
                                                  old_cap * sizeof(JValue *),
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
        size_t old_cap = obj->as.object.cap; // ARENA: track old size
        obj->as.object.cap = obj->as.object.cap ? obj->as.object.cap * 2 : 8;
        obj->as.object.members = (JMember *)xrealloc(obj->as.object.members,
                                                     old_cap * sizeof(JMember),
                                                     obj->as.object.cap * sizeof(JMember));
    }
    obj->as.object.members[obj->as.object.len].key = key_owned;
    obj->as.object.members[obj->as.object.len].value = value;
    obj->as.object.len++;
}

// ARENA OPTIMIZATION: jfree is now a no-op
static void jfree(JValue *v)
{
    // Arena allocator: all memory freed at once with arena_destroy()
    (void)v; // Suppress unused parameter warning
}

// ---------------- Simple parser ----------------

typedef struct
{
    FILE *f;
    int c; // current char or EOF
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

static void p_skip_ws(Parser *p)
{
    while (p->c != EOF && isspace((unsigned char)p->c))
        p_next(p);
}

static void p_expect(Parser *p, int ch)
{
    if (p->c != ch)
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

static void sb_push(char **buf, size_t *len, size_t *cap, char ch)
{
    if (*len + 1 >= *cap)
    {
        size_t old_cap = *cap; // ARENA: track old size
        *cap = *cap ? (*cap * 2) : 64;
        *buf = (char *)xrealloc(*buf, old_cap, *cap);
    }
    (*buf)[(*len)++] = ch;
    (*buf)[*len] = '\0';
}

static char *parse_string(Parser *p)
{
    // assumes current is '"'
    p_expect(p, '"');
    char *buf = NULL;
    size_t len = 0, cap = 0;
    sb_push(&buf, &len, &cap, '\0'); // ensure non-null and terminated
    len = 0;
    buf[0] = '\0';

    while (p->c != EOF && p->c != '"')
    {
        int ch = p->c;
        if (ch == '\\')
        {
            p_next(p);
            if (p->c == EOF)
                die("bad escape");
            switch (p->c)
            {
            case '"':
                sb_push(&buf, &len, &cap, '"');
                break;
            case '\\':
                sb_push(&buf, &len, &cap, '\\');
                break;
            case '/':
                sb_push(&buf, &len, &cap, '/');
                break;
            case 'b':
                sb_push(&buf, &len, &cap, '\b');
                break;
            case 'f':
                sb_push(&buf, &len, &cap, '\f');
                break;
            case 'n':
                sb_push(&buf, &len, &cap, '\n');
                break;
            case 'r':
                sb_push(&buf, &len, &cap, '\r');
                break;
            case 't':
                sb_push(&buf, &len, &cap, '\t');
                break;
            case 'u':
            {
                // Baseline: decode \uXXXX only for ASCII range <= 0x7F
                int v = 0;
                for (int i = 0; i < 4; i++)
                {
                    p_next(p);
                    int hv = hexval(p->c);
                    if (hv < 0)
                        die("bad \\u escape");
                    v = (v << 4) | hv;
                }
                if (v <= 0x7F)
                    sb_push(&buf, &len, &cap, (char)v);
                else
                    sb_push(&buf, &len, &cap, '?'); // baseline simplification
                break;
            }
            default:
                die("unknown escape");
            }
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

static char *parse_number_text(Parser *p)
{
    // baseline: store as raw text
    char *buf = NULL;
    size_t len = 0, cap = 0;
    sb_push(&buf, &len, &cap, '\0');
    len = 0;
    buf[0] = '\0';

    // JSON number: -? int frac? exp?
    if (p->c == '-')
    {
        sb_push(&buf, &len, &cap, (char)p->c);
        p_next(p);
    }
    if (!isdigit((unsigned char)p->c))
        die("bad number");
    if (p->c == '0')
    {
        sb_push(&buf, &len, &cap, '0');
        p_next(p);
    }
    else
    {
        while (isdigit((unsigned char)p->c))
        {
            sb_push(&buf, &len, &cap, (char)p->c);
            p_next(p);
        }
    }
    if (p->c == '.')
    {
        sb_push(&buf, &len, &cap, '.');
        p_next(p);
        if (!isdigit((unsigned char)p->c))
            die("bad number fraction");
        while (isdigit((unsigned char)p->c))
        {
            sb_push(&buf, &len, &cap, (char)p->c);
            p_next(p);
        }
    }
    if (p->c == 'e' || p->c == 'E')
    {
        sb_push(&buf, &len, &cap, (char)p->c);
        p_next(p);
        if (p->c == '+' || p->c == '-')
        {
            sb_push(&buf, &len, &cap, (char)p->c);
            p_next(p);
        }
        if (!isdigit((unsigned char)p->c))
            die("bad number exponent");
        while (isdigit((unsigned char)p->c))
        {
            sb_push(&buf, &len, &cap, (char)p->c);
            p_next(p);
        }
    }
    return buf;
}

static int p_match_kw(Parser *p, const char *kw)
{
    // Very simple keyword match: assumes current char matches first and reads ahead by fgetc/ungetc is annoying.
    // Instead we just compare as we advance.
    // Only used for "true", "false", "null".
    for (size_t i = 0; kw[i]; i++)
    {
        if (p->c != (unsigned char)kw[i])
            return 0;
        p_next(p);
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
    if (p->c == '{')
        return parse_object(p);
    if (p->c == '[')
        return parse_array(p);
    if (p->c == '"')
    {
        JValue *v = jnew(J_STRING);
        v->as.string = parse_string(p);
        return v;
    }
    if (p->c == '-' || isdigit((unsigned char)p->c))
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
    die("unexpected token");
    return NULL;
}

// ---------------- Flattener ----------------

typedef struct
{
    char *key;
    char *val;
} KVPair;

typedef struct
{
    KVPair *items;
    size_t len, cap;
} KVList;

static void kv_push(KVList *l, char *key, char *val)
{
    if (l->len == l->cap)
    {
        size_t old_cap = l->cap; // ARENA: track old size
        l->cap = l->cap ? l->cap * 2 : 16;
        l->items = (KVPair *)xrealloc(l->items, old_cap * sizeof(KVPair), l->cap * sizeof(KVPair));
    }
    l->items[l->len].key = key;
    l->items[l->len].val = val;
    l->len++;
}

static char *json_primitive_to_string(const JValue *v)
{
    switch (v->type)
    {
    case J_NULL:
        return xstrdup("");
    case J_BOOL:
        return xstrdup(v->as.boolean ? "true" : "false");
    case J_NUMBER:
        return xstrdup(v->as.number);
    case J_STRING:
        return xstrdup(v->as.string);
    default:
        return xstrdup("");
    }
}

static int array_is_all_primitives(const JValue *arr)
{
    for (size_t i = 0; i < arr->as.array.len; i++)
    {
        JType t = arr->as.array.items[i]->type;
        if (t == J_ARRAY || t == J_OBJECT)
            return 0;
    }
    return 1;
}

static char *join_array_primitives(const JValue *arr)
{
    char *buf = NULL;
    size_t len = 0, cap = 0;
    for (size_t i = 0; i < arr->as.array.len; i++)
    {
        char *s = json_primitive_to_string(arr->as.array.items[i]);
        size_t sl = strlen(s);
        while (len + sl + 2 >= cap)
        {
            size_t old_cap = cap; // ARENA: track old size
            cap = cap ? cap * 2 : 128;
            buf = (char *)xrealloc(buf, old_cap, cap);
        }
        if (i > 0)
            buf[len++] = ';';
        memcpy(buf + len, s, sl);
        len += sl;
        buf[len] = '\0';
        // ARENA: no free(s) - arena handles it
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
    for (size_t i = 0; i < obj->as.object.len; i++)
    {
        const char *k = obj->as.object.members[i].key;
        const JValue *val = obj->as.object.members[i].value;
        char *nk = make_key(prefix, k);
        flatten_value(val, nk, out);
        // ARENA: no free(nk) - arena handles it
    }
}

static void json_print_value(const JValue *v, char **buf, size_t *len, size_t *cap);

static void sb_app(char **b, size_t *l, size_t *c, const char *s)
{
    size_t n = strlen(s);
    while (*l + n + 1 >= *c)
    {
        size_t old_cap = *c; // ARENA: track old size
        *c = *c ? (*c * 2) : 128;
        *b = (char *)xrealloc(*b, old_cap, *c); // ARENA: use xrealloc instead of realloc
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
    for (size_t i = 0; i < arr->as.array.len; i++)
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
            // stringify complex array
            char *s = json_array_to_string(v);
            kv_push(out, xstrdup(prefix), s);
        }

        return;
    }
    // primitive
    kv_push(out, xstrdup(prefix), json_primitive_to_string(v));
}

// ARENA OPTIMIZATION: kvlist_free is now a no-op
static void kvlist_free(KVList *l)
{
    // Arena allocator: no individual frees needed
    l->items = NULL;
    l->len = l->cap = 0;
}

// --------------- Header collection (baseline linear set) ---------------

typedef struct
{
    char **keys;
    size_t len, cap;
} KeySet;

static int keyset_contains(const KeySet *s, const char *k)
{
    for (size_t i = 0; i < s->len; i++)
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
        size_t old_cap = s->cap; // ARENA: track old size
        s->cap = s->cap ? s->cap * 2 : 32;
        s->keys = (char **)xrealloc(s->keys, old_cap * sizeof(char *), s->cap * sizeof(char *));
    }
    s->keys[s->len++] = xstrdup(k);
}

// ARENA OPTIMIZATION: keyset_free is now a no-op
static void keyset_free(KeySet *s)
{
    // Arena allocator: no individual frees needed
    s->keys = NULL;
    s->len = s->cap = 0;
}

// --------------- CSV writer (simple) ---------------

static void csv_write_cell(FILE *out, const char *s)
{
    // Baseline: quote if contains comma/quote/newline
    int need_quote = 0;
    for (const char *p = s; *p; p++)
    {
        if (*p == ',' || *p == '"' || *p == '\n' || *p == '\r')
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
    for (const char *p = s; *p; p++)
    {
        if (*p == '"')
            fputc('"', out); // escape by doubling
        fputc(*p, out);
    }
    fputc('"', out);
}

static const char *kv_get(const KVList *l, const char *key)
{
    for (size_t i = 0; i < l->len; i++)
    {
        if (strcmp(l->items[i].key, key) == 0)
            return l->items[i].val;
    }
    return ""; // missing becomes empty cell
}

// --------------- Top-level parsing: object or array of objects ---------------

typedef struct
{
    JValue **objs;
    size_t len, cap;
} ObjList;

static void objlist_push(ObjList *ol, JValue *obj)
{
    if (ol->len == ol->cap)
    {
        size_t old_cap = ol->cap; // ARENA: track old size
        ol->cap = ol->cap ? ol->cap * 2 : 16;
        ol->objs = (JValue **)xrealloc(ol->objs, old_cap * sizeof(JValue *), ol->cap * sizeof(JValue *));
    }
    ol->objs[ol->len++] = obj;
}

// ARENA OPTIMIZATION: objlist_free is now a no-op
static void objlist_free(ObjList *ol)
{
    // Arena allocator: no individual frees needed
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
        // ensure each element is object
        for (size_t i = 0; i < top->as.array.len; i++)
        {
            JValue *it = top->as.array.items[i];
            if (it->type != J_OBJECT)
                die("top array must contain objects");
            objlist_push(&ol, it);
        }
        // ARENA: no need to free container - arena handles it
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
    
    // ARENA OPTIMIZATION: Create arena based on file size
    g_arena = arena_create(arena_estimate_size(path));
    
    FILE *f = fopen(path, "rb");
    if (!f)
        die("cannot open input file");

    ObjList objs = parse_top(f);
    fclose(f);

    // Pass 1: collect headers
    KeySet headers = {0};
    for (size_t i = 0; i < objs.len; i++)
    {
        KVList kv = {0};
        flatten_object(objs.objs[i], "", &kv);
        for (size_t j = 0; j < kv.len; j++)
            keyset_add(&headers, kv.items[j].key);
        kvlist_free(&kv);
    }

    // Print header row
    for (size_t i = 0; i < headers.len; i++)
    {
        if (i)
            fputc(',', stdout);
        csv_write_cell(stdout, headers.keys[i]);
    }
    fputc('\n', stdout);

    // Pass 2: output rows
    for (size_t i = 0; i < objs.len; i++)
    {
        KVList kv = {0};
        flatten_object(objs.objs[i], "", &kv);
        for (size_t c = 0; c < headers.len; c++)
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
    
    // ARENA OPTIMIZATION: Destroy arena
    arena_destroy(g_arena);
    
    return 0;
}