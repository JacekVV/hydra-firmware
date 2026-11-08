// Unit tests for src/CpuUsage.h - the busy-time sampler behind CpuHwUsagePercent.
//
// The sampler takes micros() as an argument, so these tests drive it with a plain counter and never
// touch the Arduino clock. Each helper below simulates one second: busy from the start of the second
// for busyUs, then a sample at the end of it.
#include "Arduino.h"
#include "CpuUsage.h"
#include <cstdint>
#include <cstdlib>
#include <unity.h>

static const uint32_t SECOND_US = 1000000u;

// One second with `busyUs` of busy time at its start; returns the percentage sampled at its end.
static uint32_t feedSecond(CpuUsageSampler &s, uint32_t startUs, uint32_t busyUs)
{
    s.startBusy(startUs);
    s.endBusy(startUs + busyUs);
    return s.sample(startUs + SECOND_US);
}

void test_idle_system_reads_zero()
{
    CpuUsageSampler s;
    s.prime(0);

    uint32_t pct = 0;
    for (uint32_t i = 0; i < 60; i++) {
        pct = feedSecond(s, i * SECOND_US, 0);
    }
    TEST_ASSERT_EQUAL_UINT32(0, pct);
}

void test_fully_busy_system_reads_one_hundred()
{
    CpuUsageSampler s;
    s.prime(0);

    uint32_t pct = 0;
    for (uint32_t i = 0; i < 60; i++) {
        pct = feedSecond(s, i * SECOND_US, SECOND_US);
    }
    TEST_ASSERT_EQUAL_UINT32(100, pct);
}

void test_half_busy_system_reads_fifty()
{
    CpuUsageSampler s;
    s.prime(0);

    uint32_t pct = 0;
    for (uint32_t i = 0; i < 60; i++) {
        pct = feedSecond(s, i * SECOND_US, SECOND_US / 2);
    }
    TEST_ASSERT_EQUAL_UINT32(50, pct);
}

void test_nested_busy_spans_count_the_outer_span_once()
{
    // The radio handler runs inside the main loop's busy span. Inner start/end must not add time.
    CpuUsageSampler s;
    s.prime(0);

    uint32_t pct = 0;
    for (uint32_t i = 0; i < 60; i++) {
        const uint32_t t = i * SECOND_US;
        s.startBusy(t);
        s.startBusy(t + 100000);
        s.endBusy(t + 300000);
        s.endBusy(t + 500000); // outer span: 500 ms
        pct = s.sample(t + SECOND_US);
    }
    TEST_ASSERT_EQUAL_UINT32(50, pct);
}

void test_unbalanced_endBusy_is_ignored()
{
    CpuUsageSampler s;
    s.prime(0);
    s.endBusy(0); // no matching start: must not go negative or add time
    s.endBusy(SECOND_US);

    uint32_t pct = 0;
    for (uint32_t i = 0; i < 60; i++) {
        pct = s.sample((i + 1) * SECOND_US);
    }
    TEST_ASSERT_EQUAL_UINT32(0, pct);
}

void test_window_forgets_busy_seconds_older_than_a_minute()
{
    CpuUsageSampler s;
    s.prime(0);

    for (uint32_t i = 0; i < 60; i++) {
        feedSecond(s, i * SECOND_US, SECOND_US);
    }
    uint32_t pct = 0;
    for (uint32_t i = 60; i < 90; i++) {
        pct = feedSecond(s, i * SECOND_US, 0);
    }
    // Thirty idle seconds have replaced half of the window.
    TEST_ASSERT_EQUAL_UINT32(50, pct);
}

void test_long_sleep_counts_only_the_busy_time_before_it()
{
    // One busy second, then a single sample a minute later: the sleep adds no busy time.
    CpuUsageSampler s;
    s.prime(0);
    s.startBusy(0);
    s.endBusy(SECOND_US);

    const uint32_t pct = s.sample(60 * SECOND_US);
    TEST_ASSERT_EQUAL_UINT32(1, pct); // 1 s of 60 s is 1.66%, truncated
}

void test_stalled_sample_is_clamped_to_five_seconds()
{
    // A ten-second busy span in one sample is treated as five seconds, so one stall cannot pin the
    // figure. Five of sixty seconds is 8.33%.
    CpuUsageSampler s;
    s.prime(0);
    s.startBusy(0);
    s.endBusy(10 * SECOND_US);

    const uint32_t pct = s.sample(10 * SECOND_US);
    TEST_ASSERT_EQUAL_UINT32(8, pct);
}

void test_percent_never_exceeds_one_hundred()
{
    // Every sample is clamped to 5 s, so 60 of them sum to 300 s of a 60 s window: 500% before the cap.
    CpuUsageSampler s;
    s.prime(0);

    uint32_t pct = 0;
    for (uint32_t i = 1; i <= 60; i++) {
        const uint32_t t = (i - 1) * 10 * SECOND_US;
        s.startBusy(t);
        s.endBusy(t + 10 * SECOND_US);
        pct = s.sample(t + 10 * SECOND_US);
    }
    TEST_ASSERT_EQUAL_UINT32(100, pct);
}

void test_micros_wrap_does_not_skew_the_figure()
{
    // Start 1.5 s before the 32-bit micros() wrap; each second has 500 ms busy across the boundary.
    CpuUsageSampler s;
    const uint32_t start = 0xFFFFFFFFu - 1500000u;
    s.prime(start);

    uint32_t pct = 0;
    for (uint32_t i = 0; i < 60; i++) {
        pct = feedSecond(s, start + i * SECOND_US, SECOND_US / 2);
    }
    TEST_ASSERT_EQUAL_UINT32(50, pct);
}

void setup()
{
    UNITY_BEGIN();
    RUN_TEST(test_idle_system_reads_zero);
    RUN_TEST(test_fully_busy_system_reads_one_hundred);
    RUN_TEST(test_half_busy_system_reads_fifty);
    RUN_TEST(test_nested_busy_spans_count_the_outer_span_once);
    RUN_TEST(test_unbalanced_endBusy_is_ignored);
    RUN_TEST(test_window_forgets_busy_seconds_older_than_a_minute);
    RUN_TEST(test_long_sleep_counts_only_the_busy_time_before_it);
    RUN_TEST(test_stalled_sample_is_clamped_to_five_seconds);
    RUN_TEST(test_percent_never_exceeds_one_hundred);
    RUN_TEST(test_micros_wrap_does_not_skew_the_figure);
    exit(UNITY_END());
}

void loop() {}
