// tests/MeasurementStatisticsTests.cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <cmath>
#include <vector>
#include "dsp/MeasurementStatistics.h"

using namespace vcotuner;
using Catch::Approx;

TEST_CASE ("a perfectly uniform period sequence has zero uncertainty")
{
    const std::vector<double> periods (20, 100.0);
    const auto fit = fitPeriod (periods.data(), (int) periods.size());

    REQUIRE (fit.valid);
    REQUIRE (fit.periodSamples  == Approx (100.0));
    REQUIRE (fit.periodStdError == Approx (0.0).margin (1e-9));
}

TEST_CASE ("slope standard error matches the analytic value")
{
    // Crossing times 0, 8, 19, 28, 40 -> slope 10, SSE 4, Sxx 10.
    const std::vector<double> periods { 8.0, 11.0, 9.0, 12.0 };
    const auto fit = fitPeriod (periods.data(), (int) periods.size());

    REQUIRE (fit.valid);
    REQUIRE (fit.periodSamples  == Approx (10.0).margin (1e-9));
    REQUIRE (fit.periodStdError == Approx (std::sqrt (2.0 / 15.0)).margin (1e-9));
}

TEST_CASE ("uncertainty shrinks as more periods are collected")
{
    auto jittered = [] (int count)
    {
        std::vector<double> p ((size_t) count);
        for (int i = 0; i < count; ++i)
            p[(size_t) i] = 100.0 + ((i % 2 == 0) ? 0.5 : -0.5);
        return p;
    };

    const auto few  = jittered (10);
    const auto many = jittered (200);

    const auto fitFew  = fitPeriod (few.data(),  (int) few.size());
    const auto fitMany = fitPeriod (many.data(), (int) many.size());

    REQUIRE (fitMany.periodStdError < fitFew.periodStdError);
}

TEST_CASE ("degenerate inputs return defined values, never NaN")
{
    const double one[] = { 100.0 };

    for (auto fit : { fitPeriod (nullptr, 0), fitPeriod (one, 1) })
    {
        REQUIRE_FALSE (fit.valid);
        REQUIRE (std::isfinite (fit.periodSamples));
        REQUIRE (std::isfinite (fit.periodStdError));
    }
}

TEST_CASE ("two periods is the boundary case: one degree of freedom, still finite")
{
    // Two periods give three crossing times, so n - 2 == 1: the only
    // degenerate input (besides 0 and 1 periods) that actually reaches the
    // sse/((n-2)*sxx) division rather than returning early. The controller's
    // ruling is that a 1-degree-of-freedom standard error is well-defined
    // and deliberately wide, not invalid, so `valid` is true here (unlike
    // the 0- and 1-period cases above).
    const double two[] = { 100.0, 120.0 };
    const auto fit = fitPeriod (two, 2);

    REQUIRE (fit.valid);
    REQUIRE (std::isfinite (fit.periodSamples));
    REQUIRE (std::isfinite (fit.periodStdError));
}

TEST_CASE ("computeMeasurement at the two-period boundary stays finite and valid")
{
    const double two[] = { 100.0, 120.0 };
    const auto result = computeMeasurement (two, 2, 48000.0, 480.0, 69);

    REQUIRE (result.valid);
    REQUIRE (std::isfinite (result.frequency));
    REQUIRE (std::isfinite (result.frequencyDeviation));
    REQUIRE (std::isfinite (result.pitch));
    REQUIRE (std::isfinite (result.pitchDeviation));
}

TEST_CASE ("measurement converts periods to frequency and pitch")
{
    // 100 samples per period at 48 kHz = 480 Hz. Reference 480 Hz at MIDI 69
    // means the measured pitch is exactly the reference pitch.
    const std::vector<double> periods (50, 100.0);
    const auto result = computeMeasurement (periods.data(), (int) periods.size(),
                                            48000.0, 480.0, 69);

    REQUIRE (result.valid);
    REQUIRE (result.frequency == Approx (480.0));
    REQUIRE (result.pitch     == Approx (69.0));
    REQUIRE (result.pitchDeviation == Approx (0.0).margin (1e-9));
}

TEST_CASE ("an octave above the reference reads as twelve semitones")
{
    const std::vector<double> periods (50, 50.0);   // 960 Hz
    const auto result = computeMeasurement (periods.data(), (int) periods.size(),
                                            48000.0, 480.0, 69);

    REQUIRE (result.frequency == Approx (960.0));
    REQUIRE (result.pitch     == Approx (81.0));
}

TEST_CASE ("measurement propagates a nonzero deviation using the exact formulas")
{
    // Every other computeMeasurement test above uses perfectly uniform
    // periods, so periodStdError == 0 there and both deviation outputs are
    // forced to zero regardless of whether the propagation formulas are
    // right. This case uses the same analytic {8, 11, 9, 12} fit as
    // "slope standard error matches the analytic value" so the expected
    // deviations are hand-derived, not recomputed from the code's own
    // formula (which would be circular):
    //
    //   periodSamples      = 10
    //   periodStdError     = sqrt(2/15) = 0.3651483716701107
    //   relative           = periodStdError / periodSamples
    //                      = 0.3651483716701107 / 10
    //                      = 0.03651483716701107
    //   frequency          = sampleRate / periodSamples = 48000 / 10 = 4800 Hz
    //   frequencyDeviation = frequency * relative
    //                      = 4800 * 0.03651483716701107
    //                      = 175.27121840165314
    //   pitchDeviation     = 12 * relative / ln(2)
    //                      = 0.43817804600413284 / 0.6931471805599453
    //                      = 0.6321572939965786  (about 63.2 cents)
    //
    // referenceFrequency is set equal to the measured frequency (4800 Hz)
    // so the pitch assertion is exact: 0 semitones from a 69 reference.
    const std::vector<double> periods { 8.0, 11.0, 9.0, 12.0 };
    const auto result = computeMeasurement (periods.data(), (int) periods.size(),
                                            48000.0, 4800.0, 69);

    REQUIRE (result.valid);
    REQUIRE (result.frequency == Approx (4800.0));
    REQUIRE (result.pitch     == Approx (69.0).margin (1e-9));

    REQUIRE (result.frequencyDeviation == Approx (175.27121840165314).margin (1e-6));
    REQUIRE (result.pitchDeviation     == Approx (0.6321572939965786).margin (1e-6));

    // Guard against the two specific mis-derivations named in review, either
    // of which a uniform-period test cannot catch since both silently
    // produce zero there.
    //
    // Dropping the division by periodSamples when computing `relative`
    // would leave frequencyDeviation = frequency * periodStdError instead:
    REQUIRE (result.frequencyDeviation != Approx (4800.0 * std::sqrt (2.0 / 15.0)));
    // Omitting the 1/ln(2) factor when converting to semitones would leave
    // pitchDeviation = 12 * relative:
    REQUIRE (result.pitchDeviation != Approx (12.0 * (std::sqrt (2.0 / 15.0) / 10.0)));
}
