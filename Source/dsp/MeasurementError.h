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
