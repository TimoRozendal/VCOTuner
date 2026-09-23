// tests/PeriodDetectorTests.cpp
#include <catch2/catch_test_macros.hpp>
#include "dsp/PeriodDetector.h"

using namespace vcotuner;

TEST_CASE ("a freshly reset detector is collecting")
{
    PeriodDetector detector;
    detector.reset (PeriodDetectorConfig {});
    REQUIRE (detector.status() == DetectorStatus::collecting);
}

#include <catch2/catch_approx.hpp>
#include <cmath>
#include <vector>

using Catch::Approx;

namespace
{
    constexpr double kPi = 3.14159265358979323846;

    // Generates a sine with a given DC offset and amplitude.
    std::vector<float> makeSine (double freq, double sampleRate, int numSamples,
                                 double amplitude = 1.0, double dc = 0.0)
    {
        std::vector<float> out ((size_t) numSamples);
        for (int i = 0; i < numSamples; ++i)
            out[(size_t) i] = (float) (dc + amplitude
                                * std::sin (2.0 * kPi * freq * i / sampleRate));
        return out;
    }
}

TEST_CASE ("level tracking finds the midpoint of a DC-offset signal")
{
    PeriodDetectorConfig cfg;
    cfg.warmupSamples = 4800;              // 100 ms at 48 kHz
    const auto samples = makeSine (220.0, 48000.0, 4800, 0.8, 0.3);

    PeriodDetector detector;
    detector.reset (cfg);
    detector.processBlock (samples.data(), (int) samples.size());

    REQUIRE (detector.midpoint()  == Approx (0.3).margin (0.01));
    REQUIRE (detector.amplitude() == Approx (0.8).margin (0.01));
}

TEST_CASE ("level tracking handles an asymmetric waveform")
{
    // Ramp from -0.2 to +1.0: midpoint 0.4, amplitude 0.6.
    PeriodDetectorConfig cfg;
    cfg.warmupSamples = 1200;
    std::vector<float> samples (1200);
    for (int i = 0; i < 1200; ++i)
        samples[(size_t) i] = (float) (-0.2 + 1.2 * ((i % 100) / 100.0));

    PeriodDetector detector;
    detector.reset (cfg);
    detector.processBlock (samples.data(), (int) samples.size());

    REQUIRE (detector.midpoint()  == Approx (0.4).margin (0.02));
    REQUIRE (detector.amplitude() == Approx (0.6).margin (0.02));
}

TEST_CASE ("silence is reported as failedNoCrossings, not a divide by zero")
{
    PeriodDetectorConfig cfg;
    cfg.warmupSamples = 480;
    std::vector<float> silence (480, 0.0f);

    PeriodDetector detector;
    detector.reset (cfg);
    detector.processBlock (silence.data(), (int) silence.size());

    REQUIRE (detector.status() == DetectorStatus::failedNoCrossings);
    REQUIRE (std::isfinite (detector.midpoint()));
    REQUIRE (std::isfinite (detector.amplitude()));
}
