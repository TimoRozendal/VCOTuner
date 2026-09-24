# Manual verification — measurement accuracy changes

Work through this with a real VCO and a real MIDI-to-CV interface. It is the
entire verification for everything the automated test suite cannot reach —
treat it as the actual gate for this branch, not a formality to skim before a
merge.

## What the automated suite covers, and what it can't

33 Catch2 tests (`ctest --test-dir build -C Debug`) cover the pure
`vcotuner_dsp` library — logic with no JUCE dependency, in `Source/dsp/`:

| File | Tests | Covers |
|---|---|---|
| `PeriodDetectorTests.cpp` | 16 | Zero-crossing interpolation, hysteresis arming, level tracking / warm-up (the DC/noise immunity), silence and degenerate-input handling, stability detection, terminal statuses (`failedUnstable`, `failedNoCrossings`, `failedBufferFull`) |
| `MeasurementStatisticsTests.cpp` | 9 | The regression-slope period/uncertainty fit that replaced the old standard-deviation-of-periods error bar, the divide-by-zero guard on short period sequences, the two-period boundary |
| `MeasurementErrorTests.cpp` | 4 | Fatal vs. per-note error classification, per-sweep failure list, reset on each new sweep |
| `MeasurementTimingTests.cpp` | 4 | The timeout-floor fix, across the entire MIDI 0–127 range, confirming it never evaluates to zero cycles |

What none of this touches: JUCE at all. That means the `VCOTuner` state
machine, the real audio callback receiving real hardware buffers, real MIDI
output, `AudioDeviceManager` device-loss callbacks, and every GUI class
(`MainComponent`, `Visualizer`, the report wizard screens) are untested by
machine. Mocking JUCE's timer/MIDI/audio stack to close that gap was ruled a
non-goal for this work — so this document is where that gap is closed by a
person instead. If a box below is unchecked, that behaviour is unverified,
full stop; it is not covered "in spirit" by the unit tests next to it.

## Setup

- [ ] A VCO with a clean waveform output (saw, square, triangle or sine),
      through a MIDI-to-CV interface, into an audio interface input.
- [ ] Build Release and launch it:
      ```
      cmake --build build --config Release
      open build/VCOTuner_artefacts/Release/VCOTuner.app
      ```
      (The `MACOSX_DEPLOYMENT_TARGET=11.0` prefix this used to need is gone as
      of the JUCE 8 upgrade — JUCE 6.1.5 called `CGWindowListCreateImage`,
      which Apple obsoleted in the macOS 15 SDK.)
- [ ] Audio and MIDI devices selected in the in-app audio settings panel, and
      the MIDI-to-CV interface confirmed to be the selected MIDI output.

## 1. Accuracy and error bars

- [ ] Start live tuning on a known-good VCO with the "huge > normal (24-96,
      +6)" regime and resolution "100 - okay". Confirm the shaded band around
      each point (the error bar) is a few cents tall, not the tens-of-cents
      bands the old build produced.
- [ ] With the same VCO and range, switch resolution from "20 - quick & dirty"
      to "400 - never accurate enough" (its label is a leftover from the old
      behaviour, not a warning about the current one) and let a full cycle
      complete at each setting. Confirm the bars visibly narrow as resolution
      increases — this specifically did not happen before this branch.
- [ ] Compare the plotted centre value (the horizontal green line per column,
      not the shaded band) for several notes against a report or screenshot
      made before this branch, or against `master`. Confirm the centre values
      are materially unchanged — only the bar width should differ, since the
      interpolation fix corrects jitter, not the mean.

## 2. High pitch

- [ ] Run the "huge > fine (24-96, +1)" regime, the highest pitch reachable
      through the shipped UI (both live-tuning regimes and report generation
      cap at MIDI 96 — see `Source/MainComponent.cpp`'s `regimes` array and
      `Source/ReportProperties.h`'s `highestPitch`). Confirm MIDI 96 completes
      normally at every resolution setting, in particular "400 - never
      accurate enough", which takes the longest per note.
- [ ] To confirm the original bug is actually fixed in situ, not only by the
      unit test that exercises `computeTimeoutCycles` numerically across MIDI
      0–127: temporarily edit `Source/ReportProperties.h`, changing
      `highestPitch` from `96` to `120`, rebuild, and run a report (or add a
      13th entry to `MainComponent::regimes` in `Source/MainComponent.cpp`
      with `endNote = 120` and use live tuning instead — either works).
      Confirm a sweep reaching MIDI 108 and above completes rather than
      aborting instantly. **Revert the edit afterward** — it is a
      verification aid for this checklist, not a product change, and must
      not be committed.

## 3. Robustness

- [ ] If your audio interface is DC-coupled and the VCO output carries a
      visible DC offset (check on a scope, or note if the interface's own
      meters show it), sweep it directly without additional AC-coupling and
      confirm it measures correctly instead of over-triggering.
- [ ] Introduce mild noise into the signal path (a slightly hot gain stage,
      an unshielded cable, or similar) and confirm the sweep measures rather
      than raising the "zero crossings ... don't seem to be coming in at a
      constant rate" dialog/status.
- [ ] Attenuate the VCO output to a very quiet level (well below the
      interface's nominal input level, but still above the noise floor) and
      confirm it still measures — the trigger threshold now scales to the
      measured amplitude rather than using a fixed level.
- [ ] Drive the VCO output hot (close to clipping the input, but not
      clipping) and confirm it also still measures correctly at that level.
- [ ] Deliberately unplug the audio input cable mid-sweep. Confirm the
      affected note(s) are marked as failed and the sweep continues, rather
      than the whole run dying silently or hanging.

## 4. Failed notes — the core new behaviour

- [ ] In live tuning, deliberately make one note fail on the very **first**
      attempt of the session (e.g. temporarily disconnect the VCO's CV input
      so one specific note mistunes into silence or off-range, or unplug the
      audio input for a moment while that note is playing, then restore it).
      Confirm that note appears on the graph as a distinct marker the very
      first time it is drawn — this specifically failed to render at all in
      an earlier revision (no column was drawn for a note that had never
      succeeded).
- [ ] Look closely at the failed marker: a translucent orange-red fill over
      the full height of that note's column, plus a bold "×" drawn across it.
      Confirm it reads as clearly distinct from a normal green
      measurement column at a glance, including with red/green colour vision
      simulated or checked (the shape, not just the colour, should carry the
      meaning).
- [ ] With the same note still failing, confirm the status line under the
      graph names it (e.g. "Not reading: MIDI 84") while live tuning cycles.
- [ ] Fix whatever caused that note to fail (reconnect the cable / CV). Let
      the sweep complete its current cycle and start the next one. Confirm
      the note renders as a normal green measurement on the next cycle and
      drops off the status line.
- [ ] While a note is failing in live tuning, confirm no dialog box appears
      at any point during the cycling — not when the note first fails, not
      on any subsequent cycle.
- [ ] With a note still failing, press Stop. Confirm no dialog box appears.
- [ ] Run a report (not live tuning) with at least one note failing somewhere
      in the range. Confirm exactly **one** summary dialog appears, at the
      end of the whole report (after the reference-pitch re-check, not after
      the main sweep), titled "Measurement finished", listing each failed
      MIDI pitch and a short reason (e.g. "unsteady rate", "no signal
      detected", "timed out", "settled, but not long enough; try a lower
      resolution").
- [ ] Known cosmetic overlap to check, not necessarily to fix: the blue
      reference-pitch highlight is drawn after the failed-note marker, so if
      the reference pitch itself fails to measure, the blue tint draws over
      its orange-red column. Look at this specific case (make the reference
      pitch note fail) and judge whether the failed marker (the fill and the
      "×") is still legible through the blue tint. Note your judgement either
      way — this is a known, accepted trade-off unless it turns out the
      marker becomes unreadable.

## 5. Fatal errors must still abort

- [ ] Start a sweep, then deselect the MIDI output device in the audio
      settings panel (or otherwise make no MIDI device available). Confirm a
      dialog appears and the run stops.
- [ ] Start a sweep, then disconnect the audio device entirely (unplug the
      interface, or select "no device" if your OS allows it) mid-run.
      Confirm a dialog appears and the run stops.
- [ ] In both cases above, confirm the dialog appears immediately — not only
      after the current note's full timeout — and that it is a single
      "Error!" dialog, not the per-note failure summary from section 4.

## 6. Report generation

- [ ] Run "Create Report" end to end on a real oscillator and save the
      result. Confirm it produces a valid, viewable `.png` file.
- [ ] With the reference pitch held rock-steady (no drift beyond
      `ReportProperties::desiredDriftMargin`), confirm the report completes
      with **no** drift dialog at all ("within margin") and the failure
      summary (if any notes failed) still appears once, at the very end.
- [ ] Deliberately cause drift beyond the margin during a report (nudge the
      VCO's tuning slightly partway through, or use an oscillator that is
      still warming up). When the "Warning: High drift!" dialog appears:
  - [ ] Click **"Repeat"**. Confirm the sweep restarts from the beginning,
        any previously failed notes from the aborted attempt are cleared
        (the status/summary reflects only the repeated attempt), and the
        report proceeds normally from there.
  - [ ] On a separate run, click **"Keep the poor results"**. Confirm the
        report proceeds using the drifted measurement, and the end-of-report
        failure summary (if any notes failed) appears exactly once,
        immediately after this choice.
  - [ ] On a separate run, click **"Cancel"**. Confirm the report wizard
        closes without producing a report and without showing the per-note
        failure summary dialog.

## 7. Regression

- [ ] Run at least one sweep in each of the four range regimes (narrow,
      medium, large, huge) and, within one of them, each of the five
      resolution settings (20, 50, 100, 200, 400). Confirm all combinations
      complete without errors, hangs, or crashes.
- [ ] Deliberately produce a dead note (e.g. silence the audio input for one
      note only) at the lowest pitch in range and the highest resolution
      (MIDI 24, resolution 400). Time how long it takes to report failure and
      move on. Expect roughly 25 seconds. Confirm that, watching it happen,
      this reads as "the app is patiently waiting out a timeout" rather than
      "the app has hung" — note your subjective impression, since this
      figure was accepted as a deliberate trade-off (thorough integration
      time at very low pitch) rather than a bug.

## JUCE 8 upgrade — the two things a build cannot prove

Added when the project moved from JUCE 6.1.5 to 8.0.15. JUCE 8 removed
`AudioIODeviceCallback::audioDeviceIOCallback()` and replaced it with
`audioDeviceIOCallbackWithContext()`. The base-class implementation of the new
one is an empty body, so a class that keeps the old signature still compiles
cleanly, launches, and renders its UI — while never receiving a single sample.
A fork of this project shipped in exactly that state.

Our override is marked `override`, which makes that specific mistake a compile
error rather than a silent failure. These checks exist because the build still
cannot prove the path is live end to end.

- [ ] **Audio actually reaches the detector.** Start a sweep with the
      oscillator connected. If any note produces a frequency reading at all,
      the callback is being called. A sweep where *every* note fails with
      "no signal detected" is the signature of a dead callback — distinguish
      it from a genuinely disconnected input by checking the input level in
      the audio settings panel first.
- [ ] **MIDI actually sends.** Confirm the oscillator's pitch audibly changes
      as the sweep advances. If the pitch never moves, the sweep should abort
      with the "MIDI-to-CV interface is not responding" error rather than
      silently reporting a flat line.

Both changed in the upgrade and neither is covered by the automated suite,
which links no JUCE at all.

- [ ] **Graph axis labels still lay out correctly.** JUCE 8 removed
      `Font::getStringWidth()` and changed text metrics; the pitch-axis label
      spacing was migrated to `GlyphArrangement::getStringWidth()`. Check the
      MIDI note numbers along the bottom of the graph are evenly spaced, not
      overlapping, and not dropping out at narrow window widths. Resize the
      window to its minimum and back.

## Out-of-range markers in the report

The report plots a fixed +/-15 cents (`ReportDisplayScreen.cpp`). A note whose
reading falls outside that used to draw nothing, so a badly tracking oscillator
went blank at exactly the notes worth looking at - indistinguishable from a note
that was never measured. An arrow now marks the edge the reading ran off.

The live graph auto-scales to fit every reading, so it can never trigger there;
this is a report-only behaviour.

- [ ] Produce a report from an oscillator that is off by more than 15 cents
      somewhere in its range - detune it deliberately if need be. Confirm those
      notes show a solid triangle at the top edge (reading too high) or the
      bottom edge (too low), rather than a blank column.
- [ ] Confirm the arrow points the way the reading went, and sits in that note's
      own column, lined up with its MIDI number on the axis.
- [ ] Confirm notes still inside +/-15 cents draw normally, with their band and
      centre line, and get no arrow.

Verified during development against a synthetic fixture: readings of +28 and
-31 cents produced correct up and down arrows while in-range notes drew normally.
What remains unverified is the path through real report generation with real
measurements.
