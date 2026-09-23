# VCOTuner measurement accuracy and robustness — design

Date: 2026-09-23
Status: approved, ready for implementation planning

## Problem

Three user-reported problems, which investigation showed share mostly one root cause:

1. Measurements abort with "zero crossings ... don't seem to be coming in at a
   constant rate" (`Errors::highJitter`) even when the oscillator sounds fine.
   A single failed note discards the entire sweep.
2. Error bars are large and their meaning is unclear.
3. Uncertainty about whether sub-sample interpolation of zero crossings is used
   and whether it applies.

### Root cause 1: the interpolation formula is wrong

`Source/VCOTuner.cpp:505-511` interpolates between the samples either side of a
zero crossing, but the algebra is incorrect:

```cpp
double m = (lastSample - currentSample);   // negated slope
double n = lastSample - m*(sampleCounter); // lastSample is at sampleCounter-1
double zeroCrossingPos = -n / m;
```

With `f` the true fractional crossing position within the sample interval:

- correct:    `x0 = (sampleCounter-1) + f`
- this code:  `x0 = (sampleCounter-1) + (1 - f)`

The fractional part is **mirrored** within the interval. It is correct only at
`f = 0.5` and worst at the interval ends.

Measured against a synthetic sine (jitter in samples, standard deviation of
measured period length):

| variant                  | 440 Hz | 4186 Hz | 8372 Hz |
|--------------------------|--------|---------|---------|
| current code             | 0.574  | 0.992   | 0.866   |
| correct interpolation    | 0.000  | 0.007   | 0.022   |
| no interpolation at all  | 0.287  | 0.499   | 0.442   |

The current interpolation is roughly **twice as bad as not interpolating**. It
injects about one sample of jitter per crossing rather than removing it.

Effect on what the user sees, on a mathematically perfect oscillator:

| MIDI | current code   | with fix |
|------|----------------|----------|
| 69   | ±9.5 cents     | ±0.0     |
| 84   | ±30.7 cents    | ±0.0     |
| 96   | ±47.1 cents    | ±0.0     |
| 108  | aborts         | ±0.0     |
| 120  | aborts         | ±6.5     |

The *mean* period is essentially unaffected (~0.005 cents), so reported tuning
values have always been correct. Only the spread is wrong — which is why the
oscillator sounds fine while the app complains.

### Root cause 2: no hysteresis and no DC handling

The detector uses a hard `0.0` threshold (`Source/VCOTuner.cpp:500`) with no
hysteresis and no DC handling. Noise or DC offset produces multiple triggers per
cycle. Simulated, 1 s of 220 Hz (220 true periods expected):

| noise | DC offset | crossings found | with ±10% hysteresis |
|-------|-----------|-----------------|----------------------|
| 2%    | 0         | 222             | 220                  |
| 5%    | 0         | 314             | 220                  |
| 2%    | 0.9       | 305             | 220                  |
| 5%    | 0.9       | 609             | 221                  |

This is the literal "multiple zero crossings" case, and a DC-coupled interface
makes it routine.

### Root cause 3: error bars answer the wrong question

`Source/VCOTuner.cpp:232-243` computes the standard deviation of *individual*
period measurements; `Source/Visualizer.cpp:135-136` draws it as the error bar
on the *mean*. Those are different quantities. See §2 for the correct estimator.

Knock-on effect: `Source/Visualizer.cpp:36-44` auto-scales the plot to include
the error bars, so oversized bars compress the actual tuning curve into a thin
strip.

### Secondary defects found

- **Premature timeout at high pitch** (`Source/VCOTuner.cpp:270-275`).
  `expectedCycles = roundToInt(expectedTime * 100)` evaluates to **0** for e.g.
  8372 Hz at 20 periods, giving the measurement ~10 ms to complete — less than
  typical MIDI+audio round-trip latency.
- **Stall until timeout at high resolution** (`Source/VCOTuner.cpp:544-565`).
  `maxNumPeriodLengths` is 600 but resolution reaches 400. If the signal
  stabilises late, the buffer caps, neither the finish branch nor the abort
  branch can fire, and the run stalls until the top-level timeout, then reports
  the misleading `stableTimeout`.
- **Unguarded channel access** (`Source/VCOTuner.cpp:472-499`). Only
  `inputChannelData == nullptr` is checked; `numInputChannels` never is, then
  `getSample(0, i)` reads channel 0 unconditionally.
- **Divide by zero** (`Source/VCOTuner.cpp:240-241`): `/(numMeasurements - 1)`
  yields NaN if only one period is collected. Currently unreachable via the UI.
- **Data race**: `startMeasurement` / `stopMeasurement` are plain `bool`s shared
  between the audio and message threads.

## Non-goals

- Autocorrelation or FFT-based pitch detection. Upstream attempted this on
  `feature/improvedMeasurement` and abandoned it ("works but takes way too
  long"). Fixing the interpolation makes it unnecessary.
- Abstracting the JUCE timer / MIDI layer so whole sweeps run headlessly.
  Considered and rejected: high mocking cost, little additional safety.
- Changing the report file format or the stored report schema.

## Design

Three pure C++ units with no JUCE dependency, plus a thin JUCE adapter.

### §1 `PeriodDetector` — `Source/dsp/PeriodDetector.{h,cpp}`

Owns everything from input samples to validated period lengths.

**Level tracking, not DC blocking.** A high-pass blocker would tilt the waveform
and add phase distortion at the low end (default lowest pitch is MIDI 30 ≈
46 Hz). Instead track min/max over a warm-up window at the start of each
measurement:

```
midpoint  = (min + max) / 2
amplitude = (max - min) / 2
```

This handles DC offset, asymmetric waveforms and varying module output levels
with no filtering.

**Where the warm-up window comes from.** The state machine's existing 100 ms
settle wait happens *before* `startMeasurement` is set, so the detector never
sees that audio. The detector therefore runs its own warm-up over the first
`warmupSamples` samples it receives: it updates min/max but emits no crossings.
`VCOTuner` sets `warmupSamples` to cover two cycles at the expected frequency,
which it already computes for the timeout (`Source/VCOTuner.cpp:270`), clamped
to a sane floor and ceiling. Level tracking continues after warm-up so the
detector follows slow level changes.

**Silence and degenerate levels.** If `amplitude` falls below a small fixed
floor, the input is treated as silent: the detector reports
`failedNoCrossings` rather than computing a threshold from noise. This also
removes any divide-by-zero in the threshold calculation.

**Hysteresis for arming, interpolation at the midpoint.** Re-arm when the signal
falls below `midpoint - hysteresisFraction * amplitude`; fire when it next
crosses `midpoint` upward. Noise immunity comes from the hysteresis, while the
timestamp still lands on the steepest and most repeatable part of the waveform.

**Correct interpolation** at the fire point:

```cpp
const double slope = currentSample - lastSample;   // > 0 by construction
const double crossing = (sampleCounter - 1) - lastSample / slope;
```

Proposed interface:

```cpp
struct PeriodDetectorConfig {
    double sampleRate          = 48000.0;
    double hysteresisFraction  = 0.1;   // of measured amplitude
    int    stabilityWindow     = 5;
    double stabilityTolerance  = 0.1;   // 10%
    int    maxPeriods          = 600;
    int    warmupSamples       = 2048;  // level-tracking window; VCOTuner
                                        // overrides per note (see above)
    double silenceFloor        = 1e-4;  // amplitude below this => silent
};

enum class DetectorStatus {
    collecting,         // still gathering
    stable,             // enough valid periods collected
    failedUnstable,     // never reached a stable rate
    failedNoCrossings,  // no crossings seen at all
    failedBufferFull    // ran out of storage before stabilising
};
```

`failedBufferFull` is a distinct status specifically so the stall described
above becomes a real terminating state rather than a hang.

### §2 `MeasurementStatistics` — `Source/dsp/MeasurementStatistics.{h,cpp}`

Two corrections to earlier assumptions, both recorded here deliberately:

1. Computing frequency from the total span is **not** an improvement over
   averaging period lengths — they are algebraically identical, since the
   periods telescope (`Σpᵢ = t_N − t_0`). The existing mean is already the
   right estimator.
2. `σ/√N` is **not** the correct error bar either. Consecutive period lengths
   share crossing times, so their errors are negatively correlated and `σ/√N`
   overestimates the uncertainty by roughly a further factor of √N.

The estimator used instead: **fit a straight line to crossing time against
index.** The slope is the period; the **standard error of the slope** is the
uncertainty. This is the textbook estimator, it degrades gracefully in the
presence of genuine VCO drift as well as timing jitter, and it is cleanly
testable against analytically-known inputs.

Outputs: frequency, pitch, pitch offset, and frequency/pitch uncertainty derived
from the slope standard error. Must return a well-defined result (not NaN) for
degenerate inputs of fewer than three points.

### §3 Error policy — `Source/dsp/MeasurementError.{h,cpp}`

Pure classification:

- **Fatal** — abort the run and show a dialog, as today:
  `noMidiDeviceAvailable`, `audioDeviceStoppedDuringMeasurement`,
  `noFrequencyChangeBetweenMeasurements`
- **Per-note** — mark the note and continue:
  `highJitter`, `noZeroCrossings`, `highJitterTimeOut`, `stableTimeout`,
  and a new `bufferFull`

`bufferFull` is new, and exists because `DetectorStatus::failedBufferFull`
currently has no honest user-facing message: today that path stalls and is
eventually reported as `stableTimeout`, whose text ("coming in at a constant
rate ... much slower than they should be") describes something else entirely.
It needs its own message stating that the signal never settled within the
measurement buffer, and suggesting a lower resolution setting.

Plus per-cycle failure tracking, reset at the start of every sweep.

### §4 Integration

`VCOTuner` keeps the state machine and becomes a thin adapter:

- Owns a `PeriodDetector`, feeds it audio from the callback.
- New listener callback `measurementFailed(int pitch, MeasurementError reason)`.
- On a per-note failure: record it, notify, advance to the next pitch instead of
  calling `switchState(stopped)`.
- Timeout gains a floor and latency headroom so `expectedCycles` can never be 0.
- `numInputChannels` guarded before sample access.
- `startMeasurement` / `stopMeasurement` become `std::atomic<bool>`.

`Visualizer`:

- Failed notes drawn in a distinct colour.
- Failed notes **excluded from auto-scaling**, so one failure cannot wreck the
  vertical zoom.

`MainComponent`:

- Live tuning mode (`cycle == true`): never a dialog. A status line below the
  graph names the currently failing notes. The list resets each cycle, so a note
  that starts reading correctly drops off immediately. Pressing Stop reports
  nothing; the final state stays on screen.
- Report mode (single sweep): one consolidated dialog listing all failures when
  the sweep completes.
- Fatal errors: dialog and abort in both modes.

### §5 Tests

Catch2 via CMake `FetchContent`, a `VCOTunerTests` target, `ctest` integration,
and a job added to `.github/workflows/CI.yaml`.

`PeriodDetector`:

- Interpolation exactness on synthetic sine, saw and square across the pitch
  range; recovered frequency within a tight cents tolerance.
- **Regression test pinning the mirrored-fraction bug** — asserts measured
  jitter is below a threshold the old formula provably cannot meet.
- Hysteresis against the noise and DC-offset cases tabulated above: correct
  crossing count where the old detector found 314 / 305 / 609.
- Level tracking on asymmetric waveforms and on widely differing input levels.
- Stability detection: reports `stable` only once the rate is genuinely steady.
- `failedNoCrossings` on silence; `failedBufferFull` on a never-stabilising
  signal, asserting it terminates rather than hanging.

`MeasurementStatistics`:

- Known period sequences produce known frequency, pitch and pitch offset.
- Slope standard error matches an analytically computed value.
- Uncertainty shrinks as more periods are supplied.
- Degenerate inputs (0, 1, 2 points) return defined values, never NaN.

`MeasurementError`:

- Full classification table, fatal versus per-note.
- Per-cycle failure list resets between sweeps.

Timeout:

- `expectedCycles` is never 0 across the whole supported pitch and resolution
  range — a table-driven test over every combination.

## Risks

- Extracting the detector touches the audio callback, the one place where a
  regression is least visible in tests. Mitigation: the detector is pure and
  heavily tested; the adapter layer stays as thin as possible.
- Changing the error bar definition makes new reports non-comparable with old
  saved ones. Accepted: the old bars were wrong.
- Catch2 via `FetchContent` requires network access at configure time. CI
  already fetches submodules, so this is consistent with existing practice.
