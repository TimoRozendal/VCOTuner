/*
  ==============================================================================

    VCOTuner.cpp
    Created: 17 May 2016 8:21:15pm
    Author:  Johannes Neumann

  ==============================================================================
*/

#include "../JuceLibraryCode/JuceHeader.h"
#include "VCOTuner.h"

#include "dsp/MeasurementTiming.h"

#include <cmath>

namespace
{
    /** How long a prep state waits for a pending stop request to be consumed.

        Only the audio callback clears stopMeasurement, so the wait is bounded:
        100 cycles of the 10 ms timer is one second, which is several times the
        longest realistic buffer period (8192 frames at 44.1 kHz is 186 ms),
        yet finite when no device is running at all.
    */
    constexpr int maxStopWaitCycles = 100;

    /** Translates what the detector reported into the error the sweep records. */
    vcotuner::MeasurementError errorForStatus (vcotuner::DetectorStatus status)
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

    /** Frequency and its uncertainty from the periods the detector collected.

        computeMeasurement() cannot serve the paths that use this: it also
        expresses a pitch, which needs a reference frequency that these paths
        either do not have yet or do not care about.
    */
    bool fitFrequency (const vcotuner::PeriodDetector& detector, double sampleRate,
                       double& frequency, double& deviation)
    {
        const auto fit = vcotuner::fitPeriod (detector.validPeriods(),
                                              detector.numValidPeriods());

        if (! fit.valid || fit.periodSamples <= 0.0 || sampleRate <= 0.0)
            return false;

        frequency = sampleRate / fit.periodSamples;
        deviation = frequency * (fit.periodStdError / fit.periodSamples);
        return true;
    }
}

VCOTuner::VCOTuner(AudioDeviceManager* d)
{
    state = stopped;
    numPeriodSamples = 10;
    lowestPitch = 30;
    highestPitch = 120;
    pitchIncrement = 12;
    deviceManager = d;
    midiChannel = 1;
    currentlyPlayingMidiNote = -1;
    
    // Reserve the detector's storage here, on the message thread. reset() is
    // called from the audio callback and asks for this same capacity, which
    // makes its reserve a no-op instead of a real-time heap allocation.
    detector.prepare(vcotuner::PeriodDetectorConfig().maxPeriods);
    
    d->addChangeListener(this);
    d->addAudioCallback(this);
    
    startTimer(10);
}

VCOTuner::~VCOTuner()
{
    stopTimer();
    
    if (currentlyPlayingMidiNote >= 0)
        trySendMidiNoteOff(currentlyPlayingMidiNote);
    
    deviceManager->removeAudioCallback(this);
}


void VCOTuner::setNumMeasurementRange(int lPitch, int pitchInc, int hPitch)
{
    lowestPitch = lPitch;
    pitchIncrement = pitchInc;
    highestPitch = hPitch;
}

void VCOTuner::addListener(Listener* l)
{
    listeners.add(l);
}

void VCOTuner::removeListener(Listener* l)
{
    listeners.remove(l);
}

void VCOTuner::toggleState()
{
    if (!isRunning())
    {
        switchState(prepRefMeasurement);
    }
    else
    {
        switchState(stopped);
    }
}

void VCOTuner::start()
{
    if (!isRunning())
        switchState(prepRefMeasurement);
}

void VCOTuner::stop()
{
    if (isRunning())
        switchState(stopped);
}

void VCOTuner::startSingleMeasurement(int pitch)
{
    if (state != stopped && state != finished)
        switchState(stopped);
    
    singleMeasurementPitch = pitch;
    singleMeasurementResult = -1;
    
    switchState(prepareSingleMeasurement);
}

StringArray VCOTuner::getLastErrors()
{
    StringArray tmp = errors;
    errors.clear();
    return tmp;
}

void VCOTuner::timerCallback()
{
    switch (state)
    {
        case stopped:
            break;
        case prepRefMeasurement:
            // wait for the low level state machine to stop measuring (bounded;
            // see awaitingStopRequest). Changing the pitch-range or resolution
            // combo while running stops and restarts the tuner within a single
            // message-thread call stack, so a stop request can still be pending
            // here when the restart arrives.
            if (awaitingStopRequest())
                break;
            
            if (cycleCounter == 0)
            {
                // send reference midi note
                referencePitch = (highestPitch + lowestPitch) / 2;
                currentPitch = referencePitch;
                trySendMidiNoteOn(currentPitch);
            }
            else
            {
                // after 100ms, we expect the oscillator to be settled at the new pitch
                // (gives some safety margin for audio/midi interface latency)
                if (cycleCounter >= 10)
                {
                    // start a measurement and see if we get a stable pitch here
                    startDetectorRun(currentPitch);
                    switchState(refMeasurement);
                    break;
                }
            }
            cycleCounter++;
            break;
        case refMeasurement:
        {
            // measurement done
            if (!startMeasurement)
            {
                // send note off
                trySendMidiNoteOff(currentPitch);
                
                // A failed reference measurement is always fatal: every other
                // note's pitch is expressed relative to this frequency, so
                // without it the rest of the sweep would be meaningless.
                const vcotuner::DetectorStatus status = lastDetectorStatus();
                
                if (status != vcotuner::DetectorStatus::stable)
                {
                    errors.add(errorMessageForStatus(status));
                    switchState(stopped);
                    break;
                }
                
                double frequency = 0.0, deviation = 0.0;
                if (!fitFrequency(detector, sampleRate, frequency, deviation))
                {
                    errors.add(Errors::highJitter);
                    switchState(stopped);
                    break;
                }
                
                referenceFrequency = (float) frequency;
                
                // prepare next measurement
                currentPitch = lowestPitch;
                currentIndex = 0;
                switchState(prepMeasurement);
                break;
            }

            if (cycleCounter > 1000)
            {
                errors.add(errorMessageForStatus(lastDetectorStatus()));
                stopMeasurement = true;
                switchState(stopped);
                break;
            }
            cycleCounter++;
            break;
        }
        case prepMeasurement:
            // wait for low level state machine to stop measuring: handing it a
            // new run while a stop request is pending would have it consume the
            // stale request and kill the run it was meant to start. Bounded;
            // see awaitingStopRequest.
            if (awaitingStopRequest())
                break;
            
            if (cycleCounter == 0)
            {
                // send midi note
                trySendMidiNoteOn(currentPitch);
            }
            else
            {
                // after 100ms, we expect the oscillator to be settled at the new pitch
                // (gives some safety margin for audio/midi interface latency)
                if (cycleCounter >= 10)
                {
                    // start a measurement and see if we get a stable pitch here
                    startDetectorRun(currentPitch);
                    switchState(measurement);
                    break;
                }
            }
            cycleCounter++;
            break;
        case measurement:
        {
            // measurement done
            if (!startMeasurement)
            {
                // A single note that cannot be measured is not fatal: record it
                // and carry on with the sweep. failCurrentNote() sends the note
                // off, so it is not sent here.
                const vcotuner::DetectorStatus status = lastDetectorStatus();
                
                if (status != vcotuner::DetectorStatus::stable)
                {
                    failCurrentNote(errorForStatus(status));
                    break;
                }
                
                const auto result = vcotuner::computeMeasurement(detector.validPeriods(),
                                                                 detector.numValidPeriods(),
                                                                 sampleRate,
                                                                 referenceFrequency,
                                                                 referencePitch);
                if (!result.valid)
                {
                    failCurrentNote(vcotuner::MeasurementError::highJitter);
                    break;
                }
                
                // send note off
                trySendMidiNoteOff(currentPitch);
                
                // check if the frequency has changed compared to the reference frequency
                // if not, it is likely that the MIDI output is not working. Do this only for the very first measurement
                if (currentIndex == 0
                    && std::abs(result.frequency - referenceFrequency) / referenceFrequency < 0.1)
                {
                    errors.add(Errors::noFrequencyChangeBetweenMeasurements);
                    switchState(stopped);
                    break;
                }
                
                measurement_t m;
                m.timestamp = Time::getCurrentTime();
                m.frequency = result.frequency;
                m.pitch = result.pitch;
                m.midiPitch = currentPitch;
                m.pitchOffset = result.pitch - currentPitch;
                m.freqDeviation = result.frequencyDeviation;
                m.pitchDeviation = result.pitchDeviation;
                m.numMeasurements = detector.numValidPeriods();
                listeners.call(&Listener::newMeasurementReady, m);
                
                // prepare next measurement
                currentPitch += pitchIncrement;
                currentIndex++;
                
                if (currentPitch <= highestPitch)
                    switchState(prepMeasurement);
                else
                    switchState(finished);
                break;
            }
            
            const double expectedFrequency = referenceFrequency
                * std::pow(2.0, ((double) currentPitch - (double) referencePitch) / 12.0);
            const int expectedCycles = vcotuner::computeTimeoutCycles(expectedFrequency,
                                                                      numPeriodSamples,
                                                                      0.01, 0.3);
            if (cycleCounter > expectedCycles)
            {
                failCurrentNote(errorForStatus(lastDetectorStatus()));
                break;
            }
            cycleCounter++;
            break;
        }
        case finished:
            break;
        case prepareContinuousFrequencyMeasurement:
        {
            // wait for low level state machine to stop measuring (bounded;
            // see awaitingStopRequest)
            if (awaitingStopRequest())
                break;
                
            // send midi note and start measuring
            trySendMidiNoteOn(continuousFrequencyMeasurementPitch);
            startDetectorRun(continuousFrequencyMeasurementPitch);
            switchState(continuousFrequencyMeasurement);
            cycleCounter++;
        } break;
        case continuousFrequencyMeasurement:
        {
            // if the measurement is done)
            if (!startMeasurement)
            {
                // Keep the previous reading when this one did not settle - this
                // mode runs until the user stops it, so there is nobody to tell.
                double frequency = 0.0, deviation = 0.0;
                if (lastDetectorStatus() == vcotuner::DetectorStatus::stable
                    && fitFrequency(detector, sampleRate, frequency, deviation))
                {
                    continuousFreqMeasurementResult = frequency;
                    continuousFreqMeasurementDeviation = deviation;
                }
                
                // restart measurement
                startDetectorRun(continuousFrequencyMeasurementPitch);
            }
            cycleCounter++;
        } break;
        case prepareSingleMeasurement:
        {
            // wait for low level state machine to stop measuring (bounded;
            // see awaitingStopRequest)
            if (awaitingStopRequest())
                break;
            
            if (cycleCounter == 0)
            {
                // send midi note
                trySendMidiNoteOn(singleMeasurementPitch);
            }
            else
            {
                // after 100ms, we expect the oscillator to be settled at the new pitch
                // (gives some safety margin for audio/midi interface latency)
                if (cycleCounter >= 10)
                {
                    // start a measurement and see if we get a stable pitch here
                    startDetectorRun(singleMeasurementPitch);
                    switchState(singleMeasurement);
                    break;
                }
            }
            cycleCounter++;
        } break;
        case singleMeasurement:
        {
            // measurement done
            if (!startMeasurement)
            {
                // send note off
                trySendMidiNoteOff(singleMeasurementPitch);
                
                const vcotuner::DetectorStatus status = lastDetectorStatus();
                
                if (status != vcotuner::DetectorStatus::stable)
                {
                    errors.add(errorMessageForStatus(status));
                    switchState(stopped);
                    break;
                }
                
                double frequency = 0.0, deviation = 0.0;
                if (!fitFrequency(detector, sampleRate, frequency, deviation))
                {
                    errors.add(Errors::highJitter);
                    switchState(stopped);
                    break;
                }
                
                singleMeasurementResult = frequency;
                singleMeasurementDeviation = deviation;
                
                switchState(finished);
                break;
            }
            
            // timeout handling
            if (cycleCounter > 1000)
            {
                errors.add(errorMessageForStatus(lastDetectorStatus()));
                stopMeasurement = true;
                switchState(stopped);
                break;
            }
            cycleCounter++;
        } break;
        default:
            state = stopped;
            break;
    }
}

void VCOTuner::startContinuousMeasurement(int pitch)
{
    continuousFrequencyMeasurementPitch = pitch;
    continuousFreqMeasurementResult = -1.0;
    continuousFreqMeasurementDeviation = 0.0;
    if (state != stopped && state != finished)
        switchState(stopped);
    state = prepareContinuousFrequencyMeasurement;
}

void VCOTuner::trySendMidiNoteOn(int pitch)
{
    MidiOutput* midiOut = deviceManager->getDefaultMidiOutput();
    if (midiOut == nullptr)
    {
        errors.add(Errors::noMidiDeviceAvailable);
        switchState(stopped);
        return;
    }
    
    if (currentlyPlayingMidiNote != -1)
        trySendMidiNoteOff(currentlyPlayingMidiNote);
    
    midiOut->sendMessageNow(MidiMessage::noteOn(midiChannel, pitch, (uint8_t) 100));
    currentlyPlayingMidiNote = pitch;
}

void VCOTuner::trySendMidiNoteOff(int pitch)
{
    MidiOutput* midiOut = deviceManager->getDefaultMidiOutput();
    if (midiOut == nullptr)
    {
        errors.add(Errors::noMidiDeviceAvailable);
        switchState(stopped);
        return;
    }
    
    midiOut->sendMessageNow(MidiMessage::noteOff(midiChannel, pitch));
    currentlyPlayingMidiNote = -1;
}

void VCOTuner::startDetectorRun(int pitch)
{
    // Size the detector's level tracking window to two cycles of the frequency
    // we expect at this pitch: the trigger level is latched at the end of that
    // window, so a low note needs a longer look at the signal than a high one.
    const double expectedFreq = (state == prepRefMeasurement || referenceFrequency <= 0.0f)
        ? 440.0 * std::pow(2.0, (pitch - 69) / 12.0)
        : referenceFrequency * std::pow(2.0, (pitch - referencePitch) / 12.0);
    const double twoCycles = (expectedFreq > 0.0) ? (2.0 * sampleRate / expectedFreq) : 2048.0;
    currentWarmupSamples = jlimit(256, 48000, (int) twoCycles);
    
    // Publish 'collecting' before handing the detector to the audio thread, so
    // that the state machine cannot read the previous run's terminal status
    // while it is waiting for this one.
    detectorStatusFlag = (int) vcotuner::DetectorStatus::collecting;
    startMeasurement = true;
}

bool VCOTuner::awaitingStopRequest()
{
    if (!stopMeasurement)
    {
        stopWaitCounter = 0;
        return false;
    }

    if (++stopWaitCounter <= maxStopWaitCycles)
        return true;

    // Nothing has consumed the stop request for a full second, so no audio
    // callback is running. Every prep state used to wait here forever, which
    // looked to the user exactly like the tuner having frozen. Report it with
    // the message that already describes this situation and stop.
    errors.add(Errors::audioDeviceStoppedDuringMeasurement);
    switchState(stopped);   // resets stopWaitCounter along with cycleCounter
    return true;
}

void VCOTuner::failCurrentNote(vcotuner::MeasurementError reason)
{
    trySendMidiNoteOff(currentPitch);
    
    // Only cancel a run that is actually in flight - the timeout path. When the
    // detector finished on its own the audio thread has already cleared its own
    // state, and a stop request left armed here would be consumed by the next
    // note's run instead.
    if (startMeasurement)
        stopMeasurement = true;
    
    // trySendMidiNoteOff() stops the tuner when the MIDI device has gone away.
    // That is fatal, so do not resume the sweep on top of it.
    if (state == stopped)
        return;
    
    failureTracker.recordFailure(currentPitch, reason);
    listeners.call(&Listener::measurementFailed, currentPitch, reason);
    
    // currentIndex deliberately does not advance here: it counts *successful*
    // measurements, and the "MIDI-to-CV interface isn't responding" check keys
    // on currentIndex == 0 to run on the first one. Advancing it on failure
    // would skip that check for the whole sweep whenever the first note fails.
    currentPitch += pitchIncrement;
    
    if (currentPitch <= highestPitch)
        switchState(prepMeasurement);
    else
        switchState(finished);
}

const String& VCOTuner::errorMessageForStatus(vcotuner::DetectorStatus status) const
{
    switch (status)
    {
        case vcotuner::DetectorStatus::failedNoCrossings:
            return Errors::noZeroCrossings;
        case vcotuner::DetectorStatus::failedUnstable:
            return Errors::highJitter;
        case vcotuner::DetectorStatus::failedBufferFull:
            // Distinct from highJitter: the signal may have been perfectly
            // steady, it just needed more storage than this resolution setting
            // allows before it could be confirmed stable.
            return Errors::bufferFull;
        case vcotuner::DetectorStatus::collecting:
        case vcotuner::DetectorStatus::stable:
            break;
    }

    // Still collecting when the caller gave up. This covers two different
    // situations that look the same from here: the crossings never settled
    // into a steady rate, or the signal was too weak/intermittent for enough
    // of them to arrive in the first place (warm-up never finished). Say
    // neither is confirmed rather than asserting the first.
    return Errors::stableTimeout;
}

/** Short, user-facing description of a per-note measurement failure. Declared
    in VCOTuner.h (outside the class) rather than in Source/dsp/, since it
    returns a JUCE String and Source/dsp/ must stay JUCE-free. */
String describeError (vcotuner::MeasurementError error)
{
    using vcotuner::MeasurementError;
    switch (error)
    {
        case MeasurementError::highJitter:
        case MeasurementError::highJitterTimeOut: // unreachable; see MeasurementError.h
            return "unsteady rate";
        case MeasurementError::noZeroCrossings:
            return "no signal detected";
        case MeasurementError::stableTimeout:
            return "timed out";
        case MeasurementError::bufferFull:
            return "settled, but not long enough; try a lower resolution";
        case MeasurementError::none:
        case MeasurementError::noFrequencyChange:
        case MeasurementError::noMidiDevice:
        case MeasurementError::audioDeviceStopped:
        default:
            // The fatal reasons never reach here - they abort the sweep and
            // are reported through tunerStopped() instead of the per-note
            // failure list this describes.
            return "failed";
    }
}

void showMeasurementFailureSummary (const std::vector<vcotuner::NoteFailure>& failures)
{
    if (failures.empty())
        return;

    StringArray lines;
    for (const auto& f : failures)
        lines.add ("  - MIDI " + String (f.midiPitch) + " - " + describeError (f.reason));

    NativeMessageBox::showMessageBox (AlertWindow::InfoIcon,
        "Measurement finished",
        String (failures.size()) + " of the measured notes could not be read:\n\n"
            + lines.joinIntoString ("\n"));
}

/** inherited from AudioIODeviceCallback */
void VCOTuner::audioDeviceIOCallback (const float** inputChannelData,
                                    int numInputChannels,
                                    float** outputChannelData,
                                    int numOutputChannels,
                                    int numSamples)
{
    // Channels that were not enabled when the device was opened are null, and
    // AudioBuffer::clear() would memset straight through them.
    if (outputChannelData != nullptr)
    {
        for (int channel = 0; channel < numOutputChannels; channel++)
            if (outputChannelData[channel] != nullptr)
                FloatVectorOperations::clear(outputChannelData[channel], numSamples);
    }

    if (stopMeasurement)
    {
        startMeasurement = false;
        stopMeasurement = false;
        initialized = false;
    }

    if (!startMeasurement)
        return;

    // Guard the channel access: numInputChannels was never checked before,
    // so a device with no enabled input channels read out of bounds.
    if (inputChannelData == nullptr || numInputChannels <= 0
        || inputChannelData[0] == nullptr)
        return;

    if (!initialized)
    {
        vcotuner::PeriodDetectorConfig cfg;
        cfg.sampleRate      = sampleRate;
        cfg.requiredPeriods = numPeriodSamples;
        cfg.warmupSamples   = currentWarmupSamples;
        // cfg.maxPeriods must stay at the default the constructor reserved,
        // otherwise this reset() would allocate on the audio thread.
        jassert(cfg.maxPeriods == vcotuner::PeriodDetectorConfig().maxPeriods);
        detector.reset(cfg);
        initialized = true;
    }

    detector.processBlock(inputChannelData[0], numSamples);
    detectorStatusFlag = (int) detector.status();

    if (detector.status() != vcotuner::DetectorStatus::collecting)
    {
        initialized = false;
        // Published last: the state machine treats this as permission to read
        // the detector, so every write above must already be visible.
        startMeasurement = false;
    }
}

void VCOTuner::switchState(VCOTuner::State newState)
{
    cycleCounter = 0;
    stopWaitCounter = 0;
    state = newState;
    if (state == stopped)
    {
        if (currentlyPlayingMidiNote >= 0 && currentlyPlayingMidiNote < 128)
            trySendMidiNoteOff(currentlyPlayingMidiNote);
        stopMeasurement = true;
        listeners.call(&Listener::tunerStopped);
    }
    else if (newState == prepRefMeasurement)
    {
        // a new sweep: the status line should describe this pass, not history
        failureTracker.beginSweep();
        listeners.call(&Listener::tunerStarted);
    }
    else if (newState == finished)
        listeners.call(&Listener::tunerFinished);
    
    listeners.call(&Listener::tunerStatusChanged, getStatusString());
}

/** inherited from AudioIODeviceCallback */
void VCOTuner::audioDeviceAboutToStart (AudioIODevice* device)
{
    sampleRate = device->getCurrentSampleRate();
}

/** inherited from AudioIODeviceCallback */
void VCOTuner::audioDeviceStopped()
{
	if (isRunning())
        errors.add(Errors::audioDeviceStoppedDuringMeasurement);

    switchState(stopped);
}

void VCOTuner::changeListenerCallback (ChangeBroadcaster* source)
{
    if (source == deviceManager)
    {
        switchState(stopped);
    }
}

String VCOTuner::getStatusString() const 
{
    switch(state)
    {
        case stopped:
            return "Stopped.";
            break; // these breaks are only here to prevent IDE warnings...
        case prepRefMeasurement:
        case refMeasurement:
            return "Measuring reference frequency ...";
            break;
        case prepMeasurement:
        case measurement:
            return "Measuring frequency for MIDI note " + String(currentPitch) + " ...";
            break;
        case finished:
            return "Finished.";
            break;
        case prepareContinuousFrequencyMeasurement:
        case continuousFrequencyMeasurement:
            return "Continuously measuring frequency...";
            break;
        case prepareSingleMeasurement:
        case singleMeasurement:
            return "Measuring frequency for MIDI note " + String(singleMeasurementPitch) + " ...";
        default:
            return "";
            break;
    }
}

const String VCOTuner::Errors::highJitter = "There are zero crossings in the incoming signal but they don't seem to be coming in at a constant rate. Are you sure you're recording on the correct channel? Please use only primitive waveforms (saw, square, triangle, sine, ...) without any other processing such as delays, reverbs, etc. This error typically appears when you are accidentally recording the signal from a microphone or another sound source. Or when you have dropouts (aka clicks and pops) in your audio.";

const String VCOTuner::Errors::noZeroCrossings = "The incoming audio signal does not seem to contain any zero-crossings. Are you sure the oscillator signal is getting through to us? Check your audio device settings.";

const String VCOTuner::Errors::bufferFull = "The signal did settle into a steady rate - it just didn't hold that rate long enough to finish the measurement before the buffer ran out of storage. Try a lower resolution setting (fewer periods per note); needing fewer periods means the same buffer is enough to complete the measurement.";

const String VCOTuner::Errors::stableTimeout = "The measurement did not finish in time. Either the incoming zero-crossings never settled into a steady rate, or the signal was too weak or intermittent for enough of them to arrive in the first place. Are you recording from the right oscillator, on the right channel, and is its level high enough?";

const String VCOTuner::Errors::noFrequencyChangeBetweenMeasurements = "Apparently the frequency of the oscillator is not changing between measurements. Please check if your MIDI-to-CV interface is set to the correct MIDI channel and make sure that it is selected as the default midi output device in the audio and midi settings.";

const String VCOTuner::Errors::noMidiDeviceAvailable = "You don't have a MIDI output device selected or the selected device is not available.";

const String VCOTuner::Errors::audioDeviceStoppedDuringMeasurement = "The audio device was stopped while the measurement was still running. Please check that the device is still powered, all cables are connected and the driver is working correctly.";
