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
