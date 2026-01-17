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

static inline void p_skip_ws(Parser *p)
{
    // Inline to reduce function call overhead (called 12+ times)
    while (p->c == ' ' || p->c == '\t' || p->c == '\n' || p->c == '\r')
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
        *cap = *cap ? (*cap * 2) : 64;
        *buf = (char *)xrealloc(*buf, *cap);
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
        l->cap = l->cap ? l->cap * 2 : 16;
        l->items = (KV *)xrealloc(l->items, l->cap * sizeof(KV));
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
    for (size_t i = 0; i < l->len; i++)
    {
        free(l->items[i].key);
        free(l->items[i].val);
    }
    free(l->items);
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
        // ensure each element is object
        for (size_t i = 0; i < top->as.array.len; i++)
        {
            JValue *it = top->as.array.items[i];
            if (it->type != J_OBJECT)
                die("top array must contain objects");
            objlist_push(&ol, it);
        }
        // free container array node but not elements (we moved pointers)
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
    return 0;
}
