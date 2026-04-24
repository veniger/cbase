/*
 * cbase_time.c - Monotonic nanosecond clock + fixed-timestep loop helper.
 *
 * Clock is lock-free on all platforms; cb_time_now_ns is safe to call from
 * any thread. cb_tick_loop_t is single-thread-owned by contract (no mutex).
 *
 * Platform sources of truth:
 *   POSIX/macOS : clock_gettime(CLOCK_MONOTONIC)
 *   Windows     : QueryPerformanceCounter + QueryPerformanceFrequency
 *
 * On Windows the performance-counter frequency is constant after boot, so we
 * cache it in a static initialized on first use. A single-threaded init race
 * is harmless since every thread will compute the same frequency value.
 *
 * Nanosecond math is done in uint64_t. QPC gives ticks, and we convert to ns
 * as (ticks / freq) * 1e9 + ((ticks % freq) * 1e9) / freq to avoid overflow.
 */

#include <stdint.h>
#include <stdbool.h>

#ifdef CB_PLATFORM_POSIX
    #include <time.h>
#endif

#ifdef CB_PLATFORM_WINDOWS
    /* windows.h is already included via cbase.h (threading segment). */
#endif

/* --- Clock --- */

#ifdef CB_PLATFORM_WINDOWS

static uint64_t cb__qpc_freq = 0;

static uint64_t cb__qpc_get_freq(void)
{
    uint64_t f = cb__qpc_freq;
    if (f == 0)
    {
        LARGE_INTEGER li;
        QueryPerformanceFrequency(&li);
        f = (uint64_t)li.QuadPart;
        cb__qpc_freq = f; /* benign race: every caller computes the same value */
    }
    return f;
}

uint64_t cb_time_now_ns(void)
{
    uint64_t freq = cb__qpc_get_freq();
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    uint64_t ticks = (uint64_t)li.QuadPart;
    /* Split to avoid overflow on long-running systems: ticks can easily
       exceed 2^64 / 1e9 after a few years uptime. */
    uint64_t whole = ticks / freq;
    uint64_t frac  = ticks % freq;
    return whole * 1000000000ull + (frac * 1000000000ull) / freq;
}

void cb_time_sleep_ms(uint32_t ms)
{
    Sleep((DWORD)ms);
}

#else /* CB_PLATFORM_POSIX */

uint64_t cb_time_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void cb_time_sleep_ms(uint32_t ms)
{
    struct timespec req;
    req.tv_sec  = (time_t)(ms / 1000u);
    req.tv_nsec = (long)((ms % 1000u) * 1000000u);
    /* Loop on EINTR so a signal cannot cut the sleep short. */
    struct timespec rem;
    while (nanosleep(&req, &rem) != 0)
    {
        req = rem;
    }
}

#endif

uint64_t cb_time_now_us(void)
{
    return cb_time_now_ns() / 1000ull;
}

uint64_t cb_time_now_ms(void)
{
    return cb_time_now_ns() / 1000000ull;
}

/* --- Tick loop --- */

cb_tick_loop_t cb_tick_loop_create(uint32_t tick_hz, uint32_t max_ticks_per_call)
{
    cb_tick_loop_t loop;
    loop.info               = CB_INFO_OK;
    loop.tick_ns            = 0;
    loop.max_ticks_per_call = max_ticks_per_call;
    loop.last_time_ns       = 0;
    loop.accumulator_ns     = 0;
    loop.sim_tick_index     = 0;
    loop.started            = false;

    if (tick_hz == 0)
    {
        loop.info = CB_INFO_TIME_BAD_TICK_HZ;
        return loop;
    }
    loop.tick_ns = 1000000000ull / (uint64_t)tick_hz;
    return loop;
}

cb_tick_loop_step_t cb_tick_loop_advance(cb_tick_loop_t *loop)
{
    cb_tick_loop_step_t step;
    step.info             = CB_INFO_OK;
    step.ticks_to_run     = 0;
    step.first_tick_index = loop->sim_tick_index;
    step.leftover_ns      = 0;

    uint64_t now = cb_time_now_ns();

    if (!loop->started)
    {
        loop->started      = true;
        loop->last_time_ns = now;
        /* First call establishes the baseline; no ticks yet. */
        return step;
    }

    /* Monotonic clock guarantees now >= last_time_ns, but be defensive. */
    uint64_t elapsed = (now >= loop->last_time_ns) ? (now - loop->last_time_ns) : 0;
    loop->last_time_ns = now;
    loop->accumulator_ns += elapsed;

    /* tick_ns > 0 is an invariant for a well-constructed loop; the guard in
       create() returns CB_INFO_TIME_BAD_TICK_HZ. Still, avoid div-by-zero. */
    if (loop->tick_ns == 0)
    {
        step.info             = CB_INFO_TIME_BAD_TICK_HZ;
        loop->info            = CB_INFO_TIME_BAD_TICK_HZ;
        step.first_tick_index = loop->sim_tick_index;
        return step;
    }

    uint64_t ticks64 = loop->accumulator_ns / loop->tick_ns;
    uint64_t leftover = loop->accumulator_ns - ticks64 * loop->tick_ns;

    bool clamped = false;
    if (loop->max_ticks_per_call != 0 && ticks64 > (uint64_t)loop->max_ticks_per_call)
    {
        ticks64 = (uint64_t)loop->max_ticks_per_call;
        clamped = true;
    }

    /* ticks64 now fits in uint32_t: either it came from the max_ticks cap,
       or max_ticks == 0 and we fit in uint32 only if the accumulator is
       reasonable. On a 32-bit-tick overflow we still clamp to UINT32_MAX as
       a sanity ceiling — callers that care should set max_ticks_per_call. */
    if (ticks64 > (uint64_t)UINT32_MAX)
    {
        ticks64 = (uint64_t)UINT32_MAX;
        clamped = true;
    }

    step.first_tick_index = loop->sim_tick_index;
    step.ticks_to_run     = (uint32_t)ticks64;
    loop->sim_tick_index += ticks64;

    if (clamped)
    {
        /* Death-spiral guard: drop the extra accumulator; keep only the
           sub-tick remainder so interpolation alpha stays meaningful. */
        loop->accumulator_ns = leftover;
        step.leftover_ns     = leftover;
        step.info            = CB_INFO_TIME_TICK_LAG;
        loop->info           = CB_INFO_TIME_TICK_LAG;
    }
    else
    {
        loop->accumulator_ns = leftover;
        step.leftover_ns     = leftover;
    }

    return step;
}

cb_fx16_t cb_tick_loop_alpha(const cb_tick_loop_t *loop)
{
    if (loop->tick_ns == 0) return 0;

    /* leftover is guaranteed < tick_ns by advance(), so (leftover << 16) / tick_ns
       is in [0, 2^16). Still clamp defensively. Widen to 64-bit before the
       shift to avoid losing high bits when tick_ns is large (e.g. 1 Hz). */
    uint64_t num = loop->accumulator_ns << 16;
    uint64_t q   = num / loop->tick_ns;
    uint64_t cap = ((uint64_t)1 << 16) - 1u;
    if (q > cap) q = cap;
    return (cb_fx16_t)q;
}

void cb_tick_loop_reset(cb_tick_loop_t *loop)
{
    loop->info           = (loop->tick_ns == 0) ? CB_INFO_TIME_BAD_TICK_HZ : CB_INFO_OK;
    loop->last_time_ns   = 0;
    loop->accumulator_ns = 0;
    loop->sim_tick_index = 0;
    loop->started        = false;
    /* tick_ns and max_ticks_per_call are preserved. */
}
