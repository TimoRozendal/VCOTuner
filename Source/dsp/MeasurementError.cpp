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
