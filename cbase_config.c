/*
 * cbase_config.c - Minimal key/value config parser.
 *
 * Flat namespace (no sections), '#' or ';' full-line comments, optional
 * double-quoted values with C-ish backslash escapes (\", \\, \n, \t, \r),
 * unquoted values get trailing '#'/';' comments stripped. Duplicate keys:
 * later wins. Keys are [A-Za-z_][A-Za-z0-9_.-]*.
 *
 * Storage: singly-linked list of entries in parse order; each entry owns a
 * null-terminated key buffer and a null-terminated value buffer, both
 * allocated via cb__alloc (arena or malloc). On duplicate key, the old
 * value buffer is freed and replaced while the entry keeps its position.
 * A tail pointer makes every append O(1); duplicate-key lookup is still
 * linear but bounded by the total unique-key count — acceptable for the
 * intended use (small config files, parsed once).
 *
 * parse_file caps file size at 16 MiB. Line length cap is 4096 bytes.
 *
 * Safety:
 *   - All cb__alloc return values are checked; allocation failure produces
 *     CB_INFO_CONFIG_ALLOC_FAILED with error_line == current 1-based line.
 *   - cb_config_destroy is safe on a failed parse (any partial state from
 *     a failed cb_config_parse is torn down before return, so head == NULL).
 *   - All string copies are explicit memcpy + '\0' terminator; no strncpy.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define CB__CONFIG_LINE_MAX      4096u
#define CB__CONFIG_FILE_MAX      (16u * 1024u * 1024u)

struct cb__config_entry_t
{
    cb__config_entry_t *next;
    char               *key;   /* null-terminated; cb__alloc'd */
    char               *value; /* null-terminated; cb__alloc'd */
};

/* --- Small helpers --- */

static bool cb__config_is_space(char c)
{
    /* Horizontal whitespace only — '\n' and '\r' are line terminators, never
       stripped as leading/trailing value whitespace. */
    return c == ' ' || c == '\t' || c == '\v' || c == '\f';
}

static bool cb__config_key_head_ok(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static bool cb__config_key_tail_ok(char c)
{
    if (cb__config_key_head_ok(c)) return true;
    if (c >= '0' && c <= '9')      return true;
    if (c == '.' || c == '-')      return true;
    return false;
}

static bool cb__config_key_valid(const char *s, size_t len)
{
    if (len == 0) return false;
    if (!cb__config_key_head_ok(s[0])) return false;
    for (size_t i = 1; i < len; ++i)
    {
        if (!cb__config_key_tail_ok(s[i])) return false;
    }
    return true;
}

static char cb__config_lower(char c)
{
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    return c;
}

static bool cb__config_ieq(const char *a, const char *b)
{
    /* a is the candidate (any case), b is the lowercase literal. Lengths
       must match exactly. */
    size_t i = 0;
    while (a[i] != '\0' && b[i] != '\0')
    {
        if (cb__config_lower(a[i]) != b[i]) return false;
        ++i;
    }
    return a[i] == '\0' && b[i] == '\0';
}

/* cb__alloc + memcpy + NUL; returns NULL on OOM. */
static char *cb__config_strndup(cb_arena_t *arena, const char *s, size_t n)
{
    char *out = (char *)cb__alloc(arena, n + 1u, 1u);
    if (!out) return NULL;
    if (n > 0) memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

/* --- Entry list management --- */

static cb__config_entry_t *cb__config_find_entry(const cb_config_t *cfg, const char *key)
{
    cb__config_entry_t *e = cfg->head;
    while (e)
    {
        if (strcmp(e->key, key) == 0) return e;
        e = e->next;
    }
    return NULL;
}

/* Insert-or-update. Returns CB_INFO_OK on success, CB_INFO_CONFIG_ALLOC_FAILED on OOM.
   On OOM the existing entry is left intact (old value preserved). */
static cb_info_t cb__config_set(cb_config_t *cfg,
                                const char *key_s, size_t key_len,
                                const char *val_s, size_t val_len)
{
    /* Look for an existing entry with this key. Keys in the linked list are
       already null-terminated, but key_s is a pointer into the input buffer
       — compare explicitly with lengths before reaching for strcmp. */
    cb__config_entry_t *existing = NULL;
    for (cb__config_entry_t *e = cfg->head; e; e = e->next)
    {
        size_t ek = strlen(e->key);
        if (ek == key_len && memcmp(e->key, key_s, key_len) == 0)
        {
            existing = e;
            break;
        }
    }

    char *new_value = cb__config_strndup(cfg->arena, val_s, val_len);
    if (!new_value) return CB_INFO_CONFIG_ALLOC_FAILED;

    if (existing)
    {
        cb__free(cfg->arena, existing->value);
        existing->value = new_value;
        return CB_INFO_OK;
    }

    char *new_key = cb__config_strndup(cfg->arena, key_s, key_len);
    if (!new_key)
    {
        cb__free(cfg->arena, new_value);
        return CB_INFO_CONFIG_ALLOC_FAILED;
    }

    cb__config_entry_t *entry = (cb__config_entry_t *)cb__alloc(
        cfg->arena, sizeof(cb__config_entry_t), sizeof(void *));
    if (!entry)
    {
        cb__free(cfg->arena, new_key);
        cb__free(cfg->arena, new_value);
        return CB_INFO_CONFIG_ALLOC_FAILED;
    }
    entry->next  = NULL;
    entry->key   = new_key;
    entry->value = new_value;

    /* Append in parse order via the tail pointer — O(1) per insert, so parse
     * stays linear regardless of config size. */
    if (!cfg->head)
    {
        cfg->head = entry;
    }
    else
    {
        cfg->tail->next = entry;
    }
    cfg->tail = entry;
    cfg->count += 1u;
    return CB_INFO_OK;
}

/* --- Line parsing --- */

/* Parse the value portion of a line (RHS of '='). Writes the decoded value
   into `out` (length-capped buffer of cap bytes) and stores length in *out_len.
   Returns CB_INFO_OK, CB_INFO_CONFIG_UNTERMINATED_STRING, or CB_INFO_CONFIG_BAD_ESCAPE. */
static cb_info_t cb__config_decode_value(const char *s, size_t len,
                                         char *out, size_t cap, size_t *out_len)
{
    /* Strip leading whitespace. */
    size_t i = 0;
    while (i < len && cb__config_is_space(s[i])) ++i;

    if (i >= len || s[i] != '"')
    {
        /* Unquoted: find unescaped '#' or ';' as trailing comment start. */
        size_t start = i;
        size_t j = i;
        while (j < len)
        {
            if (s[j] == '#' || s[j] == ';') break;
            ++j;
        }
        size_t end = j;
        /* Re-strip trailing whitespace from [start, end). */
        while (end > start && cb__config_is_space(s[end - 1])) --end;

        size_t n = end - start;
        if (n > cap) n = cap; /* should not trigger given line cap >= cap, but defensive */
        if (n > 0) memcpy(out, s + start, n);
        *out_len = n;
        return CB_INFO_OK;
    }

    /* Quoted. Walk until the matching closing quote, decoding escapes. */
    size_t j = i + 1; /* skip opening '"' */
    size_t w = 0;
    while (j < len)
    {
        char c = s[j];
        if (c == '"')
        {
            /* Closing quote found. Anything after it must be whitespace or
             * a full-line comment ('#'/';'); otherwise the line is malformed
             * (e.g. `key = "x"hello`, which previously silently accepted). */
            size_t k = j + 1;
            while (k < len && cb__config_is_space(s[k])) ++k;
            if (k < len && s[k] != '#' && s[k] != ';')
            {
                return CB_INFO_CONFIG_PARSE_ERROR;
            }
            *out_len = w;
            return CB_INFO_OK;
        }
        if (c == '\\')
        {
            if (j + 1 >= len) return CB_INFO_CONFIG_UNTERMINATED_STRING;
            char esc = s[j + 1];
            char decoded;
            switch (esc)
            {
                case '"':  decoded = '"';  break;
                case '\\': decoded = '\\'; break;
                case 'n':  decoded = '\n'; break;
                case 't':  decoded = '\t'; break;
                case 'r':  decoded = '\r'; break;
                default:   return CB_INFO_CONFIG_BAD_ESCAPE;
            }
            if (w < cap) out[w++] = decoded;
            j += 2;
            continue;
        }
        if (w < cap) out[w++] = c;
        ++j;
    }

    /* Ran off the end without a closing quote. */
    return CB_INFO_CONFIG_UNTERMINATED_STRING;
}

/* Parse a single logical line (already stripped of terminator). line_buf
   length is provided explicitly; it may contain embedded NULs (they will
   be caught by the key validator). Returns CB_INFO_OK on success (including
   blank/comment lines), or a CB_INFO_CONFIG_* error. */
static cb_info_t cb__config_parse_line(cb_config_t *cfg, const char *line, size_t len)
{
    /* Strip leading whitespace. */
    size_t i = 0;
    while (i < len && cb__config_is_space(line[i])) ++i;

    /* Blank or comment? */
    if (i >= len) return CB_INFO_OK;
    if (line[i] == '#' || line[i] == ';') return CB_INFO_OK;

    /* Find '='. */
    size_t eq = i;
    while (eq < len && line[eq] != '=') ++eq;
    if (eq >= len)
    {
        /* No '=' at all and it wasn't a blank/comment: parse error. */
        return CB_INFO_CONFIG_PARSE_ERROR;
    }

    /* Key is [i, eq) with trailing whitespace stripped. */
    size_t key_end = eq;
    while (key_end > i && cb__config_is_space(line[key_end - 1])) --key_end;

    size_t key_len = key_end - i;
    if (!cb__config_key_valid(line + i, key_len))
    {
        return CB_INFO_CONFIG_BAD_KEY;
    }

    /* Decode value from (eq+1, len). Allocate a temporary decoded buffer
       sized to the remaining line length (upper bound — escapes only shrink). */
    size_t rhs_len = len - (eq + 1);
    char *decoded  = (char *)cb__alloc(cfg->arena, rhs_len + 1u, 1u);
    if (!decoded) return CB_INFO_CONFIG_ALLOC_FAILED;

    size_t val_len = 0;
    cb_info_t dinfo = cb__config_decode_value(line + eq + 1, rhs_len,
                                              decoded, rhs_len, &val_len);
    if (dinfo != CB_INFO_OK)
    {
        cb__free(cfg->arena, decoded);
        return dinfo;
    }

    cb_info_t sinfo = cb__config_set(cfg, line + i, key_len, decoded, val_len);
    cb__free(cfg->arena, decoded);
    return sinfo;
}

/* --- Public: parse / destroy --- */

static void cb__config_free_entries(cb_config_t *cfg)
{
    cb__config_entry_t *e = cfg->head;
    while (e)
    {
        cb__config_entry_t *next = e->next;
        cb__free(cfg->arena, e->key);
        cb__free(cfg->arena, e->value);
        cb__free(cfg->arena, e);
        e = next;
    }
    cfg->head  = NULL;
    cfg->tail  = NULL;
    cfg->count = 0;
}

cb_config_t cb_config_parse(cb_arena_t *arena, const char *text, size_t len)
{
    cb_config_t cfg;
    cfg.info       = CB_INFO_OK;
    cfg.error_line = 0;
    cfg.count      = 0;
    cfg.arena      = arena;
    cfg.head       = NULL;
    cfg.tail       = NULL;

    if (!text && len > 0)
    {
        cfg.info       = CB_INFO_CONFIG_PARSE_ERROR;
        cfg.error_line = 0;
        return cfg;
    }

    uint32_t line_no = 0;
    size_t   i = 0;
    while (i <= len)
    {
        /* Locate end of this line. */
        size_t start = i;
        while (i < len && text[i] != '\n') ++i;
        size_t raw_end = i;
        /* Strip a single trailing '\r' so CRLF works. */
        size_t end = raw_end;
        if (end > start && text[end - 1] == '\r') --end;

        size_t line_len = end - start;
        line_no += 1u;

        /* Only parse this line if it has content OR we haven't reached EOF.
           Empty trailing line (after a final '\n') is a no-op — don't count
           the synthetic final iteration after len. */
        if (start >= len)
        {
            break;
        }

        if (line_len > (size_t)CB__CONFIG_LINE_MAX)
        {
            cfg.info       = CB_INFO_CONFIG_LINE_TOO_LONG;
            cfg.error_line = line_no;
            cb__config_free_entries(&cfg);
            return cfg;
        }

        cb_info_t info = cb__config_parse_line(&cfg, text + start, line_len);
        if (info != CB_INFO_OK)
        {
            cfg.info       = info;
            cfg.error_line = line_no;
            cb__config_free_entries(&cfg);
            return cfg;
        }

        if (i < len)
        {
            /* Skip the '\n' we found. */
            i += 1u;
        }
        else
        {
            /* Reached the end without a trailing '\n'. Stop. */
            break;
        }
    }

    return cfg;
}

cb_config_t cb_config_parse_file(cb_arena_t *arena, const char *path)
{
    cb_config_t cfg;
    cfg.info       = CB_INFO_OK;
    cfg.error_line = 0;
    cfg.count      = 0;
    cfg.arena      = arena;
    cfg.head       = NULL;
    cfg.tail       = NULL;

    FILE *f = fopen(path, "rb");
    if (!f)
    {
        cfg.info = CB_INFO_CONFIG_FILE_OPEN_FAILED;
        return cfg;
    }

    /* Size the file. We use fseek/ftell — adequate for ordinary config files
       on a regular filesystem. */
    if (fseek(f, 0, SEEK_END) != 0)
    {
        fclose(f);
        cfg.info = CB_INFO_CONFIG_FILE_OPEN_FAILED;
        return cfg;
    }
    long raw_sz = ftell(f);
    if (raw_sz < 0)
    {
        fclose(f);
        cfg.info = CB_INFO_CONFIG_FILE_OPEN_FAILED;
        return cfg;
    }
    if ((unsigned long)raw_sz > (unsigned long)CB__CONFIG_FILE_MAX)
    {
        fclose(f);
        cfg.info = CB_INFO_CONFIG_FILE_TOO_LARGE;
        return cfg;
    }
    if (fseek(f, 0, SEEK_SET) != 0)
    {
        fclose(f);
        cfg.info = CB_INFO_CONFIG_FILE_OPEN_FAILED;
        return cfg;
    }

    size_t sz = (size_t)raw_sz;
    /* Read buffer lifetime is parse-scoped: allocate through cb__alloc so
     * arena callers see a single allocator path end-to-end, and cb__free it
     * before returning regardless of arena-vs-malloc. For linear/exponential
     * arenas cb__free is a no-op and the transient sits in the arena until
     * the caller resets or destroys it (documented below). For fixed-strategy
     * arenas and for the malloc fallback, cb__free actually releases. */
    char *buf = (char *)cb__alloc(arena, sz > 0 ? sz : 1u, 1u);
    if (!buf)
    {
        fclose(f);
        cfg.info = CB_INFO_CONFIG_ALLOC_FAILED;
        return cfg;
    }

    size_t nread = sz > 0 ? fread(buf, 1, sz, f) : 0;
    fclose(f);
    if (nread != sz)
    {
        cb__free(arena, buf);
        cfg.info = CB_INFO_CONFIG_FILE_OPEN_FAILED;
        return cfg;
    }

    cfg = cb_config_parse(arena, buf, sz);
    cb__free(arena, buf);
    return cfg;
}

void cb_config_destroy(cb_config_t *cfg)
{
    if (!cfg) return;
    cb__config_free_entries(cfg);
    cfg->info       = CB_INFO_OK;
    cfg->error_line = 0;
    /* cfg->arena is preserved: caller still owns the arena. */
}

/* --- Public: lookup --- */

const char *cb_config_get(const cb_config_t *cfg, const char *key)
{
    if (!cfg || !key) return NULL;
    cb__config_entry_t *e = cb__config_find_entry(cfg, key);
    return e ? e->value : NULL;
}

/* --- Typed getters --- */

/* Parse a base-10 signed integer. Accepts optional leading '-'. No leading '+',
   no hex, no oct, no underscores. Empty string or trailing junk is a failure. */
static bool cb__config_parse_i64(const char *s, int64_t *out)
{
    if (!s || s[0] == '\0') return false;

    bool neg = false;
    size_t i = 0;
    if (s[0] == '-')
    {
        neg = true;
        i = 1;
        if (s[i] == '\0') return false;
    }

    /* Require at least one digit. */
    if (!(s[i] >= '0' && s[i] <= '9')) return false;

    /* Parse into a u64 accumulator with overflow guards, then apply sign. */
    uint64_t acc = 0;
    /* Max magnitude: neg -> 2^63, pos -> 2^63 - 1. */
    uint64_t limit = neg ? (uint64_t)INT64_MAX + 1u : (uint64_t)INT64_MAX;

    while (s[i] != '\0')
    {
        char c = s[i];
        if (!(c >= '0' && c <= '9')) return false;
        uint64_t d = (uint64_t)(c - '0');
        if (acc > (limit - d) / 10u) return false;
        acc = acc * 10u + d;
        ++i;
    }

    if (neg)
    {
        if (acc == (uint64_t)INT64_MAX + 1u)
        {
            *out = INT64_MIN;
        }
        else
        {
            *out = -(int64_t)acc;
        }
    }
    else
    {
        *out = (int64_t)acc;
    }
    return true;
}

static bool cb__config_parse_u64(const char *s, uint64_t *out)
{
    if (!s || s[0] == '\0') return false;
    /* Reject leading '-' even though - would be caught by the digit check
       below; also reject '+' for consistency. */
    if (!(s[0] >= '0' && s[0] <= '9')) return false;

    uint64_t acc = 0;
    size_t i = 0;
    while (s[i] != '\0')
    {
        char c = s[i];
        if (!(c >= '0' && c <= '9')) return false;
        uint64_t d = (uint64_t)(c - '0');
        if (acc > (UINT64_MAX - d) / 10u) return false;
        acc = acc * 10u + d;
        ++i;
    }
    *out = acc;
    return true;
}

static bool cb__config_parse_bool(const char *s, bool *out)
{
    if (!s) return false;
    if (cb__config_ieq(s, "true")  || cb__config_ieq(s, "yes") ||
        cb__config_ieq(s, "on")    || cb__config_ieq(s, "1"))
    {
        *out = true;
        return true;
    }
    if (cb__config_ieq(s, "false") || cb__config_ieq(s, "no") ||
        cb__config_ieq(s, "off")   || cb__config_ieq(s, "0"))
    {
        *out = false;
        return true;
    }
    return false;
}

cb_config_i64_t cb_config_get_i64(const cb_config_t *cfg, const char *key, int64_t fallback)
{
    cb_config_i64_t r;
    r.info  = CB_INFO_OK;
    r.value = fallback;

    const char *s = cb_config_get(cfg, key);
    if (!s)
    {
        r.info = CB_INFO_CONFIG_KEY_NOT_FOUND;
        return r;
    }
    int64_t v;
    if (!cb__config_parse_i64(s, &v))
    {
        r.info = CB_INFO_CONFIG_BAD_INT;
        return r;
    }
    r.value = v;
    return r;
}

cb_config_u64_t cb_config_get_u64(const cb_config_t *cfg, const char *key, uint64_t fallback)
{
    cb_config_u64_t r;
    r.info  = CB_INFO_OK;
    r.value = fallback;

    const char *s = cb_config_get(cfg, key);
    if (!s)
    {
        r.info = CB_INFO_CONFIG_KEY_NOT_FOUND;
        return r;
    }
    uint64_t v;
    if (!cb__config_parse_u64(s, &v))
    {
        r.info = CB_INFO_CONFIG_BAD_INT;
        return r;
    }
    r.value = v;
    return r;
}

cb_config_bool_t cb_config_get_bool(const cb_config_t *cfg, const char *key, bool fallback)
{
    cb_config_bool_t r;
    r.info  = CB_INFO_OK;
    r.value = fallback;

    const char *s = cb_config_get(cfg, key);
    if (!s)
    {
        r.info = CB_INFO_CONFIG_KEY_NOT_FOUND;
        return r;
    }
    bool v;
    if (!cb__config_parse_bool(s, &v))
    {
        r.info = CB_INFO_CONFIG_BAD_BOOL;
        return r;
    }
    r.value = v;
    return r;
}
