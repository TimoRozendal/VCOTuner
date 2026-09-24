/*
  ==============================================================================

    VCOTuner.h
    Created: 17 May 2016 8:21:15pm
    Author:  Johannes Neumann

  ==============================================================================
*/

#ifndef VCOTUNER_H_INCLUDED
#define VCOTUNER_H_INCLUDED

#include "../JuceLibraryCode/JuceHeader.h"

#include "dsp/MeasurementError.h"
#include "dsp/MeasurementStatistics.h"
#include "dsp/PeriodDetector.h"

#include <atomic>
#include <vector>

class VCOTuner: public ChangeListener,
                private Timer,
                public AudioIODeviceCallback
{
public:
    VCOTuner(AudioDeviceManager* deviceManager);
    ~VCOTuner();
    
    void toggleState();
    void start();
    void stop();
    bool isRunning() const { return state != stopped && state != finished; }
    
    void setNumMeasurementRange(int lowestPitch, int pitchIncrement, int highestPitch);
    int getLowestPitch() const { return lowestPitch; }
    int getPitchIncrement() const { return pitchIncrement; }
    int getHighestPitch() const { return highestPitch; }
    
    void setMidiChannel(int channel) { midiChannel = channel; }
    int  getMidiChannel() const { return midiChannel; }
    
    void setResolution(int numCyclesPerNote) { numPeriodSamples = numCyclesPerNote; }
    int getResolution() { return numPeriodSamples; }
    
    double getCurrentSampleRate() { return sampleRate; }
    double getReferenceFrequency() { return referenceFrequency; }
    int getReferencePitch() const { return referencePitch; }
    
    String getStatusString()const;
    
    void startContinuousMeasurement(int pitch);
    double getContinuousMesurementResult() const { return continuousFreqMeasurementResult; }
    
    void startSingleMeasurement(int pitch);
    double getSingleMeasurementResult() const { return singleMeasurementResult; }
    
    /** holds all properties of a single measurements */
    typedef struct
    {
        int midiPitch;
        double frequency;
        double pitch; // according to the measured reference pitch
        double pitchOffset; // pitch - midiPitch
        double freqDeviation;
        double pitchDeviation;
        int numMeasurements;
        Time timestamp;
    } measurement_t;
    
    /** returns all error messages and removes them from the internal list */
    StringArray getLastErrors();
    
    /** the notes that failed to measure during the current sweep */
    const std::vector<vcotuner::NoteFailure>& getFailures() const
        { return failureTracker.failures(); }
    
    /** inherited from AudioIODeviceCallback */
    virtual void audioDeviceIOCallback (const float** inputChannelData,
                                        int numInputChannels,
                                        float** outputChannelData,
                                        int numOutputChannels,
                                        int numSamples);
    
    /** inherited from AudioIODeviceCallback */
    virtual void audioDeviceAboutToStart (AudioIODevice* device);
    
    /** inherited from AudioIODeviceCallback */
    virtual void audioDeviceStopped();
    
    /** inherited from ChangeListener */
    virtual void changeListenerCallback (ChangeBroadcaster* source);
    
    class Listener
    {
    public:
        virtual ~Listener() {}
        
        virtual void newMeasurementReady(const measurement_t& /*m*/) {}

        /** a single note could not be measured. The sweep carries on without it. */
        virtual void measurementFailed (int /*midiPitch*/,
                                        vcotuner::MeasurementError /*reason*/) {}

        virtual void tunerStarted() {}
        virtual void tunerStopped() {}
        virtual void tunerFinished() {}
        virtual void tunerStatusChanged(String /* statusString */) {}
    };
    
    void addListener(Listener* l);
    void removeListener(Listener* l);
    
private:
    // states for the state machine
    enum State
    {
        stopped,
        prepRefMeasurement,
        refMeasurement,
        prepMeasurement,
        measurement,
        finished,
        prepareContinuousFrequencyMeasurement,
        continuousFrequencyMeasurement,
        prepareSingleMeasurement,
        singleMeasurement
    };
    
    ListenerList<Listener> listeners;
    
    // processes the state machine
    virtual void timerCallback();
    void switchState(State newState);
    void trySendMidiNoteOn(int pitch);
    void trySendMidiNoteOff(int pitch);
    /** hands the detector to the audio thread for a measurement at this pitch */
    void startDetectorRun(int pitch);
    /** records the failure, tells the listeners and moves on to the next note */
    void failCurrentNote(vcotuner::MeasurementError reason);
    /** Bounded wait for the audio thread to consume a pending stop request.

        Every prep state must let a pending stop request drain before starting
        a new run, otherwise the low level state machine consumes the stale
        request and kills the run it was meant to start. Only the audio
        callback clears that flag, so with no device running nothing ever
        would: after one second this reports the failure and stops the tuner
        instead of waiting forever.

        @returns true while the caller must not proceed (either still waiting,
                 or the tuner has just been stopped); false when the flag is
                 clear and the state may carry on.
    */
    bool awaitingStopRequest();
    /** the user facing message for a detector status that is not 'stable' */
    const String& errorMessageForStatus(vcotuner::DetectorStatus status) const;
    int currentlyPlayingMidiNote;
    
    // counts cycles since the last state transition
    int cycleCounter;

    /** counts consecutive cycles a prep state has spent waiting for a pending
        stop request to be consumed. Kept separate from cycleCounter, which
        gates the MIDI note-on (cycleCounter == 0) and the 100 ms oscillator
        settling window: advancing that one while waiting would skip the
        note-on entirely and shorten the settling time. */
    int stopWaitCounter = 0;

    
    /** lowest pitch to be measured */
    int lowestPitch;
    /** pitch increment */
    int pitchIncrement;
    /** highest pitch to be measured */
    int highestPitch;
    
    int currentPitch;
    int currentIndex;
    
    /** midi note for which the reference measurement was done. */
    int referencePitch = 0;
    /** frequency returned during the reference measurement, 0 until measured */
    float referenceFrequency = 0.0f;
    
    /** a list with recent error messages */
    StringArray errors;
    
    AudioDeviceManager* deviceManager;
    int midiChannel;
    
    /** state of the state machine */
    State state;
    
    
    /** The detector is owned by the audio thread while startMeasurement is true.
        The message thread may read it only after it has observed startMeasurement
        == false, which the audio thread publishes after its last write. */
    vcotuner::PeriodDetector detector;
    std::atomic<bool> startMeasurement { false }; // set by message thread, reset by audio thread.
    std::atomic<bool> stopMeasurement  { false }; // set by message thread, reset by audio thread.
    /** the detector's status, published by the audio thread after every block */
    std::atomic<int>  detectorStatusFlag { (int) vcotuner::DetectorStatus::collecting };

    vcotuner::DetectorStatus lastDetectorStatus() const noexcept
        { return (vcotuner::DetectorStatus) detectorStatusFlag.load(); }

    int numPeriodSamples; // number of periods to measure before averaging
    /** length of the detector's level tracking window, sized per note */
    int currentWarmupSamples = 2048;

    /** the notes that failed during the current sweep */
    vcotuner::FailureTracker failureTracker;

    /** written in audioDeviceAboutToStart, before any measurement can run */
    double sampleRate = 44100.0;
    /** only to be accessed from the audio thread */
    bool initialized = false;
    
    int continuousFrequencyMeasurementPitch;
    /** -1 until a pass has settled: ReportPrepScreen polls this and advances
        the report wizard on it, so it must never hold an undefined value. */
    double continuousFreqMeasurementResult = -1.0;
    double continuousFreqMeasurementDeviation = 0.0;
    
    int singleMeasurementPitch;
    double singleMeasurementResult;
    double singleMeasurementDeviation;
    
    struct Errors
    {
        static const String highJitter;
        static const String noZeroCrossings;
        static const String bufferFull;
        static const String stableTimeout;
        static const String noFrequencyChangeBetweenMeasurements;
        static const String noMidiDeviceAvailable;
        static const String audioDeviceStoppedDuringMeasurement;
    };
};

/** Short, user-facing description of a per-note measurement failure, for the
    end-of-report summary dialog. Lives here rather than in Source/dsp/
    because it returns a JUCE String. */
String describeError (vcotuner::MeasurementError error);

/** Shows the end-of-report summary dialog (one NativeMessageBox naming every
    failed note) when failures is non-empty; does nothing otherwise. Shared
    by every path that finishes a report, so the wording only lives in one
    place. Currently called from ReportDetailsEditorScreen, at the point
    where the report's measurement is actually complete. */
void showMeasurementFailureSummary (const std::vector<vcotuner::NoteFailure>& failures);


#endif  // VCOTUNER_H_INCLUDED
