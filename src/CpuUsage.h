#pragma once

#include <cstdint>

/*
  fw+: CPU usage for OnDemand node stats, kept free of Arduino so the test suite can drive it.

  The caller brackets busy work with startBusy()/endBusy() and calls sample() once a second, passing
  micros() in each case. Time between busy spans is idle, including light sleep. Calls nest: only
  the outermost startBusy()/endBusy() pair counts, so a radio handler inside the main loop is not
  counted twice.

  sample() folds the busy time since the previous sample into a 60-second ring and returns the busy
  share of that window in percent. A single sample is clamped to 5 s, so one stalled tick cannot
  pin the figure at 100%.

  Times are 32-bit microseconds. Subtracting in uint32_t makes the micros() wrap (about every 71
  minutes) harmless for any span shorter than the wrap.
*/
class CpuUsageSampler
{
  public:
    static constexpr uint8_t WINDOW_SAMPLES = 60;
    static constexpr uint64_t MAX_SAMPLE_US = 5ULL * 1000000ULL;

    void startBusy(uint32_t nowUs)
    {
        if (++busyDepth == 1) {
            busySinceUs = nowUs;
        }
    }

    void endBusy(uint32_t nowUs)
    {
        if (busyDepth <= 0) {
            busyDepth = 0;
            return;
        }
        if (--busyDepth == 0) {
            totalBusyUs += (uint32_t)(nowUs - busySinceUs);
        }
    }

    // Starts the first delta from now. Call once before the first sample() so the first sample does
    // not count everything since boot.
    void prime(uint32_t nowUs) { lastBusyUs = busyUsAt(nowUs); }

    // Returns 0..100.
    uint32_t sample(uint32_t nowUs)
    {
        const uint64_t busy = busyUsAt(nowUs);
        uint64_t deltaUs = busy - lastBusyUs;
        lastBusyUs = busy;
        if (deltaUs > MAX_SAMPLE_US) {
            deltaUs = MAX_SAMPLE_US;
        }

        history[index] = deltaUs;
        index = (index + 1) % WINDOW_SAMPLES;

        uint64_t sumUs = 0;
        for (uint8_t i = 0; i < WINDOW_SAMPLES; i++) {
            sumUs += history[i];
        }

        const uint64_t windowUs = (uint64_t)WINDOW_SAMPLES * 1000000ULL;
        const uint64_t pct = (sumUs * 100ULL) / windowUs;
        return pct > 100 ? 100 : (uint32_t)pct;
    }

  private:
    // Busy time up to nowUs, including a span that is still open.
    uint64_t busyUsAt(uint32_t nowUs) const
    {
        if (busyDepth > 0) {
            return totalBusyUs + (uint32_t)(nowUs - busySinceUs);
        }
        return totalBusyUs;
    }

    int busyDepth = 0;
    uint32_t busySinceUs = 0;
    uint64_t totalBusyUs = 0;
    uint64_t lastBusyUs = 0;
    uint64_t history[WINDOW_SAMPLES] = {0};
    uint8_t index = 0;
};
