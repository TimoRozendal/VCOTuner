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
    // Currently unreachable: the detector always reaches a terminal status by
    // itself (see DetectorStatus), so a top-level timeout only ever observes
    // 'collecting', which maps to stableTimeout instead. Kept rather than
    // removed because MeasurementErrorTests.cpp asserts isFatal() on it;
    // delete both together if this enumerator is ever pruned.
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
