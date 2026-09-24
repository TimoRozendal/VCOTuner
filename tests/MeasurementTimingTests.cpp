#include <catch2/catch_test_macros.hpp>
#include "dsp/MeasurementTiming.h"

#include <cmath>

using namespace vcotuner;

TEST_CASE ("the timeout is never zero anywhere in the supported range")
{
    // The old expression roundToInt(expectedTime * 100) evaluated to 0 above
    // roughly MIDI 108, giving a measurement ~10 ms to finish - less than
    // typical MIDI plus audio round-trip latency.
    for (int midi = 0; midi <= 127; ++midi)
    {
        const double freq = 440.0 * std::pow (2.0, (midi - 69) / 12.0);
        for (int periods : { 10, 20, 50, 100, 200, 400 })
        {
            const int cycles = computeTimeoutCycles (freq, periods, 0.01, 0.3);
            INFO ("midi=" << midi << " periods=" << periods);
            REQUIRE (cycles >= 50);
            // Non-zero is not enough: the timeout has to actually cover the
            // time the measurement needs, or a re-derivation could be short
            // yet still clear the floor - the original bug's failure mode.
            REQUIRE (cycles * 0.01 >= (double) periods / freq);
        }
    }
}

TEST_CASE ("low pitches get a proportionally longer timeout")
{
    const int low  = computeTimeoutCycles (46.25,  400, 0.01, 0.3);
    const int high = computeTimeoutCycles (4186.0, 400, 0.01, 0.3);
    REQUIRE (low > high);
}

TEST_CASE ("the latency allowance is included")
{
    // numPeriods must be large enough that both results clear the 50-cycle
    // floor; at 440 Hz with the brief's original numPeriods=20, the raw
    // (unfloored) cycle counts are 10 and 40 - both still below the floor -
    // so both collapse to 50 and the comparison below is unsatisfiable by
    // any implementation that honours the documented floor.
    const int without = computeTimeoutCycles (440.0, 400, 0.01, 0.0);
    const int with    = computeTimeoutCycles (440.0, 400, 0.01, 0.3);
    REQUIRE (with > without);
}

TEST_CASE ("invalid input still yields a usable timeout")
{
    REQUIRE (computeTimeoutCycles (0.0,  20, 0.01, 0.3) >= 50);
    REQUIRE (computeTimeoutCycles (-1.0, 20, 0.01, 0.3) >= 50);
}
