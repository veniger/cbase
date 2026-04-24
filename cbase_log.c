/*
 * cbase_log.c - Minimal process-global thread-safe structured logger.
 *
 * One global logger. stderr is the default sink (always on); an optional
 * secondary FILE* sink can be attached. Writes are serialized by a single
 * mutex; formatting into a per-call 4 KiB stack buffer is lock-free. Lazy
 * init under a pthread_once / InitOnceExecuteOnce gate; cb_log_init() is
 * the explicit alternative for callers that want deterministic init.
 *
 * Timestamps are seconds-since-init, computed with integer ns math and
 * printed as "[%10s.%06s]" — no float in the log path. ANSI color is
 * applied on stderr only, never on the file sink, to keep file logs clean.
 *
 * Overlong messages (> 4096 bytes incl. prefix) are truncated with a
 * trailing "...[TRUNC]" marker and still emitted; cb_log() never fails
 * visibly to callers.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#ifdef CB_PLATFORM_POSIX
    #include <unistd.h>
    #include <pthread.h>
#else
    /* windows.h is already included via cbase.h threading segment. */
    #include <io.h>
#endif

/* --- File-scoped state --- */

#define CB__LOG_BUF_SZ       4096u
#define CB__LOG_TRUNC_MARKER "...[TRUNC]"
#define CB__LOG_MODULE_COLS  6u
#define CB__LOG_MODULE_MAX   16u

static struct {
    bool             initialized;
    cb_log_level_t   min_level;
    bool             use_color;
    bool             timestamps;
    FILE            *file_sink;
    uint64_t         start_ns;
    cb_mutex_t       mu;
} cb__log = {
    false,
    CB_LOG_INFO,
    false,
    true,
    NULL,
    0,
    {CB_INFO_OK,
#ifdef CB_PLATFORM_POSIX
     {0}
#else
     {0}
#endif
    }
};

/* --- Lazy init (POSIX pthread_once / Windows InitOnceExecuteOnce) --- */

#ifdef CB_PLATFORM_POSIX

static pthread_once_t cb__log_once = PTHREAD_ONCE_INIT;

static bool cb__log_stderr_is_tty(void)
{
    return isatty(fileno(stderr)) != 0;
}

static void cb__log_do_init(void)
{
    cb__log.min_level   = CB_LOG_INFO;
    cb__log.use_color   = cb__log_stderr_is_tty();
    cb__log.timestamps  = true;
    cb__log.file_sink   = NULL;
    cb__log.start_ns    = cb_time_now_ns();
    cb__log.mu          = cb_mutex_create();
    cb__log.initialized = true;
}

static void cb__log_ensure_init(void)
{
    pthread_once(&cb__log_once, cb__log_do_init);
}

#else /* CB_PLATFORM_WINDOWS */

static INIT_ONCE cb__log_once = INIT_ONCE_STATIC_INIT;

static bool cb__log_stderr_is_tty(void)
{
    return _isatty(_fileno(stderr)) != 0;
}

static BOOL CALLBACK cb__log_do_init(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    cb__log.min_level   = CB_LOG_INFO;
    cb__log.use_color   = cb__log_stderr_is_tty();
    cb__log.timestamps  = true;
    cb__log.file_sink   = NULL;
    cb__log.start_ns    = cb_time_now_ns();
    cb__log.mu          = cb_mutex_create();
    cb__log.initialized = true;
    return TRUE;
}

static void cb__log_ensure_init(void)
{
    InitOnceExecuteOnce(&cb__log_once, cb__log_do_init, NULL, NULL);
}

#endif

void cb_log_init(void)
{
    cb__log_ensure_init();
}

/* --- Public setters --- */

void cb_log_set_level(cb_log_level_t min_level)
{
    cb__log_ensure_init();
    cb_mutex_lock(&cb__log.mu);
    cb__log.min_level = min_level;
    cb_mutex_unlock(&cb__log.mu);
}

cb_log_level_t cb_log_get_level(void)
{
    cb__log_ensure_init();
    cb_mutex_lock(&cb__log.mu);
    cb_log_level_t lvl = cb__log.min_level;
    cb_mutex_unlock(&cb__log.mu);
    return lvl;
}

void cb_log_set_color(bool enabled)
{
    cb__log_ensure_init();
    cb_mutex_lock(&cb__log.mu);
    cb__log.use_color = enabled;
    cb_mutex_unlock(&cb__log.mu);
}

void cb_log_set_timestamps(bool enabled)
{
    cb__log_ensure_init();
    cb_mutex_lock(&cb__log.mu);
    cb__log.timestamps = enabled;
    cb_mutex_unlock(&cb__log.mu);
}

cb_info_t cb_log_set_file_sink(FILE *f)
{
    cb__log_ensure_init();
    /* Documented as NOT thread-safe w.r.t. concurrent log calls. We still
       take the mutex so the pointer swap is atomic w.r.t. a simultaneous
       cb_log() that happens to slip in. */
    cb_mutex_lock(&cb__log.mu);
    cb__log.file_sink = f;
    cb_mutex_unlock(&cb__log.mu);
    return CB_INFO_OK;
}

/* --- Internal formatting helpers --- */

static const char *cb__log_level_tag(cb_log_level_t lvl)
{
    switch (lvl) {
        case CB_LOG_TRACE: return "TRACE";
        case CB_LOG_DEBUG: return "DEBUG";
        case CB_LOG_INFO:  return "INFO ";
        case CB_LOG_WARN:  return "WARN ";
        case CB_LOG_ERROR: return "ERROR";
        case CB_LOG_FATAL: return "FATAL";
        default:           return "?????";
    }
}

/* Color prefix/suffix pair. If no color wanted, both are "". */
static void cb__log_color_strs(cb_log_level_t lvl, const char **pre, const char **suf)
{
    switch (lvl) {
        case CB_LOG_TRACE: *pre = "\x1b[2m";    *suf = "\x1b[0m"; break;
        case CB_LOG_DEBUG: *pre = "\x1b[36m";   *suf = "\x1b[0m"; break;
        case CB_LOG_INFO:  *pre = "";           *suf = "";        break;
        case CB_LOG_WARN:  *pre = "\x1b[33m";   *suf = "\x1b[0m"; break;
        case CB_LOG_ERROR: *pre = "\x1b[31m";   *suf = "\x1b[0m"; break;
        case CB_LOG_FATAL: *pre = "\x1b[1;31m"; *suf = "\x1b[0m"; break;
        default:           *pre = "";           *suf = "";        break;
    }
}

/* Append a null-terminated string to buf[pos..cap), updating pos. Silently
   truncates on overflow; caller detects via the post-call truncated flag. */
static void cb__log_append(char *buf, size_t cap, size_t *pos, const char *s)
{
    size_t p = *pos;
    while (*s && p + 1 < cap) {
        buf[p++] = *s++;
    }
    *pos = p;
}

/* Print an unsigned integer right-aligned in a field of `width` spaces into
   buf[pos..cap). Used for the seconds part of the timestamp. */
static void cb__log_append_uint_pad(char *buf, size_t cap, size_t *pos,
                                     uint64_t v, size_t width)
{
    char tmp[24];
    size_t n = 0;
    if (v == 0) {
        tmp[n++] = '0';
    } else {
        while (v > 0 && n < sizeof(tmp)) {
            tmp[n++] = (char)('0' + (v % 10u));
            v /= 10u;
        }
    }
    /* Left-pad with spaces to `width`. */
    while (n < width && *pos + 1 < cap) {
        buf[(*pos)++] = ' ';
        width--;
    }
    /* Emit digits in reverse. */
    while (n > 0 && *pos + 1 < cap) {
        buf[(*pos)++] = tmp[--n];
    }
}

/* Print an unsigned integer zero-padded to exactly `width` digits (truncates
   MSB-first if the number needs more; we only use this for 6-digit microseconds
   so that's fine). */
static void cb__log_append_uint_zeropad(char *buf, size_t cap, size_t *pos,
                                         uint64_t v, size_t width)
{
    char tmp[24];
    size_t n = 0;
    if (v == 0) {
        tmp[n++] = '0';
    } else {
        while (v > 0 && n < sizeof(tmp)) {
            tmp[n++] = (char)('0' + (v % 10u));
            v /= 10u;
        }
    }
    /* Left-pad with zeros. */
    while (n < width && *pos + 1 < cap) {
        buf[(*pos)++] = '0';
        width--;
    }
    while (n > 0 && *pos + 1 < cap) {
        buf[(*pos)++] = tmp[--n];
    }
}

/* --- Core log call --- */

void cb_log(cb_log_level_t level, const char *module, const char *fmt, ...)
{
    cb__log_ensure_init();

    /* Snapshot config under the lock to avoid a level flip mid-format. */
    cb_mutex_lock(&cb__log.mu);
    cb_log_level_t min_level = cb__log.min_level;
    bool           use_color = cb__log.use_color;
    bool           timestamps = cb__log.timestamps;
    FILE          *file_sink = cb__log.file_sink;
    uint64_t       start_ns  = cb__log.start_ns;
    cb_mutex_unlock(&cb__log.mu);

    if (level < min_level || min_level >= CB_LOG_OFF) return;

    /* Two buffers: one with color wrapping for stderr, one plain for the file
       sink. We build them separately so the file log never carries ANSI.
       Each is CB__LOG_BUF_SZ bytes including the trailing '\0' room. */
    char plain[CB__LOG_BUF_SZ];
    size_t ppos = 0;
    bool truncated = false;

    /* 1. Optional timestamp "[SSSSSSSSSS.uuuuuu] " */
    if (timestamps) {
        uint64_t now_ns = cb_time_now_ns();
        uint64_t dt_ns  = (now_ns >= start_ns) ? (now_ns - start_ns) : 0;
        uint64_t secs   = dt_ns / 1000000000ull;
        uint64_t us     = (dt_ns % 1000000000ull) / 1000ull; /* microseconds */

        cb__log_append(plain, sizeof(plain), &ppos, "[");
        cb__log_append_uint_pad(plain, sizeof(plain), &ppos, secs, 10);
        cb__log_append(plain, sizeof(plain), &ppos, ".");
        cb__log_append_uint_zeropad(plain, sizeof(plain), &ppos, us, 6);
        cb__log_append(plain, sizeof(plain), &ppos, "] ");
    }

    /* 2. Level tag (5 cols) + space. */
    cb__log_append(plain, sizeof(plain), &ppos, cb__log_level_tag(level));
    cb__log_append(plain, sizeof(plain), &ppos, " ");

    /* 3. Module (left-aligned, 6 cols, truncated at 16). */
    if (module != NULL) {
        size_t mod_len = 0;
        while (module[mod_len] != '\0' && mod_len < CB__LOG_MODULE_MAX) mod_len++;
        for (size_t i = 0; i < mod_len && ppos + 1 < sizeof(plain); ++i) {
            plain[ppos++] = module[i];
        }
        /* Pad to CB__LOG_MODULE_COLS cols if shorter. */
        while (mod_len < CB__LOG_MODULE_COLS && ppos + 1 < sizeof(plain)) {
            plain[ppos++] = ' ';
            mod_len++;
        }
    } else {
        for (size_t i = 0; i < CB__LOG_MODULE_COLS && ppos + 1 < sizeof(plain); ++i) {
            plain[ppos++] = ' ';
        }
    }
    cb__log_append(plain, sizeof(plain), &ppos, " ");

    /* 4. Formatted message. */
    /* Leave room for "...[TRUNC]\n" + '\0'. */
    size_t reserve = strlen(CB__LOG_TRUNC_MARKER) + 1 + 1; /* marker + '\n' + '\0' */
    size_t msg_cap = (sizeof(plain) > reserve) ? (sizeof(plain) - reserve) : 0;
    if (ppos < msg_cap) {
        va_list ap;
        va_start(ap, fmt);
        int written = vsnprintf(plain + ppos, msg_cap - ppos, fmt, ap);
        va_end(ap);
        if (written < 0) {
            /* Formatter error: nothing to append. */
        } else if ((size_t)written >= msg_cap - ppos) {
            /* Message was truncated by vsnprintf. */
            ppos = msg_cap - 1; /* we had msg_cap - ppos - 1 usable chars */
            /* Actually vsnprintf wrote (msg_cap - ppos - 1) chars and a NUL at
               plain[msg_cap - 1]. Advance ppos to msg_cap - 1. */
            ppos = msg_cap - 1;
            truncated = true;
        } else {
            ppos += (size_t)written;
        }
    } else {
        truncated = true;
    }

    if (truncated) {
        /* Append truncation marker. */
        cb__log_append(plain, sizeof(plain), &ppos, CB__LOG_TRUNC_MARKER);
    }

    /* 5. Trailing newline. */
    if (ppos + 1 < sizeof(plain)) {
        plain[ppos++] = '\n';
    } else {
        /* Force newline at the very end. */
        plain[sizeof(plain) - 2] = '\n';
        ppos = sizeof(plain) - 1;
    }
    /* plain is now ppos bytes of content; we do NOT null-terminate because we
       use fwrite with an explicit length. */

    /* Build the colored stderr buffer if needed. */
    char colored[CB__LOG_BUF_SZ + 32];
    size_t cpos = 0;
    const char *cpre = "";
    const char *csuf = "";
    if (use_color) cb__log_color_strs(level, &cpre, &csuf);

    size_t cpre_len = strlen(cpre);
    size_t csuf_len = strlen(csuf);

    /* Write colored version: cpre + plain-without-final-newline + csuf + '\n'.
       If there is no color, we skip the wrap work and write `plain` directly. */
    const char *stderr_buf;
    size_t      stderr_len;
    if (use_color && cpre_len > 0) {
        if (cpre_len + ppos + csuf_len + 1 <= sizeof(colored)) {
            memcpy(colored + cpos, cpre, cpre_len); cpos += cpre_len;
            /* plain content minus its trailing '\n' */
            size_t body = (ppos > 0 && plain[ppos - 1] == '\n') ? (ppos - 1) : ppos;
            memcpy(colored + cpos, plain, body); cpos += body;
            memcpy(colored + cpos, csuf, csuf_len); cpos += csuf_len;
            colored[cpos++] = '\n';
            stderr_buf = colored;
            stderr_len = cpos;
        } else {
            /* Color wrap wouldn't fit; fall back to plain. */
            stderr_buf = plain;
            stderr_len = ppos;
        }
    } else {
        stderr_buf = plain;
        stderr_len = ppos;
    }

    /* Emit under the mutex so lines from concurrent threads don't interleave
       within a single fwrite. */
    cb_mutex_lock(&cb__log.mu);

    size_t w = fwrite(stderr_buf, 1, stderr_len, stderr);
    (void)w; /* short write is silently swallowed — logging must not destabilize callers. */
    fflush(stderr);

    if (file_sink != NULL) {
        size_t wf = fwrite(plain, 1, ppos, file_sink);
        (void)wf;
        fflush(file_sink);
    }

    cb_mutex_unlock(&cb__log.mu);
}
