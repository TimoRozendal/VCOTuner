// Source/dsp/PeriodDetector.cpp
#include "PeriodDetector.h"

namespace vcotuner
{

void PeriodDetector::reset (const PeriodDetectorConfig& config)
{
    cfg = config;
    currentStatus = DetectorStatus::collecting;
}

void PeriodDetector::processBlock (const float*, int)
{
}

} // namespace vcotuner
