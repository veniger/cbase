/*
 * time_test.c - Tests for the cb_time module.
 *
 * Covers:
 *   1. Monotonic non-decreasing clock over 10000 samples.
 *   2. Unit conversions (ns -> us -> ms).
 *   3. cb_time_sleep_ms honored (>= ~45ms on a 50ms request).
 *   4. cb_tick_loop_create validates tick_hz == 0.
 *   5. First advance returns zero ticks and seeds last_time_ns.
 *   6. Integer ticks accumulate after a short sleep.
 *   7. Death-spiral clamp: max_ticks_per_call honored, excess accumulator dropped.
 *   8. cb_tick_loop_alpha stays in [0, 2^16) and is strictly positive on a
 *      partial-tick sleep.
 *   9. cb_tick_loop_reset re-initializes state.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "../cbase.h"

#define CHECK(cond, msg) do {                                                  \
    if (!(cond)) {                                                             \
        fprintf(stderr, "FAIL: %s  (at %s:%d)\n", (msg), __FILE__, __LINE__);  \
        return 1;                                                              \
    }                                                                          \
} while (0)

/* ---------- section 1: monotonic non-decreasing ---------- */

static int section1_monotonic(void)
{
    printf("--- Section 1: monotonic non-decreasing\n");

    uint64_t prev = cb_time_now_ns();
    for (int i = 0; i < 10000; ++i)
    {
        uint64_t cur = cb_time_now_ns();
        CHECK(cur >= prev, "clock went backwards");
        prev = cur;
    }

    printf("  PASS: section 1 monotonic\n");
    return 0;
}

/* ---------- section 2: unit conversions ---------- */

static int section2_units(void)
{
    printf("--- Section 2: unit conversions\n");

    /* We can't sample all three at the same instant, but ms/us/ns are all
       derived from the same underlying source — each re-reads the clock.
       Sample ns three times close together and verify the derived values
       fall in plausible bounds around ns/1000 and ns/1e6. */
    uint64_t ns_before = cb_time_now_ns();
    uint64_t us = cb_time_now_us();
    uint64_t ms = cb_time_now_ms();
    uint64_t ns_after = cb_time_now_ns();

    /* us should be within [ns_before/1000, ns_after/1000] (inclusive). */
    uint64_t us_lo = ns_before / 1000ull;
    uint64_t us_hi = ns_after  / 1000ull;
    CHECK(us >= us_lo && us <= us_hi, "us not in expected ns-derived range");

    uint64_t ms_lo = ns_before / 1000000ull;
    uint64_t ms_hi = ns_after  / 1000000ull;
    CHECK(ms >= ms_lo && ms <= ms_hi, "ms not in expected ns-derived range");

    printf("  PASS: section 2 units\n");
    return 0;
}

/* ---------- section 3: sleep honored ---------- */

static int section3_sleep(void)
{
    printf("--- Section 3: sleep honored\n");

    uint64_t t0 = cb_time_now_ns();
    cb_time_sleep_ms(50);
    uint64_t t1 = cb_time_now_ns();

    uint64_t elapsed_ns = t1 - t0;
    /* Allow generous CI slop: 45ms lower bound. */
    CHECK(elapsed_ns >= 45ull * 1000000ull, "sleep(50ms) returned too early");

    printf("  PASS: section 3 sleep (%llu ns elapsed)\n",
           (unsigned long long)elapsed_ns);
    return 0;
}

/* ---------- section 4: tick loop create validation ---------- */

static int section4_bad_hz(void)
{
    printf("--- Section 4: tick loop create validation\n");

    cb_tick_loop_t loop = cb_tick_loop_create(0, 10);
    CHECK(loop.info == CB_INFO_TIME_BAD_TICK_HZ, "hz=0 must be BAD_TICK_HZ");
    CHECK(loop.tick_ns == 0, "tick_ns must be 0 on bad hz");

    printf("  PASS: section 4 bad_hz\n");
    return 0;
}

/* ---------- section 5: first advance yields zero ticks ---------- */

static int section5_first_advance(void)
{
    printf("--- Section 5: first advance yields zero ticks\n");

    cb_tick_loop_t loop = cb_tick_loop_create(60, 0);
    CHECK(loop.info == CB_INFO_OK, "60Hz create must be OK");
    CHECK(loop.tick_ns > 0, "tick_ns must be > 0");
    CHECK(loop.started == false, "fresh loop must not be started");

    cb_tick_loop_step_t step = cb_tick_loop_advance(&loop);
    CHECK(step.info == CB_INFO_OK, "first advance info OK");
    CHECK(step.ticks_to_run == 0, "first advance must produce 0 ticks");
    CHECK(step.leftover_ns == 0, "first advance leftover must be 0");
    CHECK(step.first_tick_index == 0, "first advance first_tick_index == 0");
    CHECK(loop.started == true, "loop started flag must be set");
    CHECK(loop.last_time_ns != 0, "last_time_ns must be seeded");

    printf("  PASS: section 5 first_advance\n");
    return 0;
}

/* ---------- section 6: integer ticks accumulate ---------- */

static int section6_accumulate(void)
{
    printf("--- Section 6: integer ticks accumulate after sleep\n");

    /* 1000 Hz -> tick_ns = 1_000_000. Sleep ~5ms, expect ~5 ticks. */
    cb_tick_loop_t loop = cb_tick_loop_create(1000, 0);
    CHECK(loop.info == CB_INFO_OK, "1000Hz create OK");
    CHECK(loop.tick_ns == 1000000ull, "tick_ns == 1_000_000 at 1000Hz");

    /* First advance establishes baseline. */
    cb_tick_loop_step_t step0 = cb_tick_loop_advance(&loop);
    CHECK(step0.ticks_to_run == 0, "section 6 first advance is 0 ticks");

    cb_time_sleep_ms(5);

    cb_tick_loop_step_t step1 = cb_tick_loop_advance(&loop);
    CHECK(step1.info == CB_INFO_OK, "section 6 second advance info OK");
    CHECK(step1.ticks_to_run >= 3 && step1.ticks_to_run <= 100,
          "ticks_to_run should be in [3, 100] for ~5ms sleep");
    CHECK(step1.leftover_ns < loop.tick_ns, "leftover < tick_ns");
    CHECK(step1.first_tick_index == 0, "first_tick_index == 0 on first real batch");
    CHECK(loop.sim_tick_index == (uint64_t)step1.ticks_to_run,
          "sim_tick_index == running sum");

    /* A second sleep + advance should continue the index. */
    cb_time_sleep_ms(2);
    cb_tick_loop_step_t step2 = cb_tick_loop_advance(&loop);
    CHECK(step2.info == CB_INFO_OK, "third advance info OK");
    CHECK(step2.first_tick_index == (uint64_t)step1.ticks_to_run,
          "second batch first_tick_index continues from first batch");
    CHECK(loop.sim_tick_index ==
          (uint64_t)step1.ticks_to_run + (uint64_t)step2.ticks_to_run,
          "sim_tick_index tracks the running sum");

    printf("  PASS: section 6 accumulate (batch1=%u, batch2=%u)\n",
           step1.ticks_to_run, step2.ticks_to_run);
    return 0;
}

/* ---------- section 7: death-spiral clamp ---------- */

static int section7_clamp(void)
{
    printf("--- Section 7: death-spiral clamp\n");

    cb_tick_loop_t loop = cb_tick_loop_create(60, 5);
    CHECK(loop.info == CB_INFO_OK, "60Hz create OK");

    /* Seed manually: pretend last_time was 1 second ago (~60 ticks of lag). */
    loop.started      = true;
    loop.last_time_ns = cb_time_now_ns() - 1000000000ull;

    cb_tick_loop_step_t step = cb_tick_loop_advance(&loop);
    CHECK(step.info == CB_INFO_TIME_TICK_LAG, "clamp must flag TICK_LAG on step");
    CHECK(loop.info == CB_INFO_TIME_TICK_LAG, "clamp must flag TICK_LAG on loop");
    CHECK(step.ticks_to_run == 5, "clamp must cap ticks at max_ticks_per_call");
    CHECK(step.leftover_ns < loop.tick_ns,
          "leftover must be dropped to < tick_ns after clamp");
    CHECK(loop.accumulator_ns == step.leftover_ns,
          "accumulator must equal leftover after clamp");
    CHECK(loop.sim_tick_index == 5, "sim_tick_index advanced by clamped ticks only");

    printf("  PASS: section 7 clamp\n");
    return 0;
}

/* ---------- section 8: alpha in range ---------- */

static int section8_alpha(void)
{
    printf("--- Section 8: alpha in range\n");

    /* 100 Hz -> tick_ns = 10_000_000 (10ms). Sleep ~3ms to guarantee a
       non-zero sub-tick fractional accumulator before advancing. */
    cb_tick_loop_t loop = cb_tick_loop_create(100, 0);
    CHECK(loop.info == CB_INFO_OK, "100Hz create OK");

    (void)cb_tick_loop_advance(&loop); /* baseline */

    cb_time_sleep_ms(3);
    cb_tick_loop_step_t step = cb_tick_loop_advance(&loop);
    (void)step;

    cb_fx16_t alpha = cb_tick_loop_alpha(&loop);
    CHECK(alpha >= 0, "alpha must be non-negative");
    CHECK(alpha < ((cb_fx16_t)1 << 16), "alpha must be < 2^16");
    CHECK(alpha > 0, "alpha must be strictly positive after sub-tick sleep");
    CHECK(alpha < ((cb_fx16_t)1 << 16) - 1, "alpha must be strictly less than (2^16)-1");

    /* Sanity: alpha on a bad-hz loop returns 0. */
    cb_tick_loop_t bad = cb_tick_loop_create(0, 0);
    CHECK(cb_tick_loop_alpha(&bad) == 0, "alpha on bad-hz loop returns 0");

    printf("  PASS: section 8 alpha (alpha=%d)\n", (int)alpha);
    return 0;
}

/* ---------- section 9: reset re-initializes ---------- */

static int section9_reset(void)
{
    printf("--- Section 9: reset re-initializes\n");

    cb_tick_loop_t loop = cb_tick_loop_create(1000, 0);
    CHECK(loop.info == CB_INFO_OK, "1000Hz create OK");

    (void)cb_tick_loop_advance(&loop);
    cb_time_sleep_ms(5);
    cb_tick_loop_step_t step = cb_tick_loop_advance(&loop);
    CHECK(step.ticks_to_run > 0, "pre-reset produced some ticks");
    CHECK(loop.sim_tick_index > 0, "pre-reset sim_tick_index > 0");

    uint64_t saved_tick_ns = loop.tick_ns;
    uint32_t saved_cap     = loop.max_ticks_per_call;

    cb_tick_loop_reset(&loop);
    CHECK(loop.tick_ns == saved_tick_ns, "reset preserves tick_ns");
    CHECK(loop.max_ticks_per_call == saved_cap, "reset preserves max_ticks_per_call");
    CHECK(loop.last_time_ns == 0, "reset zeroes last_time_ns");
    CHECK(loop.accumulator_ns == 0, "reset zeroes accumulator_ns");
    CHECK(loop.sim_tick_index == 0, "reset zeroes sim_tick_index");
    CHECK(loop.started == false, "reset clears started flag");
    CHECK(loop.info == CB_INFO_OK, "reset restores info to OK");

    cb_tick_loop_step_t post = cb_tick_loop_advance(&loop);
    CHECK(post.ticks_to_run == 0, "post-reset first advance yields 0 ticks");
    CHECK(loop.sim_tick_index == 0, "post-reset sim_tick_index still 0");

    printf("  PASS: section 9 reset\n");
    return 0;
}

/* ---------- section 10: manual clock advances ticks ---------- */

static uint64_t g_manual_clock_ns = 0;
static uint64_t manual_clock(void *u) { (void)u; return g_manual_clock_ns; }

static int section10_manual_clock_advances(void)
{
    printf("--- Section 10: manual clock advances ticks\n");

    /* 1000 Hz -> tick_ns = 1_000_000 ns = 1 ms. */
    cb_tick_loop_t loop = cb_tick_loop_create(1000, 0);
    CHECK(loop.info == CB_INFO_OK, "1000Hz create OK");

    g_manual_clock_ns = 1000ull; /* arbitrary non-zero baseline */
    cb_tick_loop_set_clock(&loop, manual_clock, NULL);

    /* Baseline advance — uses the manual clock. */
    cb_tick_loop_step_t step0 = cb_tick_loop_advance(&loop);
    CHECK(step0.ticks_to_run == 0, "baseline advance yields 0 ticks");
    CHECK(loop.last_time_ns == 1000ull, "baseline seeds last_time_ns from manual clock");

    /* Jump forward 5 ms on the manual clock: expect exactly 5 ticks, 0 leftover. */
    g_manual_clock_ns = 1000ull + 5ull * 1000000ull;
    cb_tick_loop_step_t step1 = cb_tick_loop_advance(&loop);
    CHECK(step1.info == CB_INFO_OK, "advance info OK");
    CHECK(step1.ticks_to_run == 5, "5ms of manual clock == 5 ticks @ 1000Hz");
    CHECK(step1.leftover_ns == 0, "whole ms -> zero leftover");
    CHECK(loop.sim_tick_index == 5, "sim_tick_index == 5");

    /* Jump forward 2.5 ms: expect 2 ticks with 500_000 ns leftover. */
    g_manual_clock_ns += 2500000ull;
    cb_tick_loop_step_t step2 = cb_tick_loop_advance(&loop);
    CHECK(step2.ticks_to_run == 2, "2.5ms -> 2 ticks");
    CHECK(step2.leftover_ns == 500000ull, "leftover == 500_000 ns");
    CHECK(loop.sim_tick_index == 7, "sim_tick_index accumulates");

    /* No clock change -> no new ticks. */
    cb_tick_loop_step_t step3 = cb_tick_loop_advance(&loop);
    CHECK(step3.ticks_to_run == 0, "zero elapsed -> zero ticks");
    CHECK(loop.sim_tick_index == 7, "sim_tick_index unchanged");

    printf("  PASS: section 10 manual_clock_advances\n");
    return 0;
}

/* ---------- section 11: manual clock survives reset ---------- */

static int section11_manual_clock_survives_reset(void)
{
    printf("--- Section 11: manual clock survives reset\n");

    cb_tick_loop_t loop = cb_tick_loop_create(1000, 0);
    CHECK(loop.info == CB_INFO_OK, "create OK");

    g_manual_clock_ns = 10ull * 1000000ull;
    cb_tick_loop_set_clock(&loop, manual_clock, NULL);

    (void)cb_tick_loop_advance(&loop);               /* seed baseline */
    g_manual_clock_ns += 3ull * 1000000ull;
    cb_tick_loop_step_t pre = cb_tick_loop_advance(&loop);
    CHECK(pre.ticks_to_run == 3, "pre-reset produced ticks");

    cb_tick_loop_reset(&loop);
    CHECK(loop.clock_fn == manual_clock, "reset preserves clock_fn");

    /* A first advance post-reset must pick up the manual clock, not the
     * default, i.e. baseline == current g_manual_clock_ns, not cb_time_now_ns. */
    uint64_t baseline = g_manual_clock_ns;
    cb_tick_loop_step_t post0 = cb_tick_loop_advance(&loop);
    CHECK(post0.ticks_to_run == 0, "post-reset baseline advance yields 0 ticks");
    CHECK(loop.last_time_ns == baseline,
          "post-reset last_time_ns seeded from manual clock");

    /* Bump manual clock and verify ticks come out — if reset had dropped the
     * manual clock back to cb_time_now_ns, last_time_ns would be near
     * cb_time_now_ns() and a 1ms manual-clock bump would not yield a tick. */
    g_manual_clock_ns += 4ull * 1000000ull;
    cb_tick_loop_step_t post1 = cb_tick_loop_advance(&loop);
    CHECK(post1.ticks_to_run == 4, "post-reset advance uses manual clock");

    printf("  PASS: section 11 manual_clock_survives_reset\n");
    return 0;
}

/* ---------- section 12: default clock still works ---------- */

static int section12_default_clock_still_works(void)
{
    printf("--- Section 12: default clock still works without set_clock\n");

    /* 1000 Hz loop, no clock injection. */
    cb_tick_loop_t loop = cb_tick_loop_create(1000, 0);
    CHECK(loop.info == CB_INFO_OK, "create OK");
    CHECK(loop.clock_fn == NULL, "default clock_fn is NULL");

    cb_tick_loop_step_t s0 = cb_tick_loop_advance(&loop);
    CHECK(s0.ticks_to_run == 0, "first advance yields 0 ticks");
    CHECK(loop.last_time_ns != 0, "last_time_ns seeded from default clock");

    cb_time_sleep_ms(5);
    cb_tick_loop_step_t s1 = cb_tick_loop_advance(&loop);
    CHECK(s1.info == CB_INFO_OK, "second advance info OK");
    CHECK(s1.ticks_to_run >= 3, "at least 3 ticks after ~5ms real sleep");
    CHECK(loop.sim_tick_index == (uint64_t)s1.ticks_to_run,
          "sim_tick_index reflects real-time elapsed");

    /* set_clock(NULL) restores the default on a loop that had a manual clock. */
    g_manual_clock_ns = 100ull * 1000000ull;
    cb_tick_loop_set_clock(&loop, manual_clock, NULL);
    CHECK(loop.clock_fn == manual_clock, "manual clock installed");
    cb_tick_loop_set_clock(&loop, NULL, NULL);
    CHECK(loop.clock_fn == NULL, "set_clock(NULL) restores default");
    CHECK(loop.clock_user == NULL, "set_clock(NULL) clears user ptr");

    printf("  PASS: section 12 default_clock_still_works\n");
    return 0;
}

/* ---------- main ---------- */

int main(void)
{
    printf("=== cb_time tests ===\n");

    int fails = 0;

    if (section1_monotonic())      { fails++; }
    if (section2_units())          { fails++; }
    if (section3_sleep())          { fails++; }
    if (section4_bad_hz())         { fails++; }
    if (section5_first_advance())  { fails++; }
    if (section6_accumulate())     { fails++; }
    if (section7_clamp())          { fails++; }
    if (section8_alpha())          { fails++; }
    if (section9_reset())          { fails++; }
    if (section10_manual_clock_advances())        { fails++; }
    if (section11_manual_clock_survives_reset())  { fails++; }
    if (section12_default_clock_still_works())    { fails++; }

    if (fails != 0)
    {
        fprintf(stderr, "FAIL: %d section(s) failed\n", fails);
        return 1;
    }

    printf("All time tests passed!\n");
    return 0;
}
