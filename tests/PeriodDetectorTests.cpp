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
#include <algorithm>
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

TEST_CASE ("level tracking is unaffected by block chunking")
{
    // Same DC-offset sine as "level tracking finds the midpoint of a
    // DC-offset signal", but fed through processBlock in small chunks —
    // the way the real-time audio callback delivers 256-512 sample
    // buffers, never one block spanning the whole warm-up window.
    PeriodDetectorConfig cfg;
    cfg.warmupSamples = 4800;              // 100 ms at 48 kHz
    const auto samples = makeSine (220.0, 48000.0, 4800, 0.8, 0.3);

    PeriodDetector singleBlock;
    singleBlock.reset (cfg);
    singleBlock.processBlock (samples.data(), (int) samples.size());

    PeriodDetector chunked;
    chunked.reset (cfg);
    const int chunkSize = 64;
    for (int offset = 0; offset < (int) samples.size(); offset += chunkSize)
    {
        const int n = std::min (chunkSize, (int) samples.size() - offset);
        chunked.processBlock (samples.data() + offset, n);
    }

    REQUIRE (chunked.midpoint()  == singleBlock.midpoint());
    REQUIRE (chunked.amplitude() == singleBlock.amplitude());
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

#include <random>

namespace
{
    // M_PI is not standard C++ (needs _USE_MATH_DEFINES on MSVC); reuse the
    // kPi constant declared above instead.
    std::vector<float> makeNoisySine (double freq, double sampleRate, int numSamples,
                                      double noise, double dc, unsigned seed = 7)
    {
        std::mt19937 rng (seed);
        std::uniform_real_distribution<double> dist (-noise, noise);
        std::vector<float> out ((size_t) numSamples);
        for (int i = 0; i < numSamples; ++i)
            out[(size_t) i] = (float) (dc + std::sin (2.0 * kPi * freq * i / sampleRate)
                                          + dist (rng));
        return out;
    }
}

TEST_CASE ("hysteresis rejects noise-induced false crossings")
{
    // 1 second of 220 Hz => 220 periods. Warm-up consumes roughly the first
    // 2 cycles, so allow a small shortfall rather than demanding exactly 219.
    const int numSamples = 48000;

    struct Case { double noise; double dc; };
    const Case cases[] = { {0.02, 0.0}, {0.05, 0.0}, {0.02, 0.9}, {0.05, 0.9} };

    for (const auto& c : cases)
    {
        PeriodDetectorConfig cfg;
        cfg.warmupSamples  = 480;
        cfg.maxPeriods     = 2000;
        cfg.requiredPeriods = 100000;   // never declare 'stable'; just count
        const auto samples = makeNoisySine (220.0, 48000.0, numSamples, c.noise, c.dc);

        PeriodDetector detector;
        detector.reset (cfg);
        detector.processBlock (samples.data(), numSamples);

        INFO ("noise=" << c.noise << " dc=" << c.dc);
        // The old hard-threshold detector produced up to 609 here.
        REQUIRE (detector.numPeriods() >= 215);
        REQUIRE (detector.numPeriods() <= 221);
    }
}

TEST_CASE ("trigger level adapts to very quiet and very hot signals")
{
    for (double amp : { 0.01, 0.5, 4.0 })
    {
        PeriodDetectorConfig cfg;
        cfg.warmupSamples   = 480;
        cfg.maxPeriods      = 2000;
        cfg.requiredPeriods = 100000;
        const auto samples = makeSine (440.0, 48000.0, 48000, amp, 0.0);

        PeriodDetector detector;
        detector.reset (cfg);
        detector.processBlock (samples.data(), 48000);

        INFO ("amplitude=" << amp);
        REQUIRE (detector.numPeriods() >= 435);
        REQUIRE (detector.numPeriods() <= 441);
    }
}
