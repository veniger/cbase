#ifndef CBASE_H_INCLUDED
#define CBASE_H_INCLUDED

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
    This mini library is a C99 language code base for my projects to be ever expanded upon
    Use at your own risk!
*/

/* SEG Platform Detection */

#if defined(_WIN32) || defined(_WIN64)
    #define CB_PLATFORM_WINDOWS
#elif defined(__APPLE__) && defined(__MACH__)
    #define CB_PLATFORM_MACOS
#elif defined(__linux__)
    #define CB_PLATFORM_LINUX
#else
    #error "Unsupported platform"
#endif

#if defined(CB_PLATFORM_MACOS) || defined(CB_PLATFORM_LINUX)
    #define CB_PLATFORM_POSIX
#endif

/* SEG Info / Error Handling */

typedef enum
{
    /* General errors */
    CB_INFO_OK = 0,
    CB_INFO_GENERIC_ERROR,

    /* Threading errors */
    CB_INFO_THREAD_CREATE_FAILED,
    CB_INFO_THREAD_JOIN_FAILED,
    CB_INFO_THREAD_DETACH_FAILED,

    /* Mutex errors */
    CB_INFO_MUTEX_CREATE_FAILED,
    CB_INFO_MUTEX_DESTROY_FAILED,
    CB_INFO_MUTEX_LOCK_FAILED,
    CB_INFO_MUTEX_UNLOCK_FAILED,
    CB_INFO_MUTEX_TRYLOCK_FAILED,
    CB_INFO_MUTEX_BUSY,

    /* Condition variable errors */
    CB_INFO_COND_CREATE_FAILED,
    CB_INFO_COND_DESTROY_FAILED,
    CB_INFO_COND_WAIT_FAILED,
    CB_INFO_COND_SIGNAL_FAILED,

    /* Queue errors */
    CB_INFO_QUEUE_FULL,
    CB_INFO_QUEUE_EMPTY,
    CB_INFO_QUEUE_DESTROY_FAILED,

    /* Memory errors */
    CB_INFO_ALLOC_FAILED,
    CB_INFO_ARENA_OUT_OF_MEMORY,

    /* Network errors */
    CB_INFO_NET_INIT_FAILED,
    CB_INFO_NET_WOULD_BLOCK,
    CB_INFO_NET_ADDR_INVALID,
    CB_INFO_NET_SOCKET_FAILED,
    CB_INFO_NET_BIND_FAILED,
    CB_INFO_NET_LISTEN_FAILED,
    CB_INFO_NET_ACCEPT_FAILED,
    CB_INFO_NET_CONNECT_FAILED,
    CB_INFO_NET_SEND_FAILED,
    CB_INFO_NET_RECV_FAILED,
    CB_INFO_NET_CLOSED,
    CB_INFO_NET_POLL_FAILED,
    CB_INFO_NET_NONBLOCK_FAILED,

    /* Random errors */
    CB_INFO_RNG_BAD_STRIDE,

    /* Bytes errors */
    CB_INFO_BYTES_OUT_OF_BOUNDS,
    CB_INFO_BYTES_FRAME_TOO_LARGE,

    /* Time errors */
    CB_INFO_TIME_BAD_TICK_HZ,
    CB_INFO_TIME_TICK_LAG,

    /* Config errors */
    CB_INFO_CONFIG_PARSE_ERROR,       /* malformed key/value syntax */
    CB_INFO_CONFIG_BAD_KEY,           /* key fails [A-Za-z_][A-Za-z0-9_.-]* regex */
    CB_INFO_CONFIG_UNTERMINATED_STRING,
    CB_INFO_CONFIG_BAD_ESCAPE,
    CB_INFO_CONFIG_LINE_TOO_LONG,
    CB_INFO_CONFIG_FILE_OPEN_FAILED,
    CB_INFO_CONFIG_FILE_TOO_LARGE,    /* >16 MiB (cap on parse_file) */
    CB_INFO_CONFIG_ALLOC_FAILED,
    CB_INFO_CONFIG_KEY_NOT_FOUND,
    CB_INFO_CONFIG_BAD_INT,
    CB_INFO_CONFIG_BAD_BOOL,

} cb_info_t;

/* SEG Memory / Arena Allocator */

typedef enum
{
    CB_ARENA_FIXED = 0,
    CB_ARENA_LINEAR,
    CB_ARENA_EXPONENTIAL,
} cb_arena_strategy_t;

typedef struct cb__arena_block_t cb__arena_block_t;

typedef struct
{
    cb_info_t info;
    cb_arena_strategy_t strategy;
    size_t initial_size;
    cb__arena_block_t *head;
    cb__arena_block_t *current;
} cb_arena_t;

typedef struct
{
    cb_info_t info;
    void *ptr;
} cb_arena_alloc_result_t;

cb_arena_t              cb_arena_create(size_t size, cb_arena_strategy_t strategy);
void                    cb_arena_destroy(cb_arena_t *arena);
cb_arena_alloc_result_t cb_arena_alloc(cb_arena_t *arena, size_t size, size_t align);
void                    cb_arena_reset(cb_arena_t *arena);
bool                    cb_arena_check_health(cb_arena_t *arena);

/* Internal alloc/free helpers: if arena is NULL, fall back to malloc/free */
void *cb__alloc(cb_arena_t *arena, size_t size, size_t align);
void  cb__free(cb_arena_t *arena, void *ptr);

/* SEG Math */

/* SEG Fixed-Point Math */

/*
    Deterministic fixed-point math for games/physics.
    - cb_fx16_t: Q16.16 signed (int32 backing). Range ~ [-32768, +32768).
    - cb_fx32_t: Q32.32 signed (int64 backing). Range ~ [-2^31, +2^31).
    - cb_brad_t: binary radians, uint16. 65536 brad = full turn, wraps naturally.

    All arithmetic saturates on overflow. Division by zero saturates
    sign-correct (0/0 -> 0 by convention). sqrt of negative -> 0.
    atan2(0,0) -> 0.

    sin/cos/sqrt/atan2 are LUT-backed and bit-exact deterministic across
    platforms. The float/double conversion helpers are the ONLY source of
    floating-point math in this module and are tooling-only (do not use
    them in simulation paths).
*/

typedef int32_t  cb_fx16_t;
typedef int64_t  cb_fx32_t;
typedef uint16_t cb_brad_t;

#define CB_FX16_ONE     ((cb_fx16_t)65536)
#define CB_FX16_HALF    ((cb_fx16_t)32768)
#define CB_FX16_MAX     ((cb_fx16_t)INT32_MAX)
#define CB_FX16_MIN     ((cb_fx16_t)INT32_MIN)
#define CB_FX32_ONE     ((cb_fx32_t)(INT64_C(1) << 32))
#define CB_FX32_MAX     ((cb_fx32_t)INT64_MAX)
#define CB_FX32_MIN     ((cb_fx32_t)INT64_MIN)
#define CB_BRAD_FULL    ((cb_brad_t)0)
#define CB_BRAD_HALF    ((cb_brad_t)32768)
#define CB_BRAD_QUARTER ((cb_brad_t)16384)

/* --- Q16.16 --- */

cb_fx16_t cb_fx16_from_int(int32_t v);
int32_t   cb_fx16_to_int(cb_fx16_t v);
cb_fx16_t cb_fx16_from_float(float v);
float     cb_fx16_to_float(cb_fx16_t v);

cb_fx16_t cb_fx16_add(cb_fx16_t a, cb_fx16_t b);
cb_fx16_t cb_fx16_sub(cb_fx16_t a, cb_fx16_t b);
cb_fx16_t cb_fx16_mul(cb_fx16_t a, cb_fx16_t b);
cb_fx16_t cb_fx16_div(cb_fx16_t a, cb_fx16_t b);
cb_fx16_t cb_fx16_abs(cb_fx16_t v);
cb_fx16_t cb_fx16_min(cb_fx16_t a, cb_fx16_t b);
cb_fx16_t cb_fx16_max(cb_fx16_t a, cb_fx16_t b);
cb_fx16_t cb_fx16_clamp(cb_fx16_t v, cb_fx16_t lo, cb_fx16_t hi);
cb_fx16_t cb_fx16_lerp(cb_fx16_t a, cb_fx16_t b, cb_fx16_t t);
cb_fx16_t cb_fx16_sqrt(cb_fx16_t v);

cb_fx16_t cb_fx16_sin(cb_brad_t angle);
cb_fx16_t cb_fx16_cos(cb_brad_t angle);
cb_brad_t cb_fx16_atan2(cb_fx16_t y, cb_fx16_t x);

/* --- Q32.32 --- */

cb_fx32_t cb_fx32_from_int(int64_t v);
int64_t   cb_fx32_to_int(cb_fx32_t v);
cb_fx32_t cb_fx32_from_float(double v);
double    cb_fx32_to_double(cb_fx32_t v);

cb_fx32_t cb_fx32_add(cb_fx32_t a, cb_fx32_t b);
cb_fx32_t cb_fx32_sub(cb_fx32_t a, cb_fx32_t b);
cb_fx32_t cb_fx32_mul(cb_fx32_t a, cb_fx32_t b);
cb_fx32_t cb_fx32_div(cb_fx32_t a, cb_fx32_t b);
cb_fx32_t cb_fx32_abs(cb_fx32_t v);
cb_fx32_t cb_fx32_sqrt(cb_fx32_t v);

cb_fx32_t cb_fx32_from_fx16(cb_fx16_t v);
cb_fx16_t cb_fx16_from_fx32(cb_fx32_t v);

/* SEG Random */

/*
    Deterministic seeded PRNG for the 2D extraction game sim.

    Algorithm: PCG32 (O'Neill, 2014). 64-bit LCG state driven by a
    per-stream odd increment, with an XSH-RR output permutation on the
    top bits. Output is 32-bit per step.

    Contract:
    - All sim randomness flows through cb_rng_*.
    - Identical (seed, stream) pair produces bit-identical output on
      every supported target (Windows / Linux / macOS, x86_64 / ARM64).
    - No floats, no libm, no global state.
    - cb_rng_seed ALWAYS succeeds; info = CB_INFO_OK.
    - Shuffle is the only operation that can fail (CB_INFO_RNG_BAD_STRIDE
      when the element stride exceeds the internal scratch cap).

    Stream selector:
    - The second argument to cb_rng_seed picks an independent stream in
      the PCG32 stream family. Use it to give each logical source of
      randomness its own sequence (per-zone, per-match-module, replay
      slot), so reordering draws in one stream cannot desynchronize
      another.
*/

typedef struct
{
    cb_info_t info;
    uint64_t  state;
    uint64_t  inc;    /* must be odd; lowest bit is forced to 1 by cb_rng_seed */
} cb_rng_t;

/* Maximum per-element stride supported by cb_rng_shuffle's stack scratch.
   Shuffling an array whose stride exceeds this flags CB_INFO_RNG_BAD_STRIDE
   and leaves the array untouched. */
#define CB_RNG_SHUFFLE_MAX_STRIDE ((size_t)256)

/* --- Core --- */

cb_rng_t  cb_rng_seed(uint64_t seed, uint64_t stream);
uint32_t  cb_rng_u32(cb_rng_t *rng);
uint64_t  cb_rng_u64(cb_rng_t *rng);          /* (high u32, low u32) in draw order */

/* --- Bounded integers (unbiased) --- */

uint32_t  cb_rng_u32_below(cb_rng_t *rng, uint32_t bound);      /* [0, bound). bound==0 -> 0, no state advance. */
int32_t   cb_rng_i32_range(cb_rng_t *rng, int32_t lo, int32_t hi_inclusive); /* [lo, hi]. hi<lo -> lo. */

/* --- Fixed-point & angles (integrate with cb_fixed) --- */

cb_fx16_t cb_rng_fx16_unit(cb_rng_t *rng);                       /* [0, CB_FX16_ONE) */
cb_fx16_t cb_rng_fx16_range(cb_rng_t *rng, cb_fx16_t lo, cb_fx16_t hi); /* [lo, hi). hi<=lo -> lo. */
cb_brad_t cb_rng_brad(cb_rng_t *rng);                            /* uniform over full turn */

/* --- Bool / chance --- */

bool      cb_rng_bool(cb_rng_t *rng);
bool      cb_rng_chance_fx16(cb_rng_t *rng, cb_fx16_t probability); /* p<=0 -> false, p>=ONE -> true */

/* --- Shuffle (Fisher-Yates, in place, deterministic) --- */

/* Uses a stride-sized scratch buffer on the stack (cap = CB_RNG_SHUFFLE_MAX_STRIDE).
   If stride > cap, sets rng->info = CB_INFO_RNG_BAD_STRIDE and returns without
   touching the array. n <= 1 is a no-op. */
void      cb_rng_shuffle(cb_rng_t *rng, void *base, size_t n, size_t stride);

/* SEG Bytes */

/*
    Bounded byte-buffer reader/writer with a tiny length-prefix frame helper.

    - No allocations. All functions operate on caller-provided buffers.
    - All multi-byte integers are encoded and decoded little-endian on every
      host; the code uses explicit byte shifts rather than assuming host
      byte order, so it is correct on any platform.
    - Sticky info: once a writer's or reader's info is non-OK (out-of-bounds,
      frame too large), every subsequent op short-circuits and keeps
      returning that same info without mutating pos. This lets callers run
      a chain of writes or reads and inspect info once at the end.
    - On error, info is set AND the error is returned. pos is never advanced
      on a failed op.

    Frame helpers use a u16 (little-endian) length prefix. begin_frame_u16
    writes two placeholder bytes and records the offset; end_frame_u16
    patches those bytes with the body length. read_frame_u16 decodes the
    length and produces a sub-reader bounded to exactly that many bytes.

    cb_bytes_reader_remaining returns 0 on a poisoned reader (info != OK)
    so the "drained" view matches the sticky-info contract.

    cb_bytes_begin_frame_u16 zeroes *out_mark on every error-return path
    (sticky-info short-circuit, write failure); callers can rely on
    *out_mark == 0 after a failed call.
*/

typedef struct
{
    cb_info_t  info;
    uint8_t   *data;
    size_t     cap;
    size_t     pos;
} cb_bytes_writer_t;

typedef struct
{
    cb_info_t      info;
    const uint8_t *data;
    size_t         len;
    size_t         pos;
} cb_bytes_reader_t;

/* --- Writer --- */

cb_bytes_writer_t cb_bytes_writer_init(uint8_t *buf, size_t cap);

cb_info_t cb_bytes_write_u8    (cb_bytes_writer_t *w, uint8_t  v);
cb_info_t cb_bytes_write_u16_le(cb_bytes_writer_t *w, uint16_t v);
cb_info_t cb_bytes_write_u32_le(cb_bytes_writer_t *w, uint32_t v);
cb_info_t cb_bytes_write_u64_le(cb_bytes_writer_t *w, uint64_t v);

cb_info_t cb_bytes_write_i8    (cb_bytes_writer_t *w, int8_t  v);
cb_info_t cb_bytes_write_i16_le(cb_bytes_writer_t *w, int16_t v);
cb_info_t cb_bytes_write_i32_le(cb_bytes_writer_t *w, int32_t v);
cb_info_t cb_bytes_write_i64_le(cb_bytes_writer_t *w, int64_t v);

cb_info_t cb_bytes_write_bytes(cb_bytes_writer_t *w, const void *src, size_t n);

size_t    cb_bytes_writer_len(const cb_bytes_writer_t *w);

/* --- Reader --- */

cb_bytes_reader_t cb_bytes_reader_init(const uint8_t *buf, size_t len);

cb_info_t cb_bytes_read_u8    (cb_bytes_reader_t *r, uint8_t  *out);
cb_info_t cb_bytes_read_u16_le(cb_bytes_reader_t *r, uint16_t *out);
cb_info_t cb_bytes_read_u32_le(cb_bytes_reader_t *r, uint32_t *out);
cb_info_t cb_bytes_read_u64_le(cb_bytes_reader_t *r, uint64_t *out);

cb_info_t cb_bytes_read_i8    (cb_bytes_reader_t *r, int8_t  *out);
cb_info_t cb_bytes_read_i16_le(cb_bytes_reader_t *r, int16_t *out);
cb_info_t cb_bytes_read_i32_le(cb_bytes_reader_t *r, int32_t *out);
cb_info_t cb_bytes_read_i64_le(cb_bytes_reader_t *r, int64_t *out);

cb_info_t cb_bytes_read_bytes(cb_bytes_reader_t *r, void *out, size_t n);

size_t    cb_bytes_reader_remaining(const cb_bytes_reader_t *r);

/* --- Length-prefix frame helpers (u16, little-endian) --- */

cb_info_t cb_bytes_begin_frame_u16(cb_bytes_writer_t *w, size_t *out_mark);
cb_info_t cb_bytes_end_frame_u16  (cb_bytes_writer_t *w, size_t mark);
cb_info_t cb_bytes_read_frame_u16 (cb_bytes_reader_t *r, cb_bytes_reader_t *out_sub);

/* SEG Time */

/*
    Monotonic nanosecond clock + accumulator-based fixed-timestep loop helper.

    Clock:
    - cb_time_now_ns returns a monotonically non-decreasing nanosecond counter.
      Epoch is implementation-defined (NOT wall clock); only differences are
      meaningful. Safe to call from any thread; no global locks.
      POSIX / macOS: clock_gettime(CLOCK_MONOTONIC).
      Windows:       QueryPerformanceCounter + QueryPerformanceFrequency.
    - cb_time_now_us / cb_time_now_ms are thin wrappers for ergonomics.
    - cb_time_sleep_ms blocks the calling thread for approximately ms
      milliseconds (POSIX nanosleep / Win32 Sleep).

    Tick loop:
    - cb_tick_loop_t is a single-owner (not thread-safe) helper that turns
      real-time elapsed into an integer number of fixed-size simulation steps.
      The outer (render) loop calls cb_tick_loop_advance once per frame and
      advances the sim that many times.
    - Configure with a target tick rate (Hz) and a death-spiral cap: if the
      computed tick count exceeds max_ticks_per_call, the excess accumulator
      is thrown away and CB_INFO_TIME_TICK_LAG is reported on the step AND
      stored on loop->info so callers can inspect once at the end.
    - cb_tick_loop_alpha returns the fractional remainder of the accumulator
      in [0, CB_FX16_ONE - 1], useful as a render-side interpolation alpha.
*/

uint64_t cb_time_now_ns(void);
uint64_t cb_time_now_us(void);
uint64_t cb_time_now_ms(void);
void     cb_time_sleep_ms(uint32_t ms);

typedef struct
{
    cb_info_t info;
    uint64_t  tick_ns;             /* configured step duration in ns (0 on bad-hz init) */
    uint32_t  max_ticks_per_call;  /* death-spiral clamp; 0 means no clamp */
    uint64_t  last_time_ns;        /* timestamp of previous advance call */
    uint64_t  accumulator_ns;      /* elapsed ns not yet turned into ticks */
    uint64_t  sim_tick_index;      /* total ticks advanced since create/reset */
    bool      started;             /* false until the first advance call */
} cb_tick_loop_t;

typedef struct
{
    cb_info_t info;
    uint32_t  ticks_to_run;        /* number of sim steps the caller should run NOW */
    uint64_t  first_tick_index;    /* sim_tick_index of the FIRST step in this batch */
    uint64_t  leftover_ns;         /* accumulator remainder after the ticks subtract */
} cb_tick_loop_step_t;

cb_tick_loop_t      cb_tick_loop_create(uint32_t tick_hz, uint32_t max_ticks_per_call);
cb_tick_loop_step_t cb_tick_loop_advance(cb_tick_loop_t *loop);
cb_fx16_t           cb_tick_loop_alpha(const cb_tick_loop_t *loop);
void                cb_tick_loop_reset(cb_tick_loop_t *loop);

/* SEG Config */

/*
    Minimal key/value config parser. Flat namespace, no sections.

    Grammar:
      - Blank lines ignored.
      - Lines starting with '#' or ';' (after leading whitespace) are
        full-line comments.
      - Otherwise: split on FIRST '='. LHS is the key, RHS is the value.
      - Keys must match [A-Za-z_][A-Za-z0-9_.-]* (dot and dash allowed for
        namespacing like "net.udp.port"). Empty key -> parse error.
      - Values:
          * Leading/trailing whitespace stripped.
          * If the stripped value starts with '"', the value is the contents
            between the first '"' and the matching closing '"' on the same
            line. Backslash escapes inside quotes: \", \\, \n, \t, \r.
            Any other \x is an error.
          * If NOT quoted, an unescaped '#' or ';' starts a trailing comment
            and is stripped; the remaining characters are the value (then
            trailing whitespace is re-stripped). Backslash has no special
            meaning in unquoted values.
      - Duplicate keys: the LATER value wins (typical override semantics).
      - Keys are case-sensitive.
      - Line length cap: 4096 bytes. Exceeding it -> CB_INFO_CONFIG_LINE_TOO_LONG
        with the 1-based line number recorded on the returned struct.

    Storage:
      - Entries stored in a singly-linked list in parse order. Lookup is O(n);
        config files are tiny, so no hash table.
      - Key and value strings are duplicated into freshly cb__alloc'ed buffers
        (null-terminated). On duplicate key, the value buffer is replaced.

    Allocation:
      - Follows the cbase convention: arena != NULL uses the arena, arena == NULL
        falls back to malloc/free.
      - cb_config_destroy is safe on a failed parse and safe on both allocator
        modes.

    Typed getters:
      - Integer parsing is base-10 only, no hex, no oct, no underscores.
      - i64 accepts optional leading '-'. u64 does not.
      - Bool accepts true/false, yes/no, on/off, 1/0 (case-insensitive).
      - Missing key -> CB_INFO_CONFIG_KEY_NOT_FOUND + fallback.
      - Unparseable    -> CB_INFO_CONFIG_BAD_INT / CB_INFO_CONFIG_BAD_BOOL + fallback.
      - Present + parseable -> CB_INFO_OK + parsed value.
*/

typedef struct cb__config_entry_t cb__config_entry_t;

typedef struct
{
    cb_info_t           info;
    uint32_t            error_line;       /* 1-based on parse error; 0 on success */
    uint32_t            count;            /* number of unique keys */
    cb_arena_t         *arena;            /* nullable; same allocate-on-arena-or-malloc convention */
    cb__config_entry_t *head;             /* opaque linked list; internal */
} cb_config_t;

/* Load from a null-terminated text buffer. `len` is the number of bytes to
   consume (the buffer does not need to end with '\0'; len is authoritative). */
cb_config_t cb_config_parse(cb_arena_t *arena, const char *text, size_t len);

/* Load from a file on disk. Returns CB_INFO_CONFIG_FILE_OPEN_FAILED on open
   error. Files larger than 16 MiB error with CB_INFO_CONFIG_FILE_TOO_LARGE. */
cb_config_t cb_config_parse_file(cb_arena_t *arena, const char *path);

/* Frees all entries (via cb__free per entry). Safe on a failed parse. */
void        cb_config_destroy(cb_config_t *cfg);

/* Lookup. Returns NULL if absent. Pointer is stable until destroy / reparse. */
const char *cb_config_get(const cb_config_t *cfg, const char *key);

/* Typed getters. If key missing or value unparseable, write fallback into
   the value field and return the corresponding info code. */
typedef struct { cb_info_t info; int64_t  value; } cb_config_i64_t;
typedef struct { cb_info_t info; uint64_t value; } cb_config_u64_t;
typedef struct { cb_info_t info; bool     value; } cb_config_bool_t;

cb_config_i64_t  cb_config_get_i64 (const cb_config_t *cfg, const char *key, int64_t  fallback);
cb_config_u64_t  cb_config_get_u64 (const cb_config_t *cfg, const char *key, uint64_t fallback);
cb_config_bool_t cb_config_get_bool(const cb_config_t *cfg, const char *key, bool     fallback);

/* SEG System Stuff */

/* SEG Threading */

#ifdef CB_PLATFORM_POSIX
    #include <pthread.h>
#else
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#endif

/* --- Thread --- */

typedef struct
{
    cb_info_t info;
    void *result;
} cb_thread_result_t;

typedef cb_thread_result_t (*cb_thread_function_t)(void *arg);

typedef struct
{
    cb_info_t info;
    void *cb__internal;
#ifdef CB_PLATFORM_POSIX
    pthread_t handle;
#else
    HANDLE handle;
#endif
} cb_thread_t;

cb_thread_t         cb_thread_create(cb_arena_t *arena, cb_thread_function_t fn, void *arg);
cb_thread_result_t  cb_thread_join(cb_thread_t *thread);
cb_info_t           cb_thread_detach(cb_thread_t *thread);

/* --- Mutex --- */

typedef struct
{
    cb_info_t info;
#ifdef CB_PLATFORM_POSIX
    pthread_mutex_t handle;
#else
    CRITICAL_SECTION handle;
#endif
} cb_mutex_t;

cb_mutex_t  cb_mutex_create(void);
cb_info_t   cb_mutex_destroy(cb_mutex_t *mutex);

cb_info_t   cb_mutex_lock(cb_mutex_t *mutex);
cb_info_t   cb_mutex_unlock(cb_mutex_t *mutex);
cb_info_t   cb_mutex_trylock(cb_mutex_t *mutex);

/* --- Condition Variable --- */

typedef struct
{
    cb_info_t info;
#ifdef CB_PLATFORM_POSIX
    pthread_cond_t handle;
#else
    CONDITION_VARIABLE handle;
#endif
} cb_cond_t;

cb_cond_t   cb_cond_create(void);
cb_info_t   cb_cond_destroy(cb_cond_t *cond);

cb_info_t   cb_cond_wait(cb_cond_t *cond, cb_mutex_t *mutex);
cb_info_t   cb_cond_signal(cb_cond_t *cond);
cb_info_t   cb_cond_broadcast(cb_cond_t *cond);

/* --- Thread-Safe Queue --- */

typedef struct
{
    cb_info_t info;
    void **items;
    uint32_t capacity;
    uint32_t count;
    uint32_t head;
    uint32_t tail;
    cb_mutex_t mutex;
    cb_cond_t not_full;
    cb_cond_t not_empty;
} cb_tsqueue_t;

typedef struct
{
    cb_info_t info;
    void *data;
} cb_tsqueue_item_t;

cb_tsqueue_t        cb_tsqueue_create(cb_arena_t *arena, uint32_t capacity);
cb_info_t           cb_tsqueue_destroy(cb_arena_t *arena, cb_tsqueue_t *queue);

cb_info_t           cb_tsqueue_push(cb_tsqueue_t *queue, void *item);
cb_tsqueue_item_t   cb_tsqueue_pop(cb_tsqueue_t *queue);
cb_tsqueue_item_t   cb_tsqueue_try_pop(cb_tsqueue_t *queue);
uint32_t            cb_tsqueue_count(cb_tsqueue_t *queue);

/* SEG Network */

#ifdef CB_PLATFORM_WINDOWS
    /* winsock2.h must be included before windows.h was pulled in above;
       WIN32_LEAN_AND_MEAN (set in the threading section) prevents windows.h
       from including the legacy winsock.h, so order is safe. */
    #include <winsock2.h>
    #include <ws2tcpip.h>
    typedef SOCKET cb__sockfd_t;
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    typedef int cb__sockfd_t;
#endif

/* --- Address (IPv4, host-byte-order fields) --- */

typedef struct
{
    cb_info_t info;
    uint32_t  ip;    /* host byte order: 127.0.0.1 == 0x7F000001 */
    uint16_t  port;  /* host byte order */
} cb_net_addr_t;

cb_net_addr_t cb_net_addr_v4(const char *ip_dotted, uint16_t port);

/* --- Lifecycle --- */

cb_info_t cb_net_init(void);      /* WSAStartup on Windows, no-op on POSIX */
cb_info_t cb_net_shutdown(void);

/* --- Shared result types --- */

typedef struct
{
    cb_info_t info;   /* OK / WOULD_BLOCK / CLOSED / SEND_FAILED / RECV_FAILED */
    size_t    bytes;  /* bytes sent or received */
} cb_net_io_result_t;

/* --- UDP (connectionless datagrams) --- */

typedef struct
{
    cb_info_t     info;
    cb__sockfd_t  handle;
} cb_udp_socket_t;

typedef struct
{
    cb_info_t     info;
    cb_net_addr_t from;
    size_t        size;  /* bytes written to user buf */
} cb_udp_recv_result_t;

cb_udp_socket_t      cb_udp_open(uint16_t bind_port);   /* 0 = any/ephemeral */
cb_info_t            cb_udp_close(cb_udp_socket_t *s);
cb_net_io_result_t   cb_udp_send(cb_udp_socket_t *s, cb_net_addr_t to,
                                 const void *data, size_t size);
cb_udp_recv_result_t cb_udp_recv(cb_udp_socket_t *s, void *buf, size_t buf_size);

/* --- TCP (byte stream) --- */

typedef struct
{
    cb_info_t     info;
    cb__sockfd_t  handle;
} cb_tcp_socket_t;

typedef struct
{
    cb_info_t     info;
    cb__sockfd_t  handle;
} cb_tcp_listener_t;

cb_tcp_listener_t  cb_tcp_listen(uint16_t port, int backlog);
cb_info_t          cb_tcp_listener_close(cb_tcp_listener_t *l);
cb_tcp_socket_t    cb_tcp_accept(cb_tcp_listener_t *l);   /* non-blocking */
cb_tcp_socket_t    cb_tcp_connect(cb_net_addr_t to);      /* blocks until connected; non-blocking thereafter */
cb_info_t          cb_tcp_close(cb_tcp_socket_t *s);
cb_net_io_result_t cb_tcp_send(cb_tcp_socket_t *s, const void *data, size_t size);
cb_net_io_result_t cb_tcp_recv(cb_tcp_socket_t *s, void *buf, size_t buf_size);

/* --- Polling (wait for any of N sockets to be ready) --- */

typedef enum
{
    CB_NET_POLL_READ  = 1 << 0,
    CB_NET_POLL_WRITE = 1 << 1,
    CB_NET_POLL_ERROR = 1 << 2,
} cb_net_poll_flags_t;

typedef struct
{
    cb__sockfd_t handle;
    int          events;   /* requested CB_NET_POLL_* bits */
    int          revents;  /* returned CB_NET_POLL_* bits */
} cb_net_pollable_t;

cb_info_t cb_net_poll(cb_net_pollable_t *items, size_t count, int timeout_ms);

/* SEG IO */

#endif
