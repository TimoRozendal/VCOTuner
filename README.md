# VCOTuner
A JUCE based tuner application for VCOs, VCFs and other analog gear. It runs on Windows, Mac and Linux.

## Overview

**How tuning usually works** - Tuning is usually a tedious ping-pong game between adjusting a fine tune pot and adjusting one or multiple tuning trimmers. Whenever a trimmer has been adjusted, the fine tune pot has to be adjusted as well to bring the pitch back to a specific note. 

**How tuning works with the app** - The app spits out midi notes and measures the frequency. This is done for multiple notes in a user selectable note range. At first the note in the center of the range is selected as the reference pitch. All other measurements will be compared to this reference. This removes the need to adjust the fine tune pot. Tuning the oscillator is just a matter of tweaking the trimmers and looking at the screen. Takes no longer than a few minutes. See the video below. 

**The application can also produce a report** that features measurements in the highest accuracy and over a very wide pitch range. Reports are saved as a *.png file including information on the device under test and the CV interface that was used. 

This video shows how to use it:

<a href="http://www.youtube.com/watch?feature=player_embedded&v=JpMFTOBXuv8
" target="_blank"><img src="http://img.youtube.com/vi/JpMFTOBXuv8/0.jpg" 
alt="Youtube tutorial video" width="400" border="0" /></a>

## What's new

**Measurements are much more accurate.** The app finds each zero crossing more precisely than the sample rate alone allows, by interpolating between the two samples either side of it. That calculation was wrong — it mirrored the result within the sample interval, which added roughly *twice* as much timing jitter as doing no interpolation at all. With it fixed, the jitter at 440 Hz drops from about a third of a sample to essentially nothing.

You'll notice this as **far narrower error bars**. On a perfectly good oscillator they used to be tens of cents wide; on a clean signal they're now a fraction of a cent. Raising the resolution setting narrows them further, which it previously did not.

**The error bars were also measuring the wrong thing.** They showed the spread of the individual period readings rather than the uncertainty of the averaged result that's actually plotted. They now show the uncertainty of the number on screen.

**A bad note no longer ruins the whole sweep.** Any note that failed to measure used to pop up a dialog and stop the run. Now it's marked on the graph with an orange "×", named in a status line beneath it, and the sweep carries on. Fix the cause mid-run — tweak a trimmer, reseat a cable — and that note goes back to normal on the next pass.

Live tuning never interrupts you with a dialog at all. Report mode shows a single summary at the end listing anything that failed. Errors that genuinely mean nothing can work — no MIDI device selected, the audio device disappearing, the MIDI-to-CV interface not responding — still stop the run and tell you why.

**Far fewer false "unstable signal" errors.** The old detector triggered on a fixed threshold at zero with no noise immunity, so noise or a DC offset on the input produced several false triggers per cycle — on a noisy signal it could find 609 crossings where 220 was correct. That's what produced the "zero crossings ... don't seem to be coming in at a constant rate" error on oscillators that sounded perfectly fine. The trigger now adapts to the measured signal level and ignores noise between its thresholds. Very quiet and very hot signals both measure correctly with no adjustment.

**High notes complete.** Each note's timeout was calculated in a way that rounded down to zero at the top of the range, so the measurement gave up before the audio could physically arrive. High notes now get a sensible minimum.

**A drifting oscillator is now flagged instead of quietly measured.** The stability check used to confirm the signal was steady across five consecutive cycles and then never look again — so an oscillator that drifted after that point was still reported as a confident reading. It now re-checks the whole measurement before accepting it.

One thing to expect from that: a note whose capture contains an audible click or dropout will now be marked as failed rather than absorbed into a wider error bar. At the highest resolution setting, where each note is measured over hundreds of cycles, a single glitch anywhere in the capture is enough to do it. That's deliberate — a flagged note is more useful than a plausible wrong number — but if you start seeing failures where you didn't before, suspect the audio path before the oscillator.

**Runs on current macOS.** Updated to JUCE 8, which removes the build workarounds previously needed on macOS 15 and later.

## Download

[Head over to the "release" section of this repository to download the latest release.](https://github.com/TheSlowGrowth/VCOTuner/releases/latest)

macOS downloads are not signed by Apple, so Gatekeeper will refuse to open them and claim the app is damaged. It isn't — right-click the app and choose Open, or clear the quarantine flag:

```
xattr -dr com.apple.quarantine VCOTuner.app
```

## Building from source

Requires CMake 3.22 or later and a C++17 compiler.

```
git clone --recursive https://github.com/TheSlowGrowth/VCOTuner.git
cd VCOTuner
cmake -B build
cmake --build build --config Release
```

On Linux, install the dependencies first:

```
sudo apt-get install g++ libfreetype6-dev libfontconfig1-dev libx11-dev \
    libxinerama-dev libxrandr-dev libxcursor-dev mesa-common-dev \
    libasound2-dev freeglut3-dev libxcomposite-dev
```

The measurement code has an automated test suite. It is not built by default:

```
cmake -B build -DVCOTUNER_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build
```

## Help to improve it

[If you find bugs, please raise an issue here!](https://github.com/TheSlowGrowth/VCOTuner/issues)

## Are you on Muff's?

[Here's a thread on MuffWiggler. Post your tuning reports here, if you like](https://www.muffwiggler.com/forum/viewtopic.php?p=2276045)
