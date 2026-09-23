// Source/dsp/PeriodDetector.cpp
#include "PeriodDetector.h"

namespace vcotuner
{

void PeriodDetector::reset (const PeriodDetectorConfig& config)
{
    cfg = config;
    currentStatus   = DetectorStatus::collecting;
    sampleCounter   = 0;
    warmupRemaining = config.warmupSamples;
    runningMin      =  1e30;
    runningMax      = -1e30;
    levelMidpoint   = 0.0;
    levelAmplitude  = 0.0;
    haveLevel       = false;
}

void PeriodDetector::processBlock (const float* samples, int numSamples)
{
    if (samples == nullptr || numSamples <= 0)
        return;

    for (int i = 0; i < numSamples; ++i)
    {
        const double s = (double) samples[i];

        // Level tracking runs continuously so the detector follows slow
        // level changes, not just the warm-up window.
        if (s < runningMin) runningMin = s;
        if (s > runningMax) runningMax = s;

        if (warmupRemaining > 0)
        {
            if (--warmupRemaining == 0)
                finishWarmup();
        }

        ++sampleCounter;
    }
}

void PeriodDetector::finishWarmup()
{
    levelMidpoint  = (runningMax + runningMin) * 0.5;
    levelAmplitude = (runningMax - runningMin) * 0.5;

    if (levelAmplitude < cfg.silenceFloor)
    {
        levelAmplitude = 0.0;
        currentStatus  = DetectorStatus::failedNoCrossings;
        return;
    }

    haveLevel = true;
}

} // namespace vcotuner
