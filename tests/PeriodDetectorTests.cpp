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
