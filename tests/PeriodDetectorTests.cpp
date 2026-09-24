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
    // The absolute count follows from the fixture: 48000 samples at 440 Hz
    // is 440 cycles, warm-up consumes 480 samples (4.4 cycles), the trigger
    // needs up to another half cycle to arm, and the first crossing is
    // discarded for having no predecessor. That lands on 434. What matters
    // is that the count does not move with amplitude.
    std::vector<int> counts;

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
        counts.push_back (detector.numPeriods());
    }

    INFO ("counts: " << counts[0] << ", " << counts[1] << ", " << counts[2]);
    // A threshold fixed in absolute terms rather than scaled to the measured
    // amplitude would miss every crossing at 0.01 and still fire at 4.0.
    REQUIRE (counts[0] == counts[1]);
    REQUIRE (counts[1] == counts[2]);
    REQUIRE (counts[0] >= 430);
    REQUIRE (counts[0] <= 441);
}

#include <numeric>

namespace
{
    double periodJitter (const PeriodDetector& d)
    {
        const int n = d.numPeriods();
        if (n < 2) return 1e9;
        const double* p = d.periodData();
        const double mean = std::accumulate (p, p + n, 0.0) / n;
        double acc = 0.0;
        for (int i = 0; i < n; ++i) acc += (p[i] - mean) * (p[i] - mean);
        return std::sqrt (acc / n);
    }

    PeriodDetectorConfig countingConfig()
    {
        PeriodDetectorConfig cfg;
        cfg.warmupSamples   = 480;
        cfg.maxPeriods      = 4000;
        cfg.requiredPeriods = 100000;
        return cfg;
    }
}

TEST_CASE ("interpolation makes period measurement sub-sample accurate")
{
    // Regression guard for the mirrored-fraction bug. The old formula
    // produced ~0.57 samples of jitter here; no interpolation at all gives
    // ~0.29. Neither can reach 0.05.
    for (double freq : { 110.0, 440.0, 1318.51, 4186.01 })
    {
        const auto samples = makeSine (freq, 48000.0, 48000, 0.9, 0.0);
        PeriodDetector detector;
        detector.reset (countingConfig());
        detector.processBlock (samples.data(), 48000);

        INFO ("freq=" << freq << " jitter=" << periodJitter (detector));
        REQUIRE (detector.numPeriods() > 50);
        REQUIRE (periodJitter (detector) < 0.05);
    }
}

TEST_CASE ("recovered frequency is accurate to well under a cent")
{
    for (double freq : { 110.0, 440.0, 1318.51, 4186.01 })
    {
        const auto samples = makeSine (freq, 48000.0, 48000, 0.9, 0.0);
        PeriodDetector detector;
        detector.reset (countingConfig());
        detector.processBlock (samples.data(), 48000);

        const int n = detector.numPeriods();
        const double* p = detector.periodData();
        const double meanPeriod = std::accumulate (p, p + n, 0.0) / n;
        const double measured   = 48000.0 / meanPeriod;
        const double cents      = 1200.0 * std::log2 (measured / freq);

        INFO ("freq=" << freq << " cents error=" << cents);
        REQUIRE (std::abs (cents) < 0.1);
    }
}

TEST_CASE ("interpolation is accurate on a saw wave")
{
    const double freq = 440.0;
    std::vector<float> samples (48000);
    for (int i = 0; i < 48000; ++i)
    {
        const double phase = std::fmod (freq * i / 48000.0, 1.0);
        samples[(size_t) i] = (float) (2.0 * phase - 1.0);
    }

    PeriodDetector detector;
    detector.reset (countingConfig());
    detector.processBlock (samples.data(), 48000);

    REQUIRE (periodJitter (detector) < 0.05);
}

TEST_CASE ("a steady signal reaches the stable status")
{
    PeriodDetectorConfig cfg;
    cfg.warmupSamples   = 480;
    cfg.requiredPeriods = 20;
    const auto samples = makeSine (440.0, 48000.0, 48000, 0.9, 0.0);

    PeriodDetector detector;
    detector.reset (cfg);
    detector.processBlock (samples.data(), 48000);

    REQUIRE (detector.status() == DetectorStatus::stable);
    REQUIRE (detector.numValidPeriods() >= 20);
}

TEST_CASE ("a constantly changing rate never stabilises and terminates")
{
    // A sweep from 200 Hz to 2 kHz never holds a steady period. The detector
    // must reach a terminal state rather than hanging - the old code left the
    // measurement running here and stalled until the top-level timeout.
    PeriodDetectorConfig cfg;
    cfg.warmupSamples   = 480;
    cfg.maxPeriods      = 200;
    cfg.requiredPeriods = 20;

    std::vector<float> samples (48000);
    double phase = 0.0;
    for (int i = 0; i < 48000; ++i)
    {
        const double f = 200.0 + 1800.0 * (i / 48000.0);
        phase += 2.0 * kPi * f / 48000.0;
        samples[(size_t) i] = (float) std::sin (phase);
    }

    PeriodDetector detector;
    detector.reset (cfg);
    detector.processBlock (samples.data(), 48000);

    REQUIRE (detector.status() == DetectorStatus::failedUnstable);
}

TEST_CASE ("a steady signal that outruns the buffer reports failedBufferFull")
{
    // Stabilises immediately, but the buffer cannot hold enough periods to
    // satisfy requiredPeriods. Distinct from failedUnstable: the signal is
    // fine, the resolution setting is simply too high for the storage.
    PeriodDetectorConfig cfg;
    cfg.warmupSamples   = 480;
    cfg.maxPeriods      = 20;
    cfg.requiredPeriods = 500;
    const auto samples = makeSine (440.0, 48000.0, 48000, 0.9, 0.0);

    PeriodDetector detector;
    detector.reset (cfg);
    detector.processBlock (samples.data(), 48000);

    REQUIRE (detector.status() == DetectorStatus::failedBufferFull);
}

TEST_CASE ("valid periods exclude the unstable run-in")
{
    PeriodDetectorConfig cfg;
    cfg.warmupSamples   = 480;
    cfg.requiredPeriods = 10;
    const auto samples = makeSine (440.0, 48000.0, 48000, 0.9, 0.0);

    PeriodDetector detector;
    detector.reset (cfg);
    detector.processBlock (samples.data(), 48000);

    // Concrete, because the obvious relational assertions
    // (numValidPeriods() <= numPeriods(), validPeriods() != nullptr) hold by
    // construction and would still pass with run-in exclusion deleted.
    // At 440 Hz / 48 kHz the detector latches after the first stabilityWindow
    // (5) periods and stops at requiredPeriods (10) valid ones, so it holds 15
    // periods of which the first 5 are the discarded run-in.
    REQUIRE (detector.numPeriods() == 15);
    REQUIRE (detector.numValidPeriods() == 10);
    REQUIRE (detector.validPeriods() == detector.periodData() + 5);
}

TEST_CASE ("every shipped pitch and resolution reaches stable")
{
    // The bespoke fixtures above each pin one terminal status. This pins the
    // arithmetic between requiredPeriods, stabilityWindow and maxPeriods
    // across the settings a user can actually select: the pitch-range combo
    // spans MIDI 24..96 and the resolution combo offers 20, 100 and 400
    // periods per note. maxPeriods (600) has to hold requiredPeriods plus the
    // stabilityWindow (5) run-in that validPeriods() discards. Raising the top
    // resolution past 595 fails here rather than in a user's sweep.
    const double sampleRate = 48000.0;

    for (int midi : { 24, 60, 96 })
    {
        for (int requiredPeriods : { 20, 100, 400 })
        {
            const double freq = 440.0 * std::pow (2.0, (midi - 69) / 12.0);

            PeriodDetectorConfig cfg;
            cfg.requiredPeriods = requiredPeriods;
            // VCOTuner::startDetectorRun() sizes the warm-up window to two
            // cycles of the expected frequency, clamped to [256, 48000].
            cfg.warmupSamples = std::min (48000,
                                          std::max (256, (int) (2.0 * sampleRate / freq)));

            // Enough signal for the run-in, the required periods and a margin.
            const int numSamples = cfg.warmupSamples
                + (int) ((requiredPeriods + 2 * cfg.stabilityWindow + 4)
                         * sampleRate / freq)
                + 1000;
            const auto samples = makeSine (freq, sampleRate, numSamples, 0.9, 0.0);

            PeriodDetector detector;
            detector.reset (cfg);
            detector.processBlock (samples.data(), numSamples);

            INFO ("midi=" << midi << " freq=" << freq
                  << " requiredPeriods=" << requiredPeriods
                  << " numPeriods=" << detector.numPeriods());
            REQUIRE (detector.status() == DetectorStatus::stable);
            REQUIRE (detector.numValidPeriods() >= requiredPeriods);
        }
    }
}
