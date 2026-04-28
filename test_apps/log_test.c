/*
 * log_test.c - Tests for the cb_log module.
 *
 * Strategy: redirect stderr to a tmpfile via dup2 before each section (or
 * group of sections that share redirection). After logging, fflush(stderr),
 * rewind the tmpfile, read it back into a buffer, and pattern-match.
 *
 * Covers:
 *   1. Default level filters TRACE/DEBUG.
 *   2. INFO and above emit by default.
 *   3. Level setter raises/lowers threshold.
 *   4. Module tag formatting (6-col field; NULL -> six spaces).
 *   5. Long module truncated at 16 chars.
 *   6. Message truncation marker on > 4 KiB payloads.
 *   7. printf formatting works.
 *   8. Timestamps disabled.
 *   9. Color off.
 *  10. Color on.
 *  11. File sink dual-write (stderr + file).
 *  12. File sink detach.
 *  13. Thread safety: 4 x 200 = 800 interleaved log lines remain well-formed.
 *  14. Timestamps monotonically non-decreasing.
 *  15. Truncation boundary: exact-fit body emits clean line; +1 byte trips marker.
 *  16. Sticky last_error API smoke test (clear/get round-trip).
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../cbase.h"

/* ---------- helpers ---------- */

#define CHECK(cond, msg) do {                                              \
    if (!(cond)) {                                                         \
        fprintf(stdout, "FAIL: %s  (at %s:%d)\n", (msg), __FILE__, __LINE__); \
        return 1;                                                          \
    }                                                                      \
} while (0)

/* Duplicate stderr's original fd once so we can restore it between sections. */
static int g_saved_stderr_fd = -1;

static void save_stderr_once(void)
{
    if (g_saved_stderr_fd < 0) {
        g_saved_stderr_fd = dup(fileno(stderr));
    }
}

static void restore_stderr(void)
{
    if (g_saved_stderr_fd >= 0) {
        fflush(stderr);
        dup2(g_saved_stderr_fd, fileno(stderr));
    }
}

/* Returns a newly-created tmpfile that stderr is now redirected into. Caller
   fcloses. */
static FILE *redirect_stderr_to_tmp(void)
{
    FILE *tmp = tmpfile();
    if (!tmp) return NULL;
    fflush(stderr);
    dup2(fileno(tmp), fileno(stderr));
    return tmp;
}

/* Reads full contents of `f` into `out` (null-terminated). Returns #bytes. */
static size_t slurp(FILE *f, char *out, size_t cap)
{
    fflush(f);
    fseek(f, 0, SEEK_SET);
    size_t n = fread(out, 1, cap - 1, f);
    out[n] = '\0';
    return n;
}

/* Count (possibly overlapping? — we use non-overlapping) occurrences of
   `needle` in `hay`. */
static size_t count_substr(const char *hay, const char *needle)
{
    size_t nl = strlen(needle);
    if (nl == 0) return 0;
    size_t count = 0;
    const char *p = hay;
    while ((p = strstr(p, needle)) != NULL) {
        count++;
        p += nl;
    }
    return count;
}

/* ---------- Section 1: default level filters TRACE/DEBUG ---------- */

static int section1_default_filter(void)
{
    printf("--- Section 1: default level filters TRACE/DEBUG\n");

    /* Fresh defaults. Log levels default to INFO. */
    cb_log_set_level(CB_LOG_INFO);
    cb_log_set_color(false);
    cb_log_set_timestamps(true);

    FILE *tmp = redirect_stderr_to_tmp();
    CHECK(tmp != NULL, "tmpfile");

    cb_log(CB_LOG_TRACE, "t", "should-not-appear-trace-XYZ");
    cb_log(CB_LOG_DEBUG, "t", "should-not-appear-debug-XYZ");
    cb_log(CB_LOG_INFO,  "t", "present-info-XYZ");

    static char buf[8192];
    slurp(tmp, buf, sizeof(buf));
    restore_stderr();
    fclose(tmp);

    CHECK(strstr(buf, "should-not-appear-trace-XYZ") == NULL, "TRACE must be filtered");
    CHECK(strstr(buf, "should-not-appear-debug-XYZ") == NULL, "DEBUG must be filtered");
    CHECK(strstr(buf, "present-info-XYZ") != NULL, "INFO must pass through");

    /* Bonus: TRACE/DEBUG level tags should not appear anywhere in the
       captured stream (no color stripped; we set color off). */
    CHECK(strstr(buf, "TRACE") == NULL, "no TRACE tag in output");
    CHECK(strstr(buf, "DEBUG") == NULL, "no DEBUG tag in output");

    printf("  section 1 OK\n");
    return 0;
}

/* ---------- Section 2: INFO and above emit by default ---------- */

static int section2_info_and_above(void)
{
    printf("--- Section 2: INFO and above emit by default\n");

    cb_log_set_level(CB_LOG_INFO);
    cb_log_set_color(false);
    cb_log_set_timestamps(true);

    FILE *tmp = redirect_stderr_to_tmp();
    CHECK(tmp != NULL, "tmpfile");

    cb_log(CB_LOG_INFO,  "net", "info-marker-AAA");
    cb_log(CB_LOG_WARN,  "net", "warn-marker-BBB");
    cb_log(CB_LOG_ERROR, "net", "error-marker-CCC");
    cb_log(CB_LOG_FATAL, "net", "fatal-marker-DDD");

    static char buf[8192];
    slurp(tmp, buf, sizeof(buf));
    restore_stderr();
    fclose(tmp);

    CHECK(count_substr(buf, "info-marker-AAA")  == 1, "INFO once");
    CHECK(count_substr(buf, "warn-marker-BBB")  == 1, "WARN once");
    CHECK(count_substr(buf, "error-marker-CCC") == 1, "ERROR once");
    CHECK(count_substr(buf, "fatal-marker-DDD") == 1, "FATAL once");

    printf("  section 2 OK\n");
    return 0;
}

/* ---------- Section 3: level setter raises/lowers threshold ---------- */

static int section3_level_setter(void)
{
    printf("--- Section 3: level setter raises/lowers threshold\n");

    cb_log_set_color(false);
    cb_log_set_timestamps(true);

    /* Raise to WARN: INFO drops, WARN passes. */
    cb_log_set_level(CB_LOG_WARN);
    CHECK(cb_log_get_level() == CB_LOG_WARN, "get_level reports WARN");

    FILE *tmp = redirect_stderr_to_tmp();
    CHECK(tmp != NULL, "tmpfile");
    cb_log(CB_LOG_INFO, "x", "drop-info-S3");
    cb_log(CB_LOG_WARN, "x", "pass-warn-S3");

    static char buf[8192];
    slurp(tmp, buf, sizeof(buf));
    restore_stderr();
    fclose(tmp);

    CHECK(strstr(buf, "drop-info-S3") == NULL, "INFO filtered under WARN threshold");
    CHECK(strstr(buf, "pass-warn-S3") != NULL, "WARN passes under WARN threshold");

    /* Lower to TRACE: TRACE passes. */
    cb_log_set_level(CB_LOG_TRACE);
    FILE *tmp2 = redirect_stderr_to_tmp();
    CHECK(tmp2 != NULL, "tmpfile2");
    cb_log(CB_LOG_TRACE, "x", "pass-trace-S3");

    slurp(tmp2, buf, sizeof(buf));
    restore_stderr();
    fclose(tmp2);

    CHECK(strstr(buf, "pass-trace-S3") != NULL, "TRACE passes under TRACE threshold");

    /* Restore default. */
    cb_log_set_level(CB_LOG_INFO);

    printf("  section 3 OK\n");
    return 0;
}

/* ---------- Section 4: module tag formatting ---------- */

static int section4_module_tag(void)
{
    printf("--- Section 4: module tag formatting\n");

    cb_log_set_level(CB_LOG_INFO);
    cb_log_set_color(false);
    cb_log_set_timestamps(false); /* simplify the prefix for this check */

    FILE *tmp = redirect_stderr_to_tmp();
    CHECK(tmp != NULL, "tmpfile");
    cb_log(CB_LOG_INFO, "abc", "S4-msg-a");
    cb_log(CB_LOG_INFO, NULL,  "S4-msg-b");

    static char buf[8192];
    slurp(tmp, buf, sizeof(buf));
    restore_stderr();
    fclose(tmp);

    /* With timestamps off, lines start with the level tag. Format:
       "INFO  abc    S4-msg-a\n"  <- "abc" + 3 pad spaces + trailing separator = "abc   " then ' '
       "INFO         S4-msg-b\n"  <- 6 pad spaces + ' ' */
    CHECK(strstr(buf, "INFO  abc    S4-msg-a") != NULL, "abc padded to 6 cols");
    /* For the NULL-module line: six spaces after the level tag+space. */
    CHECK(strstr(buf, "INFO         S4-msg-b") != NULL, "NULL module -> six spaces");

    printf("  section 4 OK\n");
    return 0;
}

/* ---------- Section 5: long module truncated at 16 ---------- */

static int section5_module_truncation(void)
{
    printf("--- Section 5: long module truncated at 16\n");

    cb_log_set_level(CB_LOG_INFO);
    cb_log_set_color(false);
    cb_log_set_timestamps(false);

    FILE *tmp = redirect_stderr_to_tmp();
    CHECK(tmp != NULL, "tmpfile");

    /* 20-char module: "abcdefghij0123456789" */
    cb_log(CB_LOG_INFO, "abcdefghij0123456789", "S5-body");

    static char buf[8192];
    slurp(tmp, buf, sizeof(buf));
    restore_stderr();
    fclose(tmp);

    /* The printed module should be exactly first 16 chars ("abcdefghij012345"),
       followed by a single space, followed by body. The 20-char module is
       "abcdefghij" (10) + "0123456789" (10); the first 16 chars are
       "abcdefghij" + "012345". */
    CHECK(strstr(buf, "INFO  abcdefghij012345 S5-body") != NULL, "module truncated to 16 + space");
    /* And the full 20-char string must NOT appear in the output (the last
       4 chars got truncated). */
    CHECK(strstr(buf, "abcdefghij0123456789") == NULL, "full 20-char module absent");

    printf("  section 5 OK\n");
    return 0;
}

/* ---------- Section 6: message truncation marker ---------- */

static int section6_message_truncation(void)
{
    printf("--- Section 6: message truncation marker\n");

    cb_log_set_level(CB_LOG_INFO);
    cb_log_set_color(false);
    cb_log_set_timestamps(false);

    FILE *tmp = redirect_stderr_to_tmp();
    CHECK(tmp != NULL, "tmpfile");

    /* Build a 5 KiB payload. */
    static char payload[5120];
    memset(payload, 'A', sizeof(payload) - 1);
    payload[sizeof(payload) - 1] = '\0';

    cb_log(CB_LOG_INFO, "big", "%s", payload);

    static char buf[16384];
    size_t n = slurp(tmp, buf, sizeof(buf));
    restore_stderr();
    fclose(tmp);

    /* The captured text should end with "...[TRUNC]\n". */
    const char *trunc = "...[TRUNC]\n";
    size_t tl = strlen(trunc);
    CHECK(n >= tl, "output at least as long as marker");
    CHECK(strcmp(buf + n - tl, trunc) == 0, "output ends with ...[TRUNC]\\n");
    /* And the line is bounded — at most ~4 KiB+overhead. */
    CHECK(n <= 4200, "output <= 4200 bytes");

    printf("  section 6 OK (line len = %zu)\n", n);
    return 0;
}

/* ---------- Section 7: printf formatting works ---------- */

static int section7_printf(void)
{
    printf("--- Section 7: printf formatting works\n");

    cb_log_set_level(CB_LOG_INFO);
    cb_log_set_color(false);
    cb_log_set_timestamps(false);

    FILE *tmp = redirect_stderr_to_tmp();
    CHECK(tmp != NULL, "tmpfile");

    cb_log(CB_LOG_INFO, "t", "x=%d y=%s", 7, "abc");

    static char buf[8192];
    slurp(tmp, buf, sizeof(buf));
    restore_stderr();
    fclose(tmp);

    CHECK(strstr(buf, "x=7 y=abc") != NULL, "printf formatting applied");

    printf("  section 7 OK\n");
    return 0;
}

/* ---------- Section 8: timestamps disabled ---------- */

static int section8_timestamps_off(void)
{
    printf("--- Section 8: timestamps disabled\n");

    cb_log_set_level(CB_LOG_INFO);
    cb_log_set_color(false);
    cb_log_set_timestamps(false);

    FILE *tmp = redirect_stderr_to_tmp();
    CHECK(tmp != NULL, "tmpfile");
    cb_log(CB_LOG_INFO, "t", "S8-body");

    static char buf[8192];
    slurp(tmp, buf, sizeof(buf));
    restore_stderr();
    fclose(tmp);

    /* No '[' anywhere (colors off, timestamps off). */
    CHECK(strchr(buf, '[') == NULL, "no '[' when timestamps off");
    /* And the line should start directly with the level tag. */
    CHECK(strncmp(buf, "INFO ", 5) == 0, "line starts with level tag");

    /* Re-enable timestamps for later sections. */
    cb_log_set_timestamps(true);

    printf("  section 8 OK\n");
    return 0;
}

/* ---------- Section 9: color off -> no ANSI escapes ---------- */

static int section9_color_off(void)
{
    printf("--- Section 9: color off\n");

    cb_log_set_level(CB_LOG_INFO);
    cb_log_set_color(false);
    cb_log_set_timestamps(true);

    FILE *tmp = redirect_stderr_to_tmp();
    CHECK(tmp != NULL, "tmpfile");
    cb_log(CB_LOG_ERROR, "t", "S9-body");

    static char buf[8192];
    slurp(tmp, buf, sizeof(buf));
    restore_stderr();
    fclose(tmp);

    CHECK(strchr(buf, '\x1b') == NULL, "no escape bytes when color off");

    printf("  section 9 OK\n");
    return 0;
}

/* ---------- Section 10: color on -> ANSI escapes present ---------- */

static int section10_color_on(void)
{
    printf("--- Section 10: color on\n");

    cb_log_set_level(CB_LOG_INFO);
    cb_log_set_color(true);
    cb_log_set_timestamps(true);

    FILE *tmp = redirect_stderr_to_tmp();
    CHECK(tmp != NULL, "tmpfile");
    cb_log(CB_LOG_ERROR, "t", "S10-body");

    static char buf[8192];
    slurp(tmp, buf, sizeof(buf));
    restore_stderr();
    fclose(tmp);

    CHECK(strstr(buf, "\x1b[31m") != NULL, "red SGR present for ERROR");
    CHECK(strstr(buf, "\x1b[0m")  != NULL, "reset SGR present");

    cb_log_set_color(false);

    printf("  section 10 OK\n");
    return 0;
}

/* ---------- Section 11: file sink dual-write ---------- */

static int section11_file_sink_dual(void)
{
    printf("--- Section 11: file sink dual-write\n");

    cb_log_set_level(CB_LOG_INFO);
    cb_log_set_color(false);
    cb_log_set_timestamps(true);

    FILE *fsink = tmpfile();
    CHECK(fsink != NULL, "file sink tmpfile");
    CHECK(cb_log_set_file_sink(fsink) == CB_INFO_OK, "attach file sink");

    FILE *tmp = redirect_stderr_to_tmp();
    CHECK(tmp != NULL, "stderr tmpfile");
    cb_log(CB_LOG_INFO, "t", "S11-dual-ZZ");

    static char ebuf[8192];
    static char fbuf[8192];
    slurp(tmp, ebuf, sizeof(ebuf));
    restore_stderr();
    fclose(tmp);
    slurp(fsink, fbuf, sizeof(fbuf));

    CHECK(strstr(ebuf, "S11-dual-ZZ") != NULL, "line in stderr");
    CHECK(strstr(fbuf, "S11-dual-ZZ") != NULL, "line in file sink");

    /* Detach and close. */
    cb_log_set_file_sink(NULL);
    fclose(fsink);

    printf("  section 11 OK\n");
    return 0;
}

/* ---------- Section 12: file sink detach ---------- */

static int section12_file_sink_detach(void)
{
    printf("--- Section 12: file sink detach\n");

    cb_log_set_level(CB_LOG_INFO);
    cb_log_set_color(false);
    cb_log_set_timestamps(true);

    FILE *fsink = tmpfile();
    CHECK(fsink != NULL, "file sink tmpfile");
    CHECK(cb_log_set_file_sink(fsink) == CB_INFO_OK, "attach file sink");

    /* Log one line with the sink attached. */
    FILE *tmp = redirect_stderr_to_tmp();
    CHECK(tmp != NULL, "stderr tmpfile");
    cb_log(CB_LOG_INFO, "t", "S12-first");

    /* Detach, then log a second line. */
    CHECK(cb_log_set_file_sink(NULL) == CB_INFO_OK, "detach file sink");
    cb_log(CB_LOG_INFO, "t", "S12-second");

    static char ebuf[8192];
    static char fbuf[8192];
    slurp(tmp, ebuf, sizeof(ebuf));
    restore_stderr();
    fclose(tmp);
    slurp(fsink, fbuf, sizeof(fbuf));

    CHECK(strstr(ebuf, "S12-first")  != NULL, "first line in stderr");
    CHECK(strstr(ebuf, "S12-second") != NULL, "second line in stderr");
    CHECK(strstr(fbuf, "S12-first")  != NULL, "first line in file");
    CHECK(strstr(fbuf, "S12-second") == NULL, "second line NOT in detached file");

    fclose(fsink);

    printf("  section 12 OK\n");
    return 0;
}

/* ---------- Section 13: thread safety ---------- */

#define S13_THREADS       4
#define S13_LINES_PER     200
#define S13_TOTAL_LINES   (S13_THREADS * S13_LINES_PER)

static cb_thread_result_t s13_worker(void *arg)
{
    int id = (int)(intptr_t)arg;
    for (int i = 0; i < S13_LINES_PER; ++i) {
        cb_log(CB_LOG_INFO, "mt", "t=%d i=%d", id, i);
    }
    cb_thread_result_t r;
    r.info = CB_INFO_OK;
    r.result = NULL;
    return r;
}

static int section13_thread_safety(void)
{
    printf("--- Section 13: thread safety\n");

    cb_log_set_level(CB_LOG_INFO);
    cb_log_set_color(false);
    cb_log_set_timestamps(true);

    FILE *tmp = redirect_stderr_to_tmp();
    CHECK(tmp != NULL, "tmpfile");

    cb_thread_t threads[S13_THREADS];
    for (int i = 0; i < S13_THREADS; ++i) {
        threads[i] = cb_thread_create(NULL, s13_worker, (void *)(intptr_t)i);
        CHECK(threads[i].info == CB_INFO_OK, "thread create");
    }
    for (int i = 0; i < S13_THREADS; ++i) {
        cb_thread_result_t r = cb_thread_join(&threads[i]);
        CHECK(r.info == CB_INFO_OK, "thread join");
    }

    /* Read captured stderr. Lines are longer than 8 KiB/line would be, so
       compute: with timestamps, each line is ~50 bytes; 800 lines ~= 40 KiB.
       Budget 128 KiB. */
    static char buf[1 << 17];
    size_t n = slurp(tmp, buf, sizeof(buf));
    restore_stderr();
    fclose(tmp);

    /* Walk lines; verify each starts with '[' (timestamp) and '[' does NOT
       appear mid-line (interleaving detector), and count lines. */
    size_t lines = 0;
    size_t line_start = 0;
    for (size_t i = 0; i < n; ++i) {
        if (i == line_start) {
            CHECK(buf[i] == '[', "line must begin with '['");
        } else if (buf[i] == '[') {
            /* '[' appearing mid-line indicates a second line's prefix got
               interleaved into this one. */
            CHECK(0, "interleaving: '[' mid-line");
        }
        if (buf[i] == '\n') {
            lines++;
            line_start = i + 1;
        }
    }

    CHECK(lines == (size_t)S13_TOTAL_LINES, "exactly 800 well-formed lines");

    printf("  section 13 OK (%zu lines across %d threads)\n", lines, S13_THREADS);
    return 0;
}

/* ---------- Section 14: timestamps monotonic ---------- */

/* Parse the "[SSSSSSSSSS.uuuuuu]" prefix at the start of `line` into a
   nanosecond value. Returns true on success. */
static bool parse_ts(const char *line, uint64_t *out_ns)
{
    if (line[0] != '[') return false;
    /* Skip leading spaces. */
    size_t i = 1;
    while (line[i] == ' ') i++;

    uint64_t secs = 0;
    while (line[i] >= '0' && line[i] <= '9') {
        secs = secs * 10u + (uint64_t)(line[i] - '0');
        i++;
    }
    if (line[i] != '.') return false;
    i++;
    uint64_t us = 0;
    int fracd = 0;
    while (line[i] >= '0' && line[i] <= '9' && fracd < 6) {
        us = us * 10u + (uint64_t)(line[i] - '0');
        i++;
        fracd++;
    }
    if (fracd != 6) return false;
    if (line[i] != ']') return false;

    *out_ns = secs * 1000000000ull + us * 1000ull;
    return true;
}

static int section14_monotonic(void)
{
    printf("--- Section 14: timestamps monotonic\n");

    cb_log_set_level(CB_LOG_INFO);
    cb_log_set_color(false);
    cb_log_set_timestamps(true);

    FILE *tmp = redirect_stderr_to_tmp();
    CHECK(tmp != NULL, "tmpfile");

    for (int i = 0; i < 5; ++i) {
        cb_log(CB_LOG_INFO, "ts", "tick %d", i);
        cb_time_sleep_ms(1);
    }

    static char buf[8192];
    size_t n = slurp(tmp, buf, sizeof(buf));
    restore_stderr();
    fclose(tmp);

    uint64_t last = 0;
    size_t i = 0;
    size_t count = 0;
    while (i < n) {
        uint64_t ts = 0;
        CHECK(parse_ts(buf + i, &ts), "parse timestamp");
        CHECK(ts >= last, "timestamp monotonically non-decreasing");
        last = ts;
        count++;
        /* Skip to newline. */
        while (i < n && buf[i] != '\n') i++;
        if (i < n) i++;
    }
    CHECK(count == 5, "five log lines");

    printf("  section 14 OK (5 monotone timestamps)\n");
    return 0;
}

/* ---------- Section 15: truncation boundary ---------- */

/* Exercises the exact transition between "fits cleanly" and "marker tripped".
 *
 * Buffer layout (with timestamps off + module "t"):
 *   prefix  = level_tag(5) + ' ' + module_padded(6) + ' '   = 13 bytes
 *   reserve = strlen("...[TRUNC]") + 1 (newline)            = 11 bytes
 *   body_end_max         = sizeof(plain) - reserve          = 4085
 *   max clean body bytes = body_end_max - prefix            = 4072
 *   first truncated body = 4073
 */
static int section15_truncation_boundary(void)
{
    printf("--- Section 15: truncation boundary\n");

    cb_log_set_level(CB_LOG_INFO);
    cb_log_set_color(false);
    cb_log_set_timestamps(false);

    static char body_clean[4073];      /* 4072 + NUL */
    static char body_trunc[4074];      /* 4073 + NUL */
    memset(body_clean, 'x', sizeof(body_clean) - 1);
    body_clean[sizeof(body_clean) - 1] = '\0';
    memset(body_trunc, 'x', sizeof(body_trunc) - 1);
    body_trunc[sizeof(body_trunc) - 1] = '\0';

    /* Clean case: 4072-byte body should emit one full line ending in '\n'
       with no truncation marker. */
    {
        FILE *tmp = redirect_stderr_to_tmp();
        CHECK(tmp != NULL, "tmpfile clean");
        cb_log(CB_LOG_INFO, "t", "%s", body_clean);

        static char buf[8192];
        size_t n = slurp(tmp, buf, sizeof(buf));
        restore_stderr();
        fclose(tmp);

        /* Expected total: prefix(13) + body(4072) + '\n'(1) = 4086. */
        CHECK(n == 4086, "clean line is exactly 4086 bytes");
        CHECK(buf[n - 1] == '\n', "clean line ends with newline");
        CHECK(strstr(buf, "[TRUNC]") == NULL, "no truncation marker on clean line");
    }

    /* Truncated case: 4073-byte body should emit one line of exactly the full
       buffer size (4096) ending in "...[TRUNC]\n". */
    {
        FILE *tmp = redirect_stderr_to_tmp();
        CHECK(tmp != NULL, "tmpfile trunc");
        cb_log(CB_LOG_INFO, "t", "%s", body_trunc);

        static char buf[8192];
        size_t n = slurp(tmp, buf, sizeof(buf));
        restore_stderr();
        fclose(tmp);

        const char *trunc = "...[TRUNC]\n";
        size_t tl = strlen(trunc);
        CHECK(n == 4096, "truncated line is exactly 4096 bytes");
        CHECK(strcmp(buf + n - tl, trunc) == 0, "truncated line ends with ...[TRUNC]\\n");
        /* Marker must appear exactly once — no self-stomp. */
        CHECK(count_substr(buf, "[TRUNC]") == 1, "marker appears exactly once");
    }

    printf("  section 15 OK (clean=4086, truncated=4096)\n");
    return 0;
}

/* ---------- Section 16: sticky last_error ---------- */

static int section16_sticky_last_error(void)
{
    printf("--- Section 16: sticky last_error\n");

    cb_log_set_level(CB_LOG_INFO);
    cb_log_set_color(false);
    cb_log_set_timestamps(false);

    /* After a clear, last_error reports OK. */
    cb_log_clear_last_error();
    CHECK(cb_log_last_error() == CB_INFO_OK, "cleared error reads OK");

    /* A normal log call to a working stderr should not flip the sticky bit. */
    FILE *tmp = redirect_stderr_to_tmp();
    CHECK(tmp != NULL, "tmpfile s16");
    cb_log(CB_LOG_INFO, "t", "S16-normal");
    static char buf[256];
    slurp(tmp, buf, sizeof(buf));
    restore_stderr();
    fclose(tmp);
    CHECK(cb_log_last_error() == CB_INFO_OK, "successful log leaves last_error OK");
    CHECK(strstr(buf, "S16-normal") != NULL, "normal line still emitted");

    /* Idempotent clear. */
    cb_log_clear_last_error();
    cb_log_clear_last_error();
    CHECK(cb_log_last_error() == CB_INFO_OK, "double-clear is idempotent");

    printf("  section 16 OK\n");
    return 0;
}

/* ---------- main ---------- */

int main(void)
{
    /* Use stdout for framing so it stays visible through stderr redirection. */
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("=== cb_log tests ===\n");

    save_stderr_once();
    /* Initialize explicitly so the defaults (stderr tty autodetect) settle
       BEFORE we start redirecting stderr. */
    cb_log_init();

    int fails = 0;

    if (section1_default_filter())      { fails++; }
    if (section2_info_and_above())      { fails++; }
    if (section3_level_setter())        { fails++; }
    if (section4_module_tag())          { fails++; }
    if (section5_module_truncation())   { fails++; }
    if (section6_message_truncation())  { fails++; }
    if (section7_printf())              { fails++; }
    if (section8_timestamps_off())      { fails++; }
    if (section9_color_off())           { fails++; }
    if (section10_color_on())           { fails++; }
    if (section11_file_sink_dual())     { fails++; }
    if (section12_file_sink_detach())   { fails++; }
    if (section13_thread_safety())      { fails++; }
    if (section14_monotonic())          { fails++; }
    if (section15_truncation_boundary()){ fails++; }
    if (section16_sticky_last_error())  { fails++; }

    if (fails != 0) {
        fprintf(stdout, "FAIL: %d section(s) failed\n", fails);
        return 1;
    }

    printf("All log tests passed!\n");
    return 0;
}
