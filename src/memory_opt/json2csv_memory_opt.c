// json2csv_baseline.c
// Baseline JSON -> CSV transformer (V1): simple, readable, intentionally not optimized.
// - Builds full JSON tree with mallocs
// - Flattens objects into dotted keys
// - Two-pass: load all, collect headers, then output
// - Linear searches for key sets (O(n^2))
// Intended as a baseline for later optimization steps.

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

static void *xmalloc(size_t n)
{
    return arena_alloc(&A_perm, n, 16);
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
    return arena_strdup(&A_perm, s);
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

static void jobject_add(JValue *obj, char *key_owned, JValue *value)
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

    obj->as.object.members[obj->as.object.len].key = key_owned;
    obj->as.object.members[obj->as.object.len].value = value;
    obj->as.object.len++;
}

static void jfree(JValue *v)
{
    (void)v;
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
        size_t oldcap = *cap;
        size_t newcap = oldcap ? (oldcap * 2) : 64;

        *buf = (char *)arena_grow(&A_perm, *buf, oldcap, newcap, 1);
        *cap = newcap;
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
    die("unknown value");
    return NULL;
}

// --------------- Flattening to key/value pairs ---------------

typedef struct
{
    char *key;
    char *val; // already stringified for CSV cell
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

static char *json_primitive_to_string(const JValue *v)
{
    // Baseline: allocate a new string for each conversion
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
    for (size_t i = 0; i < arr->as.array.len; i++)
    {
        JType t = arr->as.array.items[i]->type;
        if (!(t == J_NULL || t == J_BOOL || t == J_NUMBER || t == J_STRING))
            return 0;
    }
    return 1;
}

static char *join_array_primitives(const JValue *arr)
{
    // join with ';'
    size_t cap = 128, len = 0;
    char *buf = (char *)xmalloc(cap);
    buf[0] = '\0';

    for (size_t i = 0; i < arr->as.array.len; i++)
    {
        char *s = json_primitive_to_string(arr->as.array.items[i]);
        size_t sl = strlen(s);
        // +1 for ; or null
        while (len + sl + 2 >= cap)
        {
            cap *= 2;
            buf = (char *)xrealloc(buf, cap);
        }
        if (i > 0)
            buf[len++] = ';';
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
        return arena_strdup(&A_tmp, k);

    size_t a = strlen(prefix), b = strlen(k);
    char *r = (char *)arena_alloc(&A_tmp, a + 1 + b + 1, 1);
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

static void kvlist_free(KVList *l)
{
    (void)l;
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
        size_t oldcap = s->cap;
        size_t newcap = oldcap ? oldcap * 2 : 32;

        s->keys = (char **)arena_grow(
            &A_perm,
            s->keys,
            oldcap * sizeof(char *),
            newcap * sizeof(char *),
            _Alignof(char *)
        );
        s->cap = newcap;
    }
    // stored headers must survive to end => permanent arena
    s->keys[s->len++] = arena_strdup(&A_perm, k);
}

static void keyset_free(KeySet *s)
{
    (void)s;
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

static ObjList parse_top(FILE *f)
{
    Parser p;
    p_init(&p, f);
    p_skip_ws(&p);

    ObjList ol = (ObjList){0};

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
        // No freeing: arena-owned
        return ol;
    }

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

    // Size arenas based on input size (heuristic)
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);

    size_t perm_cap = (fsz > 0 ? (size_t)fsz : 1) * 8 + (64u << 20); // +64MB headroom
    size_t tmp_cap  = (fsz > 0 ? (size_t)fsz : 1) * 2 + (32u << 20); // scratch

    arena_init(&A_perm, perm_cap);
    arena_init(&A_tmp, tmp_cap);

    ObjList objs = parse_top(f);
    fclose(f);

    // Pass 1: collect headers
    KeySet headers = (KeySet){0};
    for (size_t i = 0; i < objs.len; i++)
    {
        size_t mark = arena_mark(&A_tmp);

        KVList kv = (KVList){0};
        flatten_object(objs.objs[i], "", &kv);
        for (size_t j = 0; j < kv.len; j++)
            keyset_add(&headers, kv.items[j].key);

        arena_reset(&A_tmp, mark); // reclaim all temporary KV + key/value strings
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
        size_t mark = arena_mark(&A_tmp);

        KVList kv = (KVList){0};
        flatten_object(objs.objs[i], "", &kv);
        for (size_t c = 0; c < headers.len; c++)
        {
            if (c)
                fputc(',', stdout);
            const char *val = kv_get(&kv, headers.keys[c]);
            csv_write_cell(stdout, val ? val : "");
        }
        fputc('\n', stdout);

        arena_reset(&A_tmp, mark);
    }

    // All memory is arena-owned
    keyset_free(&headers);
    objlist_free(&objs);

    arena_destroy(&A_tmp);
    arena_destroy(&A_perm);
    return 0;
}

