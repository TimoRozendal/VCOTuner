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
