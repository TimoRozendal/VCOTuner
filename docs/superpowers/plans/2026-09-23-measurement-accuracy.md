# VCOTuner Measurement Accuracy Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix the zero-crossing interpolation bug, add noise/DC-immune triggering, replace the error-bar statistic with a correct estimator, and stop one bad note from discarding an entire sweep — all behind unit tests.

**Architecture:** Extract the measurement logic out of `VCOTuner`'s audio callback into three pure C++ units (`PeriodDetector`, `MeasurementStatistics`, `MeasurementError`) compiled as a `vcotuner_dsp` static library with no JUCE dependency. `VCOTuner` becomes a thin JUCE adapter that feeds samples in and reads results out. The pure library is linked by both the app and a Catch2 test binary.

**Tech Stack:** C++17, JUCE 6.1.5 (submodule), CMake 3.12+, Catch2 v3 via `FetchContent`, CTest.

**Spec:** `docs/superpowers/specs/2026-09-23-vcotuner-measurement-fixes-design.md`

## Global Constraints

- C++17 (`set(CMAKE_CXX_STANDARD 17)`, already in root `CMakeLists.txt:21`).
- Files under `Source/dsp/` MUST NOT include any JUCE header. They are compiled into `vcotuner_dsp`, which links no JUCE module. This is what keeps the tests fast and the logic portable.
- Namespace for all pure units: `vcotuner`.
- Catch2 pinned to `v3.5.2`.
- Commit messages use conventional-commit style (`fix:`, `feat:`, `test:`, `docs:`, `refactor:`), matching existing history.
- **Do not add `Co-Authored-By` or any Claude attribution trailer to commits.**
- Work happens on branch `feature/measurement-accuracy`.
- On macOS the build requires `MACOSX_DEPLOYMENT_TARGET=11.0` in the environment for both configure and build (JUCE 6.1.5 calls `CGWindowListCreateImage`, obsoleted in the macOS 15 SDK). Configure: `MACOSX_DEPLOYMENT_TARGET=11.0 cmake -G Xcode -B build -DCMAKE_OSX_ARCHITECTURES="arm64"`.

---

### Task 1: Test infrastructure and `PeriodDetector` skeleton

Sets up Catch2, the `vcotuner_dsp` library, the test target, CTest wiring and the CI job. Folded into one task because none of it is independently useful.

**Files:**
- Create: `Source/dsp/PeriodDetector.h`
- Create: `Source/dsp/PeriodDetector.cpp`
- Create: `tests/CMakeLists.txt`
- Create: `tests/PeriodDetectorTests.cpp`
- Modify: `CMakeLists.txt` (add library + `add_subdirectory(tests)`)
- Modify: `.github/workflows/CI.yaml` (add test job)

**Interfaces:**
- Consumes: nothing.
- Produces: `vcotuner::PeriodDetectorConfig`, `vcotuner::DetectorStatus`, `vcotuner::PeriodDetector` with `reset(const PeriodDetectorConfig&)`, `processBlock(const float*, int)`, `status() const`. Tasks 2–5 extend this class; Task 8 consumes it.

- [ ] **Step 1: Write the header**

```cpp
// Source/dsp/PeriodDetector.h
#pragma once

#include <vector>

namespace vcotuner
{

struct PeriodDetectorConfig
{
    double sampleRate         = 48000.0;
    double hysteresisFraction = 0.1;    // of measured amplitude
    int    stabilityWindow    = 5;      // consecutive periods compared
    double stabilityTolerance = 0.1;    // 10% spread allowed
    int    maxPeriods         = 600;    // storage limit
    int    warmupSamples      = 2048;   // level-tracking window
    double silenceFloor       = 1e-4;   // amplitude below this => silent
    int    requiredPeriods    = 10;     // valid periods needed for 'stable'
};

enum class DetectorStatus
{
    collecting,         // still gathering
    stable,             // enough valid periods collected
    failedUnstable,     // never reached a steady rate
    failedNoCrossings,  // silent, or no crossings at all
    failedBufferFull    // ran out of storage before stabilising
};

class PeriodDetector
{
public:
    void reset (const PeriodDetectorConfig& config);
    void processBlock (const float* samples, int numSamples);

    DetectorStatus status() const noexcept { return currentStatus; }

private:
    PeriodDetectorConfig cfg {};
    DetectorStatus currentStatus = DetectorStatus::collecting;
};

} // namespace vcotuner
```

- [ ] **Step 2: Write the minimal implementation**

```cpp
// Source/dsp/PeriodDetector.cpp
#include "PeriodDetector.h"

namespace vcotuner
{

void PeriodDetector::reset (const PeriodDetectorConfig& config)
{
    cfg = config;
    currentStatus = DetectorStatus::collecting;
}

void PeriodDetector::processBlock (const float*, int)
{
}

} // namespace vcotuner
```

- [ ] **Step 3: Write the failing test**

```cpp
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
```

- [ ] **Step 4: Write `tests/CMakeLists.txt`**

```cmake
include(FetchContent)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.5.2)
FetchContent_MakeAvailable(Catch2)

add_executable(VCOTunerTests
    PeriodDetectorTests.cpp)

target_link_libraries(VCOTunerTests PRIVATE vcotuner_dsp Catch2::Catch2WithMain)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(VCOTunerTests)
```

- [ ] **Step 5: Wire into the root `CMakeLists.txt`**

Insert after the `include_directories(Source)` line (currently `CMakeLists.txt:60`):

```cmake
# Pure DSP logic, no JUCE dependency. Linked by both the app and the tests.
add_library(vcotuner_dsp STATIC
    Source/dsp/PeriodDetector.cpp)
target_include_directories(vcotuner_dsp PUBLIC Source)
target_compile_features(vcotuner_dsp PUBLIC cxx_std_17)

option(VCOTUNER_BUILD_TESTS "Build the unit tests" ON)
if(VCOTUNER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
```

Then add `vcotuner_dsp` to the app's `target_link_libraries(VCOTuner PRIVATE ...)` list (currently `CMakeLists.txt:120`).

- [ ] **Step 6: Configure and run the test to verify it passes**

```bash
MACOSX_DEPLOYMENT_TARGET=11.0 cmake -G Xcode -B build -DCMAKE_OSX_ARCHITECTURES="arm64"
MACOSX_DEPLOYMENT_TARGET=11.0 cmake --build build --config Debug --target VCOTunerTests
ctest --test-dir build -C Debug --output-on-failure
```
Expected: 1 test passes. If Catch2 fails to fetch, check network access — that is the one configure-time network dependency.

- [ ] **Step 7: Add the CI job**

Append to `.github/workflows/CI.yaml`:

```yaml
  ###############################################################################
  # builds and runs the unit tests
  unitTests:
    runs-on: ubuntu-latest

    steps:
    - name: Setup cmake
      uses: jwlawson/actions-setup-cmake@v1.8
      with:
        cmake-version: '3.19.x'

    - name: Checkout
      uses: actions/checkout@v2
      with:
        submodules: recursive

    - name: Configure
      run: |
        cmake -G "Unix Makefiles" -B build -DVCOTUNER_BUILD_TESTS=ON

    - name: Build tests
      run: |
        cmake --build build --target VCOTunerTests

    - name: Run tests
      run: |
        ctest --test-dir build --output-on-failure
```

- [ ] **Step 8: Commit**

```bash
git add Source/dsp tests CMakeLists.txt .github/workflows/CI.yaml
git commit -m "test: add Catch2 test harness and PeriodDetector skeleton"
```

---

### Task 2: Level tracking and silence detection

**Files:**
- Modify: `Source/dsp/PeriodDetector.h`
- Modify: `Source/dsp/PeriodDetector.cpp`
- Modify: `tests/PeriodDetectorTests.cpp`

**Interfaces:**
- Consumes: Task 1's `PeriodDetector`.
- Produces: `double midpoint() const noexcept`, `double amplitude() const noexcept`. Task 3 uses these to place its trigger thresholds.

- [ ] **Step 1: Write the failing tests**

```cpp
// append to tests/PeriodDetectorTests.cpp
#include <catch2/catch_approx.hpp>
#include <cmath>
#include <vector>

using Catch::Approx;

namespace
{
    // Generates a sine with a given DC offset and amplitude.
    std::vector<float> makeSine (double freq, double sampleRate, int numSamples,
                                 double amplitude = 1.0, double dc = 0.0)
    {
        std::vector<float> out ((size_t) numSamples);
        for (int i = 0; i < numSamples; ++i)
            out[(size_t) i] = (float) (dc + amplitude
                                * std::sin (2.0 * M_PI * freq * i / sampleRate));
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
```

- [ ] **Step 2: Run the tests to verify they fail**

```bash
MACOSX_DEPLOYMENT_TARGET=11.0 cmake --build build --config Debug --target VCOTunerTests
ctest --test-dir build -C Debug --output-on-failure
```
Expected: FAIL — `midpoint`/`amplitude` are not members of `PeriodDetector`.

- [ ] **Step 3: Add the accessors and state to the header**

Add to the public section:

```cpp
    double midpoint()  const noexcept { return levelMidpoint; }
    double amplitude() const noexcept { return levelAmplitude; }
```

Add to the private section:

```cpp
    long long sampleCounter  = 0;
    int       warmupRemaining = 0;
    double    runningMin     = 0.0;
    double    runningMax     = 0.0;
    double    levelMidpoint  = 0.0;
    double    levelAmplitude = 0.0;
    bool      haveLevel      = false;
```

- [ ] **Step 4: Implement level tracking**

```cpp
void PeriodDetector::reset (const PeriodDetectorConfig& config)
{
    cfg = config;
    currentStatus   = DetectorStatus::collecting;
    sampleCounter   = 0;
    warmupRemaining = config.warmupSamples;
    runningMin      =  1e30;
    runningMax      = -1e30;
    levelMidpoint   = 0.0;
    levelAmplitude  = 0.0;
    haveLevel       = false;
}

void PeriodDetector::processBlock (const float* samples, int numSamples)
{
    if (samples == nullptr || numSamples <= 0)
        return;

    for (int i = 0; i < numSamples; ++i)
    {
        const double s = (double) samples[i];

        // Level tracking runs continuously so the detector follows slow
        // level changes, not just the warm-up window.
        if (s < runningMin) runningMin = s;
        if (s > runningMax) runningMax = s;

        if (warmupRemaining > 0)
        {
            if (--warmupRemaining == 0)
                finishWarmup();
        }

        ++sampleCounter;
    }
}

void PeriodDetector::finishWarmup()
{
    levelMidpoint  = (runningMax + runningMin) * 0.5;
    levelAmplitude = (runningMax - runningMin) * 0.5;

    if (levelAmplitude < cfg.silenceFloor)
    {
        levelAmplitude = 0.0;
        currentStatus  = DetectorStatus::failedNoCrossings;
        return;
    }

    haveLevel = true;
}
```

Declare `void finishWarmup();` in the private section of the header.

- [ ] **Step 5: Run the tests to verify they pass**

```bash
MACOSX_DEPLOYMENT_TARGET=11.0 cmake --build build --config Debug --target VCOTunerTests
ctest --test-dir build -C Debug --output-on-failure
```
Expected: all 4 tests PASS.

- [ ] **Step 6: Commit**

```bash
git add Source/dsp/PeriodDetector.h Source/dsp/PeriodDetector.cpp tests/PeriodDetectorTests.cpp
git commit -m "feat: add signal level tracking and silence detection to PeriodDetector"
```

---

### Task 3: Hysteresis triggering (integer crossing positions)

Isolates the noise/DC immunity fix from the interpolation fix so each is provably tested on its own. Crossings are recorded at integer sample positions here; Task 4 makes them sub-sample accurate.

**Files:**
- Modify: `Source/dsp/PeriodDetector.h`
- Modify: `Source/dsp/PeriodDetector.cpp`
- Modify: `tests/PeriodDetectorTests.cpp`

**Interfaces:**
- Consumes: Task 2's `midpoint()` / `amplitude()`.
- Produces: `int numPeriods() const noexcept`, `const double* periodData() const noexcept`. Task 5 adds validity filtering on top; Task 6 consumes the period array.

- [ ] **Step 1: Write the failing tests**

These are the exact cases from the spec, where the current detector finds 314 / 305 / 609 crossings instead of 220.

```cpp
// append to tests/PeriodDetectorTests.cpp
#include <random>

namespace
{
    std::vector<float> makeNoisySine (double freq, double sampleRate, int numSamples,
                                      double noise, double dc, unsigned seed = 7)
    {
        std::mt19937 rng (seed);
        std::uniform_real_distribution<double> dist (-noise, noise);
        std::vector<float> out ((size_t) numSamples);
        for (int i = 0; i < numSamples; ++i)
            out[(size_t) i] = (float) (dc + std::sin (2.0 * M_PI * freq * i / sampleRate)
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
    // The absolute count follows from the fixture: 48000 samples at 440 Hz is
    // 440 cycles, warm-up consumes 480 samples (4.4 cycles), the trigger needs
    // up to another half cycle to arm, and the first crossing is discarded for
    // having no predecessor. That lands on 434. What matters here is that the
    // count does not move with amplitude.
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
```

- [ ] **Step 2: Run the tests to verify they fail**

Expected: FAIL — `numPeriods` is not a member.

- [ ] **Step 3: Add storage and accessors to the header**

Public:

```cpp
    int numPeriods() const noexcept { return (int) periods.size(); }
    const double* periodData() const noexcept { return periods.data(); }
```

Private:

```cpp
    std::vector<double> periods;
    double lastCrossing = -1.0;
    double lastSample   =  0.0;
    bool   armed        = false;
    bool   haveLastSample = false;
```

- [ ] **Step 4: Implement the Schmitt trigger**

Reset additions:

```cpp
    periods.clear();
    periods.reserve ((size_t) config.maxPeriods);
    lastCrossing   = -1.0;
    lastSample     = 0.0;
    armed          = false;
    haveLastSample = false;
```

In `processBlock`, after the warm-up block and before `++sampleCounter`:

```cpp
        if (haveLevel && currentStatus == DetectorStatus::collecting)
            processCrossing (s);
```

New member:

```cpp
void PeriodDetector::processCrossing (double s)
{
    const double hysteresis = cfg.hysteresisFraction * levelAmplitude;

    // Re-arm only after the signal has dropped clearly below the midpoint.
    // Noise between the rails cannot retrigger.
    if (! armed)
    {
        if (s < levelMidpoint - hysteresis)
            armed = true;
    }
    else if (haveLastSample && lastSample < levelMidpoint && s >= levelMidpoint)
    {
        armed = false;
        recordCrossing ((double) sampleCounter);
    }

    lastSample = s;
    haveLastSample = true;
}

void PeriodDetector::recordCrossing (double position)
{
    if ((int) periods.size() >= cfg.maxPeriods)
        return;

    if (lastCrossing >= 0.0)
        periods.push_back (position - lastCrossing);

    lastCrossing = position;
}
```

Declare both in the private section. Note `recordCrossing` discards the first crossing (no preceding one to measure from), which also removes the bogus first entry the old code stored.

- [ ] **Step 5: Run the tests to verify they pass**

Expected: both new tests PASS, earlier tests still PASS.

- [ ] **Step 6: Commit**

```bash
git add Source/dsp/PeriodDetector.h Source/dsp/PeriodDetector.cpp tests/PeriodDetectorTests.cpp
git commit -m "fix: reject false zero crossings with a hysteresis trigger"
```

---

### Task 4: Sub-sample interpolation — the core accuracy fix

**Files:**
- Modify: `Source/dsp/PeriodDetector.cpp`
- Modify: `tests/PeriodDetectorTests.cpp`

**Interfaces:**
- Consumes: Task 3's `recordCrossing`.
- Produces: no new API — the period values simply become sub-sample accurate.

- [ ] **Step 1: Write the failing tests**

The jitter bounds below are the crux of the whole plan: the old mirrored formula measured ~0.57 samples of jitter at 440 Hz and ~0.99 at 4186 Hz, and even *no* interpolation gives ~0.29 / ~0.50. A bound of 0.05 is unreachable by anything except correct interpolation.

```cpp
// append to tests/PeriodDetectorTests.cpp
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
```

- [ ] **Step 2: Run the tests to verify they fail**

Expected: FAIL on the jitter assertions — integer crossings give ~0.29 samples of jitter, well above 0.05.

- [ ] **Step 3: Implement correct interpolation**

Replace the `recordCrossing ((double) sampleCounter);` call in `processCrossing` with:

```cpp
        armed = false;

        // Linear interpolation between the two samples straddling the
        // midpoint. The previous sample sits at sampleCounter - 1.
        //   correct:  x0 = (sampleCounter - 1) + f
        // The old code computed (sampleCounter - 1) + (1 - f), mirroring the
        // fraction within the interval and roughly doubling the jitter versus
        // no interpolation at all.
        const double slope = s - lastSample;               // > 0 by the branch
        const double f = (slope != 0.0)
                       ? (levelMidpoint - lastSample) / slope
                       : 0.0;
        recordCrossing ((double) (sampleCounter - 1) + f);
```

- [ ] **Step 4: Run the tests to verify they pass**

Expected: all PASS. If a jitter assertion still fails, the fraction is being applied to the wrong base sample — check that it is `sampleCounter - 1`, not `sampleCounter`.

- [ ] **Step 5: Commit**

```bash
git add Source/dsp/PeriodDetector.cpp tests/PeriodDetectorTests.cpp
git commit -m "fix: correct sub-sample zero-crossing interpolation formula

The previous formula used a negated slope and paired the previous sample
with the wrong x coordinate, mirroring the fractional crossing position
within the sample interval. It produced roughly twice the jitter of no
interpolation at all, inflating error bars and causing spurious high
jitter aborts above ~MIDI 100."
```

---

### Task 5: Stability detection and terminal statuses

**Files:**
- Modify: `Source/dsp/PeriodDetector.h`
- Modify: `Source/dsp/PeriodDetector.cpp`
- Modify: `tests/PeriodDetectorTests.cpp`

**Interfaces:**
- Consumes: Task 4's period array.
- Produces: `int numValidPeriods() const noexcept`, `const double* validPeriods() const noexcept`, and terminal `DetectorStatus` values. Task 8 branches on the status; Task 6 consumes the valid range.

- [ ] **Step 1: Write the failing tests**

```cpp
// append to tests/PeriodDetectorTests.cpp
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
        phase += 2.0 * M_PI * f / 48000.0;
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

    REQUIRE (detector.numValidPeriods() <= detector.numPeriods());
    REQUIRE (detector.validPeriods() != nullptr);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Expected: FAIL — `numValidPeriods` is not a member.

- [ ] **Step 3: Add the API and state**

Public:

```cpp
    int numValidPeriods() const noexcept;
    const double* validPeriods() const noexcept;
```

Private: `int firstValidIndex = -1;` and `void updateStability();`. Reset it to `-1` in `reset`.

- [ ] **Step 4: Implement stability detection**

```cpp
int PeriodDetector::numValidPeriods() const noexcept
{
    if (firstValidIndex < 0) return 0;
    return (int) periods.size() - firstValidIndex;
}

const double* PeriodDetector::validPeriods() const noexcept
{
    if (firstValidIndex < 0) return periods.data();
    return periods.data() + firstValidIndex;
}

void PeriodDetector::updateStability()
{
    const int n = (int) periods.size();

    if (firstValidIndex < 0 && n >= cfg.stabilityWindow)
    {
        double sum = 0.0;
        for (int i = n - cfg.stabilityWindow; i < n; ++i)
            sum += periods[(size_t) i];
        const double average  = sum / cfg.stabilityWindow;
        const double boundary = average * cfg.stabilityTolerance;

        bool steady = true;
        for (int i = n - cfg.stabilityWindow; i < n; ++i)
            if (std::abs (periods[(size_t) i] - average) >= boundary)
                steady = false;

        if (steady)
            firstValidIndex = n;
    }

    if (firstValidIndex >= 0 && numValidPeriods() >= cfg.requiredPeriods)
    {
        // The latch above only proves the rate held steady across one window.
        // Re-check the whole collected set before declaring success: a drifting
        // oscillator can satisfy a single window and then wander far outside
        // tolerance, which is exactly what failedUnstable is for. The shipping
        // app has this hole - it sets indexOfFirstValidPeriodLength once and
        // never rechecks - so a thermally drifting VCO reads as a confident
        // measurement.
        const double* p = validPeriods();
        const int valid = numValidPeriods();

        double sum = 0.0;
        for (int i = 0; i < valid; ++i)
            sum += p[i];
        const double average  = sum / valid;
        const double boundary = average * cfg.stabilityTolerance;

        for (int i = 0; i < valid; ++i)
        {
            if (std::abs (p[i] - average) >= boundary)
            {
                currentStatus = DetectorStatus::failedUnstable;
                return;
            }
        }

        currentStatus = DetectorStatus::stable;
        return;
    }

    // Storage exhausted before collecting what we need. This must be a
    // terminal state: the old code left the measurement running here, so it
    // stalled until the top level timed out and then blamed the wrong thing.
    // Distinguish the two causes - never steady at all, versus steady but not
    // for long enough - because they need different advice to the user.
    if (n >= cfg.maxPeriods)
        currentStatus = (firstValidIndex < 0) ? DetectorStatus::failedUnstable
                                              : DetectorStatus::failedBufferFull;
}
```

Call `updateStability();` at the end of `recordCrossing`, and add `#include <cmath>` to the .cpp.

- [ ] **Step 5: Run the tests to verify they pass**

Expected: all PASS.

- [ ] **Step 6: Commit**

```bash
git add Source/dsp/PeriodDetector.h Source/dsp/PeriodDetector.cpp tests/PeriodDetectorTests.cpp
git commit -m "feat: add stability detection and terminal statuses to PeriodDetector"
```

---

### Task 6: `MeasurementStatistics` — regression-slope estimator

**Files:**
- Create: `Source/dsp/MeasurementStatistics.h`
- Create: `Source/dsp/MeasurementStatistics.cpp`
- Create: `tests/MeasurementStatisticsTests.cpp`
- Modify: `CMakeLists.txt` (add source to `vcotuner_dsp`)
- Modify: `tests/CMakeLists.txt` (add test source)

**Interfaces:**
- Consumes: a period array (from Task 5's `validPeriods()`).
- Produces: `vcotuner::PeriodFit`, `vcotuner::MeasurementResult`, `fitPeriod(const double*, int)`, `computeMeasurement(const double*, int, double, double, int)`. Task 8 calls `computeMeasurement`.

- [ ] **Step 1: Write the failing tests**

The standard error below is computed by hand. Crossing times reconstruct to `t = [0, 8, 19, 28, 40]`, whose least-squares fit has slope exactly 10, residuals `[1, -1, 0, -1, 1]`, `SSE = 4`, `Sxx = 10`, so `SE = sqrt(4 / (3 * 10)) = sqrt(2/15) = 0.3651484`.

```cpp
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
```

- [ ] **Step 2: Add the sources to the build**

In root `CMakeLists.txt`, add `Source/dsp/MeasurementStatistics.cpp` to `add_library(vcotuner_dsp STATIC ...)`.
In `tests/CMakeLists.txt`, add `MeasurementStatisticsTests.cpp` to `add_executable(VCOTunerTests ...)`.

- [ ] **Step 3: Run the tests to verify they fail**

Expected: FAIL to compile — `dsp/MeasurementStatistics.h` does not exist.

- [ ] **Step 4: Write the header**

```cpp
// Source/dsp/MeasurementStatistics.h
#pragma once

namespace vcotuner
{

struct PeriodFit
{
    bool   valid          = false;
    double periodSamples  = 0.0;
    double periodStdError = 0.0;
};

struct MeasurementResult
{
    bool   valid              = false;
    double frequency          = 0.0;
    double frequencyDeviation = 0.0;
    double pitch              = 0.0;
    double pitchDeviation     = 0.0;
};

/** Least-squares fit of crossing time against crossing index.

    The periods are cumulated back into crossing times, then fitted with a
    straight line. The slope is the period estimate and the standard error of
    that slope is its uncertainty.

    This replaces taking the standard deviation of the individual periods,
    which answers a different question: consecutive periods share a crossing
    time, so their errors are negatively correlated and the spread of the
    periods badly overstates the uncertainty of their mean.

    Requires at least 3 periods for a meaningful standard error; fewer
    returns valid == false with finite zeroed fields.
*/
PeriodFit fitPeriod (const double* periods, int numPeriods);

/** Converts a period sequence into frequency, pitch and their uncertainties. */
MeasurementResult computeMeasurement (const double* periods, int numPeriods,
                                      double sampleRate,
                                      double referenceFrequency,
                                      int referencePitch);

} // namespace vcotuner
```

- [ ] **Step 5: Write the implementation**

```cpp
// Source/dsp/MeasurementStatistics.cpp
#include "MeasurementStatistics.h"

#include <cmath>
#include <vector>

namespace vcotuner
{

PeriodFit fitPeriod (const double* periods, int numPeriods)
{
    PeriodFit fit;

    if (periods == nullptr || numPeriods < 2)
        return fit;

    // Cumulate periods back into crossing times: n+1 points for n periods.
    const int n = numPeriods + 1;
    std::vector<double> times ((size_t) n);
    times[0] = 0.0;
    for (int i = 0; i < numPeriods; ++i)
        times[(size_t) (i + 1)] = times[(size_t) i] + periods[i];

    double meanX = 0.0, meanY = 0.0;
    for (int i = 0; i < n; ++i) { meanX += i; meanY += times[(size_t) i]; }
    meanX /= n;
    meanY /= n;

    double sxx = 0.0, sxy = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double dx = i - meanX;
        sxx += dx * dx;
        sxy += dx * (times[(size_t) i] - meanY);
    }

    if (sxx <= 0.0)
        return fit;

    const double slope = sxy / sxx;
    const double intercept = meanY - slope * meanX;

    fit.periodSamples = slope;

    // Standard error of the slope needs at least one degree of freedom.
    if (n > 2)
    {
        double sse = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double residual = times[(size_t) i] - (intercept + slope * i);
            sse += residual * residual;
        }
        fit.periodStdError = std::sqrt (sse / ((n - 2) * sxx));
        fit.valid = true;
    }

    return fit;
}

MeasurementResult computeMeasurement (const double* periods, int numPeriods,
                                      double sampleRate,
                                      double referenceFrequency,
                                      int referencePitch)
{
    MeasurementResult result;

    const auto fit = fitPeriod (periods, numPeriods);
    if (! fit.valid || fit.periodSamples <= 0.0
        || referenceFrequency <= 0.0 || sampleRate <= 0.0)
        return result;

    result.frequency = sampleRate / fit.periodSamples;

    // Relative uncertainty carries straight across from period to frequency.
    const double relative = fit.periodStdError / fit.periodSamples;
    result.frequencyDeviation = result.frequency * relative;

    result.pitch = 12.0 * std::log2 (result.frequency / referenceFrequency)
                 + referencePitch;

    // d(pitch)/d(f) = 12 / (f * ln2), so the semitone uncertainty is just the
    // relative uncertainty scaled by 12 / ln2.
    result.pitchDeviation = 12.0 * relative / std::log (2.0);
    result.valid = true;

    return result;
}

} // namespace vcotuner
```

- [ ] **Step 6: Run the tests to verify they pass**

Expected: all PASS, including the analytic `sqrt(2/15)` value.

- [ ] **Step 7: Commit**

```bash
git add Source/dsp/MeasurementStatistics.h Source/dsp/MeasurementStatistics.cpp \
        tests/MeasurementStatisticsTests.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat: estimate period and uncertainty by least-squares fit"
```

---

### Task 7: Error classification and per-cycle failure tracking

**Files:**
- Create: `Source/dsp/MeasurementError.h`
- Create: `Source/dsp/MeasurementError.cpp`
- Create: `tests/MeasurementErrorTests.cpp`
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing.
- Produces: `vcotuner::MeasurementError`, `isFatal(MeasurementError)`, `vcotuner::FailureTracker` with `beginSweep()`, `recordFailure(int, MeasurementError)`, `hasFailures()`, `failures()`. Tasks 10 and 12 consume all of these.

- [ ] **Step 1: Write the failing tests**

```cpp
// tests/MeasurementErrorTests.cpp
#include <catch2/catch_test_macros.hpp>
#include "dsp/MeasurementError.h"

using namespace vcotuner;

TEST_CASE ("setup errors are fatal and abort the run")
{
    REQUIRE (isFatal (MeasurementError::noFrequencyChange));
    REQUIRE (isFatal (MeasurementError::noMidiDevice));
    REQUIRE (isFatal (MeasurementError::audioDeviceStopped));
}

TEST_CASE ("per-note measurement failures are not fatal")
{
    REQUIRE_FALSE (isFatal (MeasurementError::highJitter));
    REQUIRE_FALSE (isFatal (MeasurementError::noZeroCrossings));
    REQUIRE_FALSE (isFatal (MeasurementError::highJitterTimeOut));
    REQUIRE_FALSE (isFatal (MeasurementError::stableTimeout));
    REQUIRE_FALSE (isFatal (MeasurementError::bufferFull));
    REQUIRE_FALSE (isFatal (MeasurementError::none));
}

TEST_CASE ("the failure list resets at the start of each sweep")
{
    FailureTracker tracker;

    tracker.beginSweep();
    tracker.recordFailure (84, MeasurementError::highJitter);
    tracker.recordFailure (96, MeasurementError::noZeroCrossings);
    REQUIRE (tracker.hasFailures());
    REQUIRE (tracker.failures().size() == 2);

    // A new cycle starts clean, so a note that now reads correctly
    // disappears from the status line immediately.
    tracker.beginSweep();
    REQUIRE_FALSE (tracker.hasFailures());
    REQUIRE (tracker.failures().empty());
}

TEST_CASE ("failures record both the pitch and the reason")
{
    FailureTracker tracker;
    tracker.beginSweep();
    tracker.recordFailure (84, MeasurementError::highJitter);

    REQUIRE (tracker.failures()[0].midiPitch == 84);
    REQUIRE (tracker.failures()[0].reason == MeasurementError::highJitter);
}
```

- [ ] **Step 2: Add sources to the build, then run to verify failure**

Add `Source/dsp/MeasurementError.cpp` to `vcotuner_dsp` and `MeasurementErrorTests.cpp` to the test target.
Expected: FAIL to compile — header does not exist.

- [ ] **Step 3: Write the header**

```cpp
// Source/dsp/MeasurementError.h
#pragma once

#include <vector>

namespace vcotuner
{

enum class MeasurementError
{
    none,
    // Per-note: mark the note and carry on with the sweep.
    highJitter,
    noZeroCrossings,
    highJitterTimeOut,
    stableTimeout,
    bufferFull,
    // Fatal: nothing further can succeed, so abort and tell the user.
    noFrequencyChange,
    noMidiDevice,
    audioDeviceStopped
};

/** True when the error makes the rest of the run pointless. */
bool isFatal (MeasurementError error) noexcept;

struct NoteFailure
{
    int              midiPitch = 0;
    MeasurementError reason    = MeasurementError::none;
};

/** Records which notes failed during the current sweep.

    The list is cleared by beginSweep(), so in cycling mode it always
    describes the most recent pass rather than accumulating history.
*/
class FailureTracker
{
public:
    void beginSweep();
    void recordFailure (int midiPitch, MeasurementError reason);

    bool hasFailures() const noexcept { return ! entries.empty(); }
    const std::vector<NoteFailure>& failures() const noexcept { return entries; }

private:
    std::vector<NoteFailure> entries;
};

} // namespace vcotuner
```

- [ ] **Step 4: Write the implementation**

```cpp
// Source/dsp/MeasurementError.cpp
#include "MeasurementError.h"

namespace vcotuner
{

bool isFatal (MeasurementError error) noexcept
{
    switch (error)
    {
        case MeasurementError::noFrequencyChange:
        case MeasurementError::noMidiDevice:
        case MeasurementError::audioDeviceStopped:
            return true;

        case MeasurementError::none:
        case MeasurementError::highJitter:
        case MeasurementError::noZeroCrossings:
        case MeasurementError::highJitterTimeOut:
        case MeasurementError::stableTimeout:
        case MeasurementError::bufferFull:
            return false;
    }
    return false;
}

void FailureTracker::beginSweep()
{
    entries.clear();
}

void FailureTracker::recordFailure (int midiPitch, MeasurementError reason)
{
    entries.push_back (NoteFailure { midiPitch, reason });
}

} // namespace vcotuner
```

- [ ] **Step 5: Run the tests to verify they pass**

Expected: all PASS.

- [ ] **Step 6: Commit**

```bash
git add Source/dsp/MeasurementError.h Source/dsp/MeasurementError.cpp \
        tests/MeasurementErrorTests.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat: classify measurement errors and track per-sweep failures"
```

---

### Task 8: Timeout floor calculation

Extracted as a pure function specifically so the high-pitch abort bug is testable.

**Files:**
- Create: `Source/dsp/MeasurementTiming.h`
- Create: `Source/dsp/MeasurementTiming.cpp`
- Create: `tests/MeasurementTimingTests.cpp`
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing.
- Produces: `int computeTimeoutCycles(double expectedFrequency, int numPeriods, double timerIntervalSeconds, double latencyAllowanceSeconds)`. Task 9 calls this from the state machine.

- [ ] **Step 1: Write the failing tests**

```cpp
// tests/MeasurementTimingTests.cpp
#include <catch2/catch_test_macros.hpp>
#include "dsp/MeasurementTiming.h"

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
    // Both sides must clear the 50-cycle floor for the comparison to mean
    // anything: at 440 Hz with 20 periods the raw counts are 10 and 40, which
    // both clamp to 50, making `with > without` unsatisfiable by construction.
    // 400 periods puts them at 182 and 212.
    const int without = computeTimeoutCycles (440.0, 400, 0.01, 0.0);
    const int with    = computeTimeoutCycles (440.0, 400, 0.01, 0.3);
    REQUIRE (with > without);
}

TEST_CASE ("invalid input still yields a usable timeout")
{
    REQUIRE (computeTimeoutCycles (0.0,  20, 0.01, 0.3) >= 50);
    REQUIRE (computeTimeoutCycles (-1.0, 20, 0.01, 0.3) >= 50);
}
```

Add `#include <cmath>` at the top of the test file.

- [ ] **Step 2: Add to the build and run to verify failure**

Expected: FAIL to compile — header does not exist.

- [ ] **Step 3: Write the header**

```cpp
// Source/dsp/MeasurementTiming.h
#pragma once

namespace vcotuner
{

/** How many timer cycles to wait before declaring a measurement timed out.

    Covers the time needed to observe numPeriods cycles of the expected
    frequency, doubled for headroom, plus a fixed allowance for MIDI and
    audio round-trip latency, and floored so it can never reach zero.
*/
int computeTimeoutCycles (double expectedFrequency,
                          int numPeriods,
                          double timerIntervalSeconds,
                          double latencyAllowanceSeconds);

} // namespace vcotuner
```

- [ ] **Step 4: Write the implementation**

```cpp
// Source/dsp/MeasurementTiming.cpp
#include "MeasurementTiming.h"

#include <algorithm>
#include <cmath>

namespace vcotuner
{

int computeTimeoutCycles (double expectedFrequency,
                          int numPeriods,
                          double timerIntervalSeconds,
                          double latencyAllowanceSeconds)
{
    constexpr int minimumCycles = 50;   // 500 ms at the default 10 ms timer

    if (expectedFrequency <= 0.0 || numPeriods <= 0 || timerIntervalSeconds <= 0.0)
        return minimumCycles;

    const double measurementTime = (numPeriods / expectedFrequency) * 2.0;
    const double totalTime = measurementTime + std::max (0.0, latencyAllowanceSeconds);
    const int cycles = (int) std::ceil (totalTime / timerIntervalSeconds);

    return std::max (cycles, minimumCycles);
}

} // namespace vcotuner
```

- [ ] **Step 5: Run the tests to verify they pass**

Expected: all PASS.

- [ ] **Step 6: Commit**

```bash
git add Source/dsp/MeasurementTiming.h Source/dsp/MeasurementTiming.cpp \
        tests/MeasurementTimingTests.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "fix: floor the measurement timeout so high pitches are not cut off"
```

---

### Task 9: Wire `PeriodDetector` into `VCOTuner`'s audio callback

**Files:**
- Modify: `Source/VCOTuner.h` (members, includes)
- Modify: `Source/VCOTuner.cpp:466-573` (audio callback)

**Interfaces:**
- Consumes: `PeriodDetector`, `DetectorStatus`, `MeasurementError`.
- Produces: `VCOTuner::lastDetectorStatus` readable by the state machine in Task 10.

- [ ] **Step 1: Replace the detector state in the header**

In `Source/VCOTuner.h`, add `#include "dsp/PeriodDetector.h"` and `#include <atomic>`.

Delete these members: `periodLengths[maxNumPeriodLengths]`, `indexOfFirstValidPeriodLength`, `periodLengthsHead`, `lError`, `lastZeroCrossing`, `lastSample`, `sampleCounter`, the `LowLevelError` enum, and `maxNumPeriodLengths`.

Add:

```cpp
    vcotuner::PeriodDetector detector;
    std::atomic<bool> startMeasurement { false };
    std::atomic<bool> stopMeasurement { false };
    std::atomic<int>  detectorStatusFlag { 0 };   // vcotuner::DetectorStatus
```

Change the existing `bool startMeasurement; bool stopMeasurement;` declarations to the atomics above. The comment block above them about message-thread/audio-thread ownership can go — the atomics document it.

- [ ] **Step 2: Rewrite the audio callback**

Replace the body of `audioDeviceIOCallback` (`Source/VCOTuner.cpp:466-573`) with:

```cpp
void VCOTuner::audioDeviceIOCallback (const float** inputChannelData,
                                    int numInputChannels,
                                    float** outputChannelData,
                                    int numOutputChannels,
                                    int numSamples)
{
    if (outputChannelData != nullptr)
    {
        AudioBuffer<float> outputBuffer (outputChannelData, numOutputChannels, numSamples);
        outputBuffer.clear();
    }

    if (stopMeasurement)
    {
        startMeasurement = false;
        stopMeasurement = false;
        initialized = false;
    }

    if (! startMeasurement)
        return;

    // Guard the channel access: numInputChannels was never checked before,
    // so a device with no enabled input channels read out of bounds.
    if (inputChannelData == nullptr || numInputChannels <= 0
        || inputChannelData[0] == nullptr)
        return;

    if (! initialized)
    {
        vcotuner::PeriodDetectorConfig cfg;
        cfg.sampleRate      = sampleRate;
        cfg.requiredPeriods = numPeriodSamples;
        cfg.warmupSamples   = currentWarmupSamples;
        detector.reset (cfg);
        initialized = true;
    }

    detector.processBlock (inputChannelData[0], numSamples);
    detectorStatusFlag = (int) detector.status();

    if (detector.status() != vcotuner::DetectorStatus::collecting)
    {
        initialized = false;
        startMeasurement = false;
    }
}
```

- [ ] **Step 3: Add the warm-up sizing member**

In `Source/VCOTuner.h` add `int currentWarmupSamples = 2048;`. In `prepMeasurement` and `prepRefMeasurement`, before setting `startMeasurement = true`, size it to two cycles of the expected frequency:

```cpp
    {
        const double expectedFreq = (state == prepRefMeasurement || referenceFrequency <= 0.0f)
            ? 440.0 * std::pow (2.0, (currentPitch - 69) / 12.0)
            : referenceFrequency * std::pow (2.0, (currentPitch - referencePitch) / 12.0);
        const double twoCycles = (expectedFreq > 0.0) ? (2.0 * sampleRate / expectedFreq) : 2048.0;
        currentWarmupSamples = jlimit (256, 48000, (int) twoCycles);
    }
```

- [ ] **Step 4: Build the app to verify it compiles**

```bash
MACOSX_DEPLOYMENT_TARGET=11.0 cmake --build build --config Debug --target VCOTuner
```
Expected: compiles. The state machine still references removed members — Task 10 fixes those. If the build blocks progress, do Tasks 9 and 10 as one commit.

- [ ] **Step 5: Commit**

```bash
git add Source/VCOTuner.h Source/VCOTuner.cpp
git commit -m "refactor: use PeriodDetector in the audio callback

Also guards numInputChannels before reading channel 0 and makes the
measurement start/stop flags atomic, which were plain bools shared
between the audio and message threads."
```

---

### Task 10: Continue the sweep past a failed note

**Files:**
- Modify: `Source/VCOTuner.h` (listener, tracker)
- Modify: `Source/VCOTuner.cpp:129-290` (ref measurement and measurement states)

**Interfaces:**
- Consumes: Tasks 6, 7, 8, 9.
- Produces: `VCOTuner::Listener::measurementFailed(int midiPitch, vcotuner::MeasurementError reason)`, `VCOTuner::getFailures()`. Tasks 11 and 12 consume both.

- [ ] **Step 1: Extend the listener interface**

In `Source/VCOTuner.h`, add to `class Listener`:

```cpp
        virtual void measurementFailed (int /*midiPitch*/,
                                        vcotuner::MeasurementError /*reason*/) {}
```

Add to `VCOTuner`'s public section:

```cpp
    const std::vector<vcotuner::NoteFailure>& getFailures() const
        { return failureTracker.failures(); }
```

and to the private section `vcotuner::FailureTracker failureTracker;`, plus the includes for `dsp/MeasurementError.h` and `dsp/MeasurementStatistics.h`.

- [ ] **Step 2: Map detector status to an error**

Add a private helper to `Source/VCOTuner.cpp`:

```cpp
static vcotuner::MeasurementError errorForStatus (vcotuner::DetectorStatus status)
{
    using vcotuner::DetectorStatus;
    using vcotuner::MeasurementError;

    switch (status)
    {
        case DetectorStatus::failedNoCrossings: return MeasurementError::noZeroCrossings;
        case DetectorStatus::failedUnstable:    return MeasurementError::highJitter;
        case DetectorStatus::failedBufferFull:  return MeasurementError::bufferFull;
        case DetectorStatus::collecting:        return MeasurementError::stableTimeout;
        case DetectorStatus::stable:            return MeasurementError::none;
    }
    return MeasurementError::none;
}
```

- [ ] **Step 3: Replace the failure branch in the `measurement` state**

Declare the helper in the private section of `Source/VCOTuner.h`:

```cpp
    void failCurrentNote (vcotuner::MeasurementError reason);
```

In `Source/VCOTuner.cpp`, the `measurement` case currently does `errors.add(Errors::highJitter); switchState(stopped);`. Replace with a helper call, and replace the timeout branch (`Source/VCOTuner.cpp:270-285`) likewise:

```cpp
void VCOTuner::failCurrentNote (vcotuner::MeasurementError reason)
{
    trySendMidiNoteOff (currentPitch);
    stopMeasurement = true;

    failureTracker.recordFailure (currentPitch, reason);
    listeners.call (&Listener::measurementFailed, currentPitch, reason);

    currentPitch += pitchIncrement;
    currentIndex++;

    if (currentPitch <= highestPitch)
        switchState (prepMeasurement);
    else
        switchState (finished);
}
```

Every per-note failure path in the `measurement` state calls `failCurrentNote(...)` instead of `switchState(stopped)`.

The three fatal errors keep the old behaviour: `errors.add(...)` then `switchState(stopped)`.

- [ ] **Step 4: Use the new statistics and timeout**

The state machine reads `detector` from the message thread while the audio
thread owns it. This is safe only because the audio thread publishes
`startMeasurement = false` *after* its final write to the detector, and the
state machine reads the detector only once it has observed that flag clear —
the same handshake the original code relied on, now with an atomic flag
making the ordering explicit. Do not read the detector anywhere else.

In the success branch, replace the manual averaging and deviation block (`Source/VCOTuner.cpp:207-243`) with:

```cpp
    const auto result = vcotuner::computeMeasurement (detector.validPeriods(),
                                                      detector.numValidPeriods(),
                                                      sampleRate,
                                                      referenceFrequency,
                                                      referencePitch);
    if (! result.valid)
    {
        failCurrentNote (vcotuner::MeasurementError::highJitter);
        break;
    }

    measurement_t m;
    m.timestamp       = Time::getCurrentTime();
    m.frequency       = result.frequency;
    m.pitch           = result.pitch;
    m.midiPitch       = currentPitch;
    m.pitchOffset     = result.pitch - currentPitch;
    m.freqDeviation   = result.frequencyDeviation;
    m.pitchDeviation  = result.pitchDeviation;
    m.numMeasurements = detector.numValidPeriods();
    listeners.call (&Listener::newMeasurementReady, m);
```

Replace the timeout computation with:

```cpp
    const double expectedFrequency = referenceFrequency
        * std::pow (2.0, (currentPitch - referencePitch) / 12.0);
    const int expectedCycles = vcotuner::computeTimeoutCycles (expectedFrequency,
                                                               numPeriodSamples,
                                                               0.01, 0.3);
```

- [ ] **Step 5: Reset the tracker at the start of each sweep**

In `switchState`, when entering `prepRefMeasurement`, call `failureTracker.beginSweep();`. This is what makes the live-mode status line describe the current cycle rather than accumulating history.

- [ ] **Step 6: Build and run all tests**

```bash
MACOSX_DEPLOYMENT_TARGET=11.0 cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```
Expected: app compiles, all unit tests still PASS.

- [ ] **Step 7: Commit**

```bash
git add Source/VCOTuner.h Source/VCOTuner.cpp
git commit -m "feat: continue the sweep when a single note fails to measure"
```

---

### Task 11: Show failed notes in the `Visualizer`

**Files:**
- Modify: `Source/Visualizer.h`
- Modify: `Source/Visualizer.cpp:25-50` (auto-scaling), `Source/Visualizer.cpp:129-145` (drawing)

**Interfaces:**
- Consumes: Task 10's `measurementFailed`.
- Produces: no API consumed by later tasks.

- [ ] **Step 1: Record failures in the visualizer**

In `Source/Visualizer.h`, add `void measurementFailed (int midiPitch, vcotuner::MeasurementError reason) override;` and `Array<int> failedPitches;`.

In `Source/Visualizer.cpp`:

```cpp
void Visualizer::measurementFailed (int midiPitch, vcotuner::MeasurementError)
{
    failedPitches.addIfNotAlreadyThere (midiPitch);
    repaint();
}
```

`Visualizer` clears its data in exactly one place — `clearCache()`, currently an inline one-liner at `Source/Visualizer.h:29`. Extend it so the failure list is cleared with the measurements:

```cpp
    void clearCache() { measurements.clear(); failedPitches.clear(); }
```

- [ ] **Step 2: Exclude failed notes from auto-scaling**

`Source/Visualizer.cpp:36-44` currently grows the display range to include every error bar. A note with a huge deviation compresses the real curve into a thin strip, so skip failed notes:

```cpp
    for (int i = 0; i < measurements.size(); i++)
    {
        if (failedPitches.contains (measurements[i].midiPitch))
            continue;

        double value = measurements[i].pitchOffset;
        double deviation = measurements[i].pitchDeviation;
        if (value - deviation < min) min = value - deviation;
        if (value + deviation > max) max = value + deviation;
    }
```

- [ ] **Step 3: Draw failed notes in a distinct colour**

In the measurement-drawing loop (`Source/Visualizer.cpp:129-145`), before drawing each point:

```cpp
        const bool failed = failedPitches.contains (measurements[i].midiPitch);
        const Colour bandColour  = failed ? Colours::orangered.withAlpha (0.35f)
                                          : Colours::springgreen.withAlpha (0.4f);
        const Colour pointColour = failed ? Colours::orangered : Colours::green;
```

Use `bandColour` for the deviation rectangle and `pointColour` for the average line.

- [ ] **Step 4: Build and verify visually**

```bash
MACOSX_DEPLOYMENT_TARGET=11.0 cmake --build build --config Release
open build/VCOTuner_artefacts/Release/VCOTuner.app
```
Expected: the app launches. Full verification is in Task 13.

- [ ] **Step 5: Commit**

```bash
git add Source/Visualizer.h Source/Visualizer.cpp
git commit -m "feat: mark failed notes and exclude them from graph auto-scaling"
```

---

### Task 12: Non-interrupting reporting in `MainComponent`

**Files:**
- Modify: `Source/MainComponent.h` (status label, listener override)
- Modify: `Source/MainComponent.cpp:333-355` (`tunerStopped` / `tunerFinished`)

**Interfaces:**
- Consumes: Tasks 7 and 10.
- Produces: nothing.

- [ ] **Step 1: Add the failure status label**

In `Source/MainComponent.h` add `Label failureLabel;` and
`void measurementFailed (int midiPitch, vcotuner::MeasurementError reason) override;`.
Add it as a child component in the constructor and give it a row in `resized()` beneath the graph.

- [ ] **Step 2: Update the label as failures arrive**

```cpp
void MainComponent::measurementFailed (int, vcotuner::MeasurementError)
{
    const auto& failures = tuner.getFailures();

    if (failures.empty())
    {
        failureLabel.setText ({}, dontSendNotification);
        return;
    }

    StringArray pitches;
    for (const auto& f : failures)
        pitches.add (String (f.midiPitch));

    failureLabel.setText ("Not reading: MIDI " + pitches.joinIntoString (", "),
                          dontSendNotification);
}
```

Clear the label in `tunerStarted`, so each cycle starts clean.

- [ ] **Step 3: Only show a dialog in report mode**

Replace `MainComponent::tunerStopped` (`Source/MainComponent.cpp:333`):

```cpp
void MainComponent::tunerStopped()
{
    // Fatal errors only. Per-note failures never reach here; they go to the
    // status line via measurementFailed.
    StringArray errors = tuner.getLastErrors();
    for (int i = 0; i < errors.size(); i++)
        NativeMessageBox::showMessageBox (AlertWindow::WarningIcon, "Error!", errors[i]);

    startStop.setButtonText ("Start");
    cycle = false;
    creatingReport = false;
}
```

And `tunerFinished`:

```cpp
void MainComponent::tunerFinished()
{
    startStop.setButtonText ("Start");

    // A report is a single sweep with a real end, so summarise there. Live
    // tuning cycles indefinitely, so it must never raise a dialog.
    if (creatingReport)
    {
        creatingReport = false;

        const auto& failures = tuner.getFailures();
        if (! failures.empty())
        {
            StringArray lines;
            for (const auto& f : failures)
                lines.add ("  - MIDI " + String (f.midiPitch)
                           + " - " + describeError (f.reason));

            NativeMessageBox::showMessageBox (AlertWindow::InfoIcon,
                "Measurement finished",
                String (failures.size()) + " of the measured notes could not be read:\n\n"
                    + lines.joinIntoString ("\n"));
        }
    }

    if (cycle)
        tuner.toggleState();
}
```

- [ ] **Step 4: Add the error description helper**

In `Source/VCOTuner.cpp`, expose a short description used by the summary:

```cpp
String describeError (vcotuner::MeasurementError error)
{
    using vcotuner::MeasurementError;
    switch (error)
    {
        case MeasurementError::highJitter:
        case MeasurementError::highJitterTimeOut: return "unsteady rate";
        case MeasurementError::noZeroCrossings:   return "no signal detected";
        case MeasurementError::stableTimeout:     return "timed out";
        case MeasurementError::bufferFull:        return "never settled; try a lower resolution";
        default:                                  return "failed";
    }
}
```

Declare it in `Source/VCOTuner.h` outside the class.

- [ ] **Step 5: Build and run the full test suite**

```bash
MACOSX_DEPLOYMENT_TARGET=11.0 cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```
Expected: builds, all unit tests PASS.

- [ ] **Step 6: Commit**

```bash
git add Source/MainComponent.h Source/MainComponent.cpp Source/VCOTuner.h Source/VCOTuner.cpp
git commit -m "feat: report failed notes without interrupting the sweep"
```

---

### Task 13: Manual verification against real hardware

The GUI and audio-device paths cannot be meaningfully unit tested without mocking JUCE's timer, MIDI and audio stack, which the spec rules out as a non-goal. They get a written checklist instead, so "tested" stays honest about what is automated and what is not.

**Files:**
- Create: `docs/superpowers/plans/2026-09-23-manual-verification.md`

- [ ] **Step 1: Write the checklist**

```markdown
# Manual verification — measurement accuracy changes

Run against a real VCO and MIDI-to-CV interface.

## Accuracy
- [ ] Sweep a known-good VCO. Error bars are visibly narrow (a few cents),
      not tens of cents.
- [ ] Raising the resolution setting visibly narrows the error bars further.
- [ ] Reported pitch offsets match the previous version's centre values;
      only the bars should have changed.

## High pitch
- [ ] A sweep reaching MIDI 108 and above completes instead of aborting.
- [ ] MIDI 120 at the lowest resolution setting completes.

## Robustness
- [ ] A DC-coupled interface with visible DC offset measures correctly.
- [ ] A noisy or quiet source measures correctly rather than raising
      "zero crossings ... not coming in at a constant rate".
- [ ] Deliberately unplugging the audio input mid-sweep marks notes as
      failed and continues, rather than killing the run.

## Non-interrupting failures
- [ ] In live tuning, a failing note turns orange, the status line names it,
      and the sweep keeps cycling with no dialog.
- [ ] Fixing the note mid-run makes it green and drops it from the status
      line on the next cycle.
- [ ] Pressing Stop raises no dialog.
- [ ] A report run with failures shows one summary dialog at the end.

## Fatal errors still abort
- [ ] Deselecting the MIDI output device raises a dialog and stops the run.
- [ ] Disconnecting the audio device mid-run raises a dialog and stops.

## Regression
- [ ] Creating and saving a report still produces a valid .png.
- [ ] All three regime and resolution combinations still run.
```

- [ ] **Step 2: Run the full suite and the app one final time**

```bash
MACOSX_DEPLOYMENT_TARGET=11.0 cmake --build build --config Release
ctest --test-dir build -C Debug --output-on-failure
open build/VCOTuner_artefacts/Release/VCOTuner.app
```

- [ ] **Step 3: Commit**

```bash
git add docs/superpowers/plans/2026-09-23-manual-verification.md
git commit -m "docs: add manual verification checklist for measurement changes"
```

---

## Coverage map

| Spec requirement | Task |
|---|---|
| Correct interpolation formula | 4 |
| Level tracking instead of DC blocking | 2 |
| Warm-up window sourced from expected frequency | 2, 9 |
| Silence / degenerate level handling | 2 |
| Hysteresis arming, interpolation at midpoint | 3, 4 |
| `failedBufferFull` as a terminal state | 5 |
| Regression-slope period and uncertainty | 6 |
| Fatal versus per-note error classification | 7 |
| New `bufferFull` error and message | 7, 12 |
| Per-cycle failure tracking, reset each sweep | 7, 10 |
| Timeout floor and latency allowance | 8 |
| `numInputChannels` guard | 9 |
| `std::atomic` measurement flags | 9 |
| `measurementFailed` listener, continue on failure | 10 |
| Failed notes coloured, excluded from auto-scale | 11 |
| Live status line, report-mode dialog, silent stop | 12 |
| Divide-by-zero on short period sequences | 6 |
| GUI and hardware paths | 13 (manual) |

---

## Amendments made during implementation

Recorded here rather than silently rewriting the task bodies above, so the
original plan and the decisions that changed it both stay visible.

**Task 3 — test bound corrected.** The amplitude-adaptation test asserted a
period count of `>= 435`, written by estimate. The fixture yields 434
deterministically: warm-up consumes 4.4 cycles, the Schmitt trigger needs up to
another half cycle to arm, and the first crossing has no predecessor. The test
now asserts amplitude-independence, which is what it was named for.

**Task 5 — stability gate strengthened.** The gate as planned latched after a
single 5-period window and never re-validated, so a drifting oscillator could
satisfy one lucky window and be reported as a confident measurement. The spec
requires `stable` to mean genuinely steady, so `updateStability` now re-checks
the whole collected set before declaring success. The shipping app has the same
hole at `Source/VCOTuner.cpp` (it sets `indexOfFirstValidPeriodLength` once and
never rechecks).

**Task 8 — latency test fixture corrected.** `"the latency allowance is
included"` used 20 periods at 440 Hz, where both raw counts clamp to the 50-cycle
floor, making `with > without` unsatisfiable by construction. Now 400 periods.

**Task 9 — no allocation on the audio thread.** The planned callback called
`detector.reset(cfg)`, and `reset` reserves the period buffer — a heap allocation
on the real-time thread. `PeriodDetector::prepare(int)` was added so the
constructor reserves once, off the audio path.

**Tasks 9 and 10 — landed as one commit set.** Task 9 removes members Task 10's
code still references, so Task 9 could not compile alone.

**Task 11 — failed notes need a column and a shape.** As planned, `measurements`
was populated only by `newMeasurementReady`, and both the auto-scale and drawing
loops index by position in that array — so a note failing before it had ever
succeeded got no column and was not drawn at all. `measurementFailed` now upserts
a placeholder so the pitch owns a column. The marker is also a translucent fill
plus an "×" rather than the same mark recoloured: drawing a point at offset zero
would mislead (zero is where a perfectly tuned note sits), and the planned
green/orangered pair differs by only ~0.05 relative luminance, which is the
classic red/green confusion case with no brightness fallback.

**Task 12 — report summary moved to where reports end.** `startCreatingReport()`
had no caller anywhere, on master either, so `creatingReport` was never true and
the planned report-mode dialog was unreachable. The summary now fires from
`ReportDetailsEditorScreen::tunerFinished()` at its two genuine completion
points; the dead members were removed.
