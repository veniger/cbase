/*
 * config_test.c - Tests for the cb_config module.
 *
 * Covers:
 *   1. Empty input.
 *   2. Comments-only input.
 *   3. Basic key=value.
 *   4. Spaces around '='.
 *   5. Duplicate keys, last wins.
 *   6. Quoted value with spaces and '#'.
 *   7. Escapes inside quotes.
 *   8. Trailing comment unquoted.
 *   9. Dotted and dashed keys.
 *  10. Bad key -> CB_INFO_CONFIG_BAD_KEY.
 *  11. Unterminated string.
 *  12. Bad escape.
 *  13. Line too long (>4096 bytes).
 *  14. get_i64 success / bad / missing.
 *  15. get_u64 success / bad / missing.
 *  16. get_bool variants (true/false/YES/on/0/gibberish/missing).
 *  17. Roundtrip through a file on disk.
 *  18. Parse-fail safety (destroy, get).
 *  19. Arena vs malloc.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>  /* getpid (POSIX) */
#include "../cbase.h"

/* ---------- helpers ---------- */

#define CHECK(cond, msg) do {                                                  \
    if (!(cond)) {                                                             \
        fprintf(stderr, "FAIL: %s  (at %s:%d)\n", (msg), __FILE__, __LINE__);  \
        return 1;                                                              \
    }                                                                          \
} while (0)

static size_t cb__test_strlen(const char *s) { return strlen(s); }

/* Shorthand: parse a literal null-terminated string with malloc allocator. */
static cb_config_t cfg_parse_lit(const char *s)
{
    return cb_config_parse(NULL, s, cb__test_strlen(s));
}

/* ---------- section 1 ---------- */

static int section1_empty(void)
{
    printf("--- Section 1: empty input\n");

    cb_config_t cfg = cb_config_parse(NULL, "", 0);
    CHECK(cfg.info == CB_INFO_OK, "empty input must parse OK");
    CHECK(cfg.count == 0, "empty input has 0 keys");
    CHECK(cb_config_get(&cfg, "anything") == NULL, "get on empty returns NULL");
    cb_config_destroy(&cfg);

    printf("  empty OK\n");
    return 0;
}

/* ---------- section 2 ---------- */

static int section2_comments_only(void)
{
    printf("--- Section 2: comments-only input\n");

    cb_config_t cfg = cfg_parse_lit("# hi\n; bye\n\n  # indented comment\n");
    CHECK(cfg.info == CB_INFO_OK, "comments-only must parse OK");
    CHECK(cfg.count == 0, "comments-only has 0 keys");
    cb_config_destroy(&cfg);

    printf("  comments-only OK\n");
    return 0;
}

/* ---------- section 3 ---------- */

static int section3_basic(void)
{
    printf("--- Section 3: basic key=value\n");

    cb_config_t cfg = cfg_parse_lit("port=8080\n");
    CHECK(cfg.info == CB_INFO_OK, "basic must parse OK");
    CHECK(cfg.count == 1, "basic has 1 key");

    const char *v = cb_config_get(&cfg, "port");
    CHECK(v != NULL, "port must be present");
    CHECK(strcmp(v, "8080") == 0, "port == \"8080\"");
    cb_config_destroy(&cfg);

    printf("  basic OK\n");
    return 0;
}

/* ---------- section 4 ---------- */

static int section4_spaces_around_eq(void)
{
    printf("--- Section 4: spaces around '='\n");

    cb_config_t cfg = cfg_parse_lit("  key  =  value  \n");
    CHECK(cfg.info == CB_INFO_OK, "spaces-around-eq parse OK");
    const char *v = cb_config_get(&cfg, "key");
    CHECK(v != NULL, "key present");
    CHECK(strcmp(v, "value") == 0, "value stripped of surrounding spaces");
    cb_config_destroy(&cfg);

    printf("  spaces-around-eq OK\n");
    return 0;
}

/* ---------- section 5 ---------- */

static int section5_duplicate_keys(void)
{
    printf("--- Section 5: duplicate keys, last wins\n");

    cb_config_t cfg = cfg_parse_lit("x=first\nx=second\n");
    CHECK(cfg.info == CB_INFO_OK, "duplicate-keys parse OK");
    CHECK(cfg.count == 1, "duplicate-keys collapses to 1 entry");
    const char *v = cb_config_get(&cfg, "x");
    CHECK(v != NULL, "x present");
    CHECK(strcmp(v, "second") == 0, "later value wins");
    cb_config_destroy(&cfg);

    printf("  duplicate-keys OK\n");
    return 0;
}

/* ---------- section 6 ---------- */

static int section6_quoted_spaces_and_hash(void)
{
    printf("--- Section 6: quoted value w/ spaces and '#'\n");

    cb_config_t cfg = cfg_parse_lit("key = \"a b # c\"\n");
    CHECK(cfg.info == CB_INFO_OK, "quoted parse OK");
    const char *v = cb_config_get(&cfg, "key");
    CHECK(v != NULL, "key present");
    CHECK(strcmp(v, "a b # c") == 0, "quoted value preserved exactly");
    cb_config_destroy(&cfg);

    printf("  quoted-spaces-hash OK\n");
    return 0;
}

/* ---------- section 7 ---------- */

static int section7_escapes(void)
{
    printf("--- Section 7: escapes inside quotes\n");

    /* Source line: key = "line\nbreak\ttab\"quote\\back"
       In C string literal form we double the backslashes and quotes. */
    const char *src = "key = \"line\\nbreak\\ttab\\\"quote\\\\back\"\n";
    cb_config_t cfg = cfg_parse_lit(src);
    CHECK(cfg.info == CB_INFO_OK, "escape parse OK");

    const char *v = cb_config_get(&cfg, "key");
    CHECK(v != NULL, "key present");
    CHECK(strcmp(v, "line\nbreak\ttab\"quote\\back") == 0,
          "escapes decoded to their literal chars");
    cb_config_destroy(&cfg);

    printf("  escapes OK\n");
    return 0;
}

/* ---------- section 8 ---------- */

static int section8_trailing_comment_unquoted(void)
{
    printf("--- Section 8: trailing comment (unquoted)\n");

    cb_config_t cfg = cfg_parse_lit("key = value # trailing\n");
    CHECK(cfg.info == CB_INFO_OK, "trailing-comment parse OK");
    const char *v = cb_config_get(&cfg, "key");
    CHECK(v != NULL, "key present");
    CHECK(strcmp(v, "value") == 0, "trailing-comment stripped, value trimmed");

    cb_config_t cfg2 = cfg_parse_lit("k2=v2;comment\n");
    CHECK(cfg2.info == CB_INFO_OK, "trailing ';' comment parse OK");
    const char *v2 = cb_config_get(&cfg2, "k2");
    CHECK(v2 && strcmp(v2, "v2") == 0, "semicolon-starting trailing comment stripped");

    cb_config_destroy(&cfg);
    cb_config_destroy(&cfg2);

    printf("  trailing-comment OK\n");
    return 0;
}

/* ---------- section 9 ---------- */

static int section9_dotted_dashed(void)
{
    printf("--- Section 9: dotted + dashed keys\n");

    cb_config_t cfg = cfg_parse_lit("net.udp.port = 7777\nsome-key = 1\n");
    CHECK(cfg.info == CB_INFO_OK, "dotted/dashed parse OK");
    CHECK(cfg.count == 2, "two entries");

    const char *a = cb_config_get(&cfg, "net.udp.port");
    const char *b = cb_config_get(&cfg, "some-key");
    CHECK(a && strcmp(a, "7777") == 0, "dotted key lookup");
    CHECK(b && strcmp(b, "1")    == 0, "dashed key lookup");
    cb_config_destroy(&cfg);

    printf("  dotted-dashed OK\n");
    return 0;
}

/* ---------- section 10 ---------- */

static int section10_bad_key(void)
{
    printf("--- Section 10: bad key\n");

    cb_config_t cfg = cfg_parse_lit("1foo = bar\n");
    CHECK(cfg.info == CB_INFO_CONFIG_BAD_KEY, "leading digit is BAD_KEY");
    CHECK(cfg.error_line == 1, "error_line == 1");
    CHECK(cfg.count == 0, "no entries on parse error");
    CHECK(cb_config_get(&cfg, "1foo") == NULL, "nothing to look up");
    cb_config_destroy(&cfg);

    /* Empty key */
    cb_config_t cfg2 = cfg_parse_lit("= bar\n");
    CHECK(cfg2.info == CB_INFO_CONFIG_BAD_KEY, "empty key is BAD_KEY");
    CHECK(cfg2.error_line == 1, "error_line == 1 for empty key");
    cb_config_destroy(&cfg2);

    /* Bad character in key */
    cb_config_t cfg3 = cfg_parse_lit("foo bar = baz\n");
    CHECK(cfg3.info == CB_INFO_CONFIG_BAD_KEY, "space in key is BAD_KEY");
    cb_config_destroy(&cfg3);

    printf("  bad-key OK\n");
    return 0;
}

/* ---------- section 11 ---------- */

static int section11_unterminated_string(void)
{
    printf("--- Section 11: unterminated string\n");

    cb_config_t cfg = cfg_parse_lit("key = \"no close\n");
    CHECK(cfg.info == CB_INFO_CONFIG_UNTERMINATED_STRING,
          "missing closing quote is UNTERMINATED_STRING");
    CHECK(cfg.error_line == 1, "error_line == 1");
    cb_config_destroy(&cfg);

    printf("  unterminated-string OK\n");
    return 0;
}

/* ---------- section 12 ---------- */

static int section12_bad_escape(void)
{
    printf("--- Section 12: bad escape\n");

    /* Line: key = "\q" */
    cb_config_t cfg = cfg_parse_lit("key = \"\\q\"\n");
    CHECK(cfg.info == CB_INFO_CONFIG_BAD_ESCAPE, "\\q must be BAD_ESCAPE");
    CHECK(cfg.error_line == 1, "error_line == 1");
    cb_config_destroy(&cfg);

    printf("  bad-escape OK\n");
    return 0;
}

/* ---------- section 13 ---------- */

static int section13_line_too_long(void)
{
    printf("--- Section 13: line too long\n");

    /* Construct a 5000-char line. We deliberately make it a valid-looking
       "key=" line, because the length check happens BEFORE any parsing. */
    size_t N = 5000;
    char *buf = (char *)malloc(N + 2);
    CHECK(buf != NULL, "malloc");
    memset(buf, 'a', N);
    buf[N] = '\n';
    buf[N + 1] = '\0';

    cb_config_t cfg = cb_config_parse(NULL, buf, N + 1);
    CHECK(cfg.info == CB_INFO_CONFIG_LINE_TOO_LONG, "5000-char line is LINE_TOO_LONG");
    CHECK(cfg.error_line == 1, "error_line == 1");
    cb_config_destroy(&cfg);
    free(buf);

    printf("  line-too-long OK\n");
    return 0;
}

/* ---------- section 14 ---------- */

static int section14_get_i64(void)
{
    printf("--- Section 14: get_i64\n");

    cb_config_t cfg = cfg_parse_lit(
        "good=-12345\n"
        "bad=12abc\n"
        "overflow=99999999999999999999\n"
    );
    CHECK(cfg.info == CB_INFO_OK, "parse OK");

    cb_config_i64_t a = cb_config_get_i64(&cfg, "good", 42);
    CHECK(a.info == CB_INFO_OK, "good info OK");
    CHECK(a.value == -12345, "good value");

    cb_config_i64_t b = cb_config_get_i64(&cfg, "bad", 42);
    CHECK(b.info == CB_INFO_CONFIG_BAD_INT, "bad info BAD_INT");
    CHECK(b.value == 42, "bad value == fallback");

    cb_config_i64_t c = cb_config_get_i64(&cfg, "missing", -7);
    CHECK(c.info == CB_INFO_CONFIG_KEY_NOT_FOUND, "missing info KEY_NOT_FOUND");
    CHECK(c.value == -7, "missing value == fallback");

    cb_config_i64_t d = cb_config_get_i64(&cfg, "overflow", 1);
    CHECK(d.info == CB_INFO_CONFIG_BAD_INT, "overflow info BAD_INT");
    CHECK(d.value == 1, "overflow value == fallback");

    cb_config_destroy(&cfg);

    /* Edge case: parse INT64_MIN and INT64_MAX correctly. */
    cb_config_t cfg2 = cfg_parse_lit(
        "mn=-9223372036854775808\n"
        "mx=9223372036854775807\n"
    );
    cb_config_i64_t mn = cb_config_get_i64(&cfg2, "mn", 0);
    CHECK(mn.info == CB_INFO_OK && mn.value == INT64_MIN, "INT64_MIN parsed");
    cb_config_i64_t mx = cb_config_get_i64(&cfg2, "mx", 0);
    CHECK(mx.info == CB_INFO_OK && mx.value == INT64_MAX, "INT64_MAX parsed");
    cb_config_destroy(&cfg2);

    printf("  get_i64 OK\n");
    return 0;
}

/* ---------- section 15 ---------- */

static int section15_get_u64(void)
{
    printf("--- Section 15: get_u64\n");

    cb_config_t cfg = cfg_parse_lit(
        "good=7777\n"
        "neg=-1\n"
        "gibberish=foo\n"
    );
    CHECK(cfg.info == CB_INFO_OK, "parse OK");

    cb_config_u64_t a = cb_config_get_u64(&cfg, "good", 1);
    CHECK(a.info == CB_INFO_OK && a.value == 7777u, "good");

    cb_config_u64_t b = cb_config_get_u64(&cfg, "neg", 9);
    CHECK(b.info == CB_INFO_CONFIG_BAD_INT, "negative rejected for u64");
    CHECK(b.value == 9, "neg fallback");

    cb_config_u64_t c = cb_config_get_u64(&cfg, "gibberish", 9);
    CHECK(c.info == CB_INFO_CONFIG_BAD_INT, "gibberish BAD_INT");
    CHECK(c.value == 9, "gibberish fallback");

    cb_config_u64_t d = cb_config_get_u64(&cfg, "missing", 42);
    CHECK(d.info == CB_INFO_CONFIG_BAD_INT || d.info == CB_INFO_CONFIG_KEY_NOT_FOUND,
          "missing recognized");
    CHECK(d.info == CB_INFO_CONFIG_KEY_NOT_FOUND, "missing info KEY_NOT_FOUND");
    CHECK(d.value == 42, "missing fallback");

    cb_config_destroy(&cfg);

    printf("  get_u64 OK\n");
    return 0;
}

/* ---------- section 16 ---------- */

static int section16_get_bool(void)
{
    printf("--- Section 16: get_bool variants\n");

    cb_config_t cfg = cfg_parse_lit(
        "a = true\n"
        "b = FALSE\n"
        "c = YES\n"
        "d = on\n"
        "e = 0\n"
        "f = banana\n"
    );
    CHECK(cfg.info == CB_INFO_OK, "parse OK");

    struct { const char *k; bool expect; cb_info_t exp_info; bool fb; } cases[] = {
        { "a", true,  CB_INFO_OK, false },
        { "b", false, CB_INFO_OK, true  },
        { "c", true,  CB_INFO_OK, false },
        { "d", true,  CB_INFO_OK, false },
        { "e", false, CB_INFO_OK, true  },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        cb_config_bool_t r = cb_config_get_bool(&cfg, cases[i].k, cases[i].fb);
        CHECK(r.info == cases[i].exp_info, "bool info");
        CHECK(r.value == cases[i].expect, "bool value");
    }

    cb_config_bool_t gib = cb_config_get_bool(&cfg, "f", true);
    CHECK(gib.info == CB_INFO_CONFIG_BAD_BOOL, "gibberish BAD_BOOL");
    CHECK(gib.value == true, "gibberish fallback");

    cb_config_bool_t miss = cb_config_get_bool(&cfg, "nope", false);
    CHECK(miss.info == CB_INFO_CONFIG_KEY_NOT_FOUND, "missing KEY_NOT_FOUND");
    CHECK(miss.value == false, "missing fallback");

    cb_config_destroy(&cfg);

    printf("  get_bool OK\n");
    return 0;
}

/* ---------- section 17 ---------- */

/* We store the tempfile path in a static so atexit() can remove it. */
static char g_tmp_path[128];

static void cleanup_tmpfile(void)
{
    if (g_tmp_path[0] != '\0') {
        remove(g_tmp_path);
    }
}

static int section17_roundtrip_file(void)
{
    printf("--- Section 17: roundtrip through a tempfile\n");

    /* Use a fixed path containing the pid so parallel runs don't collide. */
    snprintf(g_tmp_path, sizeof(g_tmp_path),
             "/tmp/cbase_config_test_%d.cfg", (int)getpid());
    atexit(cleanup_tmpfile);

    const char *text =
        "# a comment\n"
        "port = 4242\n"
        "name = \"Hello, world\"\n"
        "flag = true\n";

    FILE *f = fopen(g_tmp_path, "wb");
    CHECK(f != NULL, "open tempfile for write");
    size_t n = strlen(text);
    size_t nw = fwrite(text, 1, n, f);
    CHECK(nw == n, "write tempfile");
    fclose(f);

    cb_config_t cfg = cb_config_parse_file(NULL, g_tmp_path);
    CHECK(cfg.info == CB_INFO_OK, "parse_file OK");
    CHECK(cfg.count == 3, "3 entries");

    const char *name = cb_config_get(&cfg, "name");
    CHECK(name != NULL, "name present");
    CHECK(strcmp(name, "Hello, world") == 0, "name value");

    cb_config_i64_t  p = cb_config_get_i64 (&cfg, "port", 0);
    cb_config_bool_t b = cb_config_get_bool(&cfg, "flag", false);
    CHECK(p.info == CB_INFO_OK && p.value == 4242, "port == 4242");
    CHECK(b.info == CB_INFO_OK && b.value == true, "flag == true");

    cb_config_destroy(&cfg);
    remove(g_tmp_path);
    g_tmp_path[0] = '\0'; /* atexit sees empty -> no-op */

    /* parse_file on a nonexistent path returns FILE_OPEN_FAILED. */
    cb_config_t bad = cb_config_parse_file(NULL, "/tmp/cbase_config_does_not_exist_xyzzy.cfg");
    CHECK(bad.info == CB_INFO_CONFIG_FILE_OPEN_FAILED, "missing file -> FILE_OPEN_FAILED");
    cb_config_destroy(&bad);

    printf("  roundtrip-file OK\n");
    return 0;
}

/* ---------- section 18 ---------- */

static int section18_parse_fail_safety(void)
{
    printf("--- Section 18: parse-fail safety\n");

    cb_config_t cfg = cfg_parse_lit("1bad = x\n");
    CHECK(cfg.info == CB_INFO_CONFIG_BAD_KEY, "parse failed as expected");
    CHECK(cfg.count == 0, "no entries after parse fail");
    CHECK(cb_config_get(&cfg, "1bad") == NULL, "get returns NULL");
    CHECK(cb_config_get(&cfg, "anything") == NULL, "get returns NULL for any key");

    /* destroy must be safe. */
    cb_config_destroy(&cfg);
    /* Calling destroy twice must also be safe (head == NULL after first). */
    cb_config_destroy(&cfg);

    printf("  parse-fail safety OK\n");
    return 0;
}

/* ---------- section 19 ---------- */

static int section19_arena_vs_malloc(void)
{
    printf("--- Section 19: arena vs malloc\n");

    const char *text =
        "a = 1\n"
        "b = \"two\"\n"
        "c.d-e = three\n";
    size_t len = strlen(text);

    /* Pass 1: malloc allocator. */
    cb_config_t cfg_mal = cb_config_parse(NULL, text, len);
    CHECK(cfg_mal.info == CB_INFO_OK, "malloc parse OK");
    CHECK(cfg_mal.count == 3, "malloc count");
    const char *v1 = cb_config_get(&cfg_mal, "c.d-e");
    CHECK(v1 && strcmp(v1, "three") == 0, "malloc lookup");
    cb_config_destroy(&cfg_mal);

    /* Pass 2: small exponential arena. */
    cb_arena_t arena = cb_arena_create(256, CB_ARENA_EXPONENTIAL);
    CHECK(arena.info == CB_INFO_OK, "arena created");

    cb_config_t cfg_ar = cb_config_parse(&arena, text, len);
    CHECK(cfg_ar.info == CB_INFO_OK, "arena parse OK");
    CHECK(cfg_ar.count == 3, "arena count");
    const char *v2 = cb_config_get(&cfg_ar, "b");
    CHECK(v2 && strcmp(v2, "two") == 0, "arena lookup");
    /* destroy on an arena-backed config is a no-op per-allocation, but must
       still null out the list so later gets are safe. */
    cb_config_destroy(&cfg_ar);
    CHECK(cb_config_get(&cfg_ar, "b") == NULL, "after-destroy get returns NULL");

    CHECK(cb_arena_check_health(&arena), "arena guards intact");
    cb_arena_destroy(&arena);

    printf("  arena-vs-malloc OK\n");
    return 0;
}

/* ---------- section 20: triple duplicate keys, last wins, count stays 1 ---------- */

static int section20_triple_duplicate(void)
{
    printf("--- Section 20: A=1\\nA=2\\nA=3 collapses to last-wins\n");

    cb_config_t cfg = cfg_parse_lit("A=1\nA=2\nA=3\n");
    CHECK(cfg.info == CB_INFO_OK, "triple-duplicate parse OK");
    CHECK(cfg.count == 1, "triple-duplicate collapses to 1 entry");
    const char *v = cb_config_get(&cfg, "A");
    CHECK(v != NULL, "A present");
    CHECK(strcmp(v, "3") == 0, "last-wins gives 3");
    cb_config_destroy(&cfg);

    printf("  triple-duplicate OK\n");
    return 0;
}

/* ---------- section 21: quoted value with trailing garbage rejected ---------- */

static int section21_quoted_trailing_garbage(void)
{
    printf("--- Section 21: quoted value trailing garbage -> PARSE_ERROR\n");

    /* `key = \"x\"hello\n` — previously silently accepted as value=\"x\". */
    cb_config_t cfg = cfg_parse_lit("key = \"x\"hello\n");
    CHECK(cfg.info == CB_INFO_CONFIG_PARSE_ERROR,
          "quoted + trailing token must be PARSE_ERROR");
    cb_config_destroy(&cfg);

    /* Whitespace after the close quote is still fine. */
    cb_config_t cfg2 = cfg_parse_lit("key = \"x\"   \n");
    CHECK(cfg2.info == CB_INFO_OK, "trailing whitespace after quoted OK");
    CHECK(cfg2.count == 1, "one entry");
    const char *v = cb_config_get(&cfg2, "key");
    CHECK(v != NULL && strcmp(v, "x") == 0, "value parsed as x");
    cb_config_destroy(&cfg2);

    /* Comment after the close quote is fine. */
    cb_config_t cfg3 = cfg_parse_lit("key = \"x\" # trailing comment\n");
    CHECK(cfg3.info == CB_INFO_OK, "comment after quoted OK");
    v = cb_config_get(&cfg3, "key");
    CHECK(v != NULL && strcmp(v, "x") == 0, "value is x");
    cb_config_destroy(&cfg3);

    printf("  quoted-trailing-garbage OK\n");
    return 0;
}

/* ---------- main ---------- */

int main(void)
{
    printf("=== cb_config tests ===\n");

    int fails = 0;

    if (section1_empty())                     { fails++; }
    if (section2_comments_only())             { fails++; }
    if (section3_basic())                     { fails++; }
    if (section4_spaces_around_eq())          { fails++; }
    if (section5_duplicate_keys())            { fails++; }
    if (section6_quoted_spaces_and_hash())    { fails++; }
    if (section7_escapes())                   { fails++; }
    if (section8_trailing_comment_unquoted()) { fails++; }
    if (section9_dotted_dashed())             { fails++; }
    if (section10_bad_key())                  { fails++; }
    if (section11_unterminated_string())      { fails++; }
    if (section12_bad_escape())               { fails++; }
    if (section13_line_too_long())            { fails++; }
    if (section14_get_i64())                  { fails++; }
    if (section15_get_u64())                  { fails++; }
    if (section16_get_bool())                 { fails++; }
    if (section17_roundtrip_file())           { fails++; }
    if (section18_parse_fail_safety())        { fails++; }
    if (section19_arena_vs_malloc())          { fails++; }
    if (section20_triple_duplicate())         { fails++; }
    if (section21_quoted_trailing_garbage())  { fails++; }

    if (fails != 0) {
        fprintf(stderr, "FAIL: %d section(s) failed\n", fails);
        return 1;
    }

    printf("All config tests passed!\n");
    return 0;
}
