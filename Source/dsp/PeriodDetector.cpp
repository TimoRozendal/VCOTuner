// Source/dsp/PeriodDetector.cpp
#include "PeriodDetector.h"
#include <cmath>

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

    periods.clear();
    periods.reserve ((size_t) config.maxPeriods);
    lastCrossing    = -1.0;
    lastSample      = 0.0;
    armed           = false;
    haveLastSample  = false;
    firstValidIndex = -1;
}

void PeriodDetector::processBlock (const float* samples, int numSamples)
{
    if (samples == nullptr || numSamples <= 0)
        return;

    for (int i = 0; i < numSamples; ++i)
    {
        const double s = (double) samples[i];

        // Min/max keep updating, but the trigger level is latched once at the
        // end of warm-up: a threshold that drifts mid-measurement would inject
        // timing error into exactly the periods we are trying to measure.
        if (s < runningMin) runningMin = s;
        if (s > runningMax) runningMax = s;

        if (warmupRemaining > 0)
        {
            if (--warmupRemaining == 0)
                finishWarmup();
        }

        if (haveLevel && currentStatus == DetectorStatus::collecting)
            processCrossing (s);

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

void PeriodDetector::processCrossing (double s)
{
    const double hysteresis = cfg.hysteresisFraction * levelAmplitude;

    // Re-arm only after the signal has dropped clearly below the midpoint.
    // Noise between the rails cannot retrigger.
    if (! armed)
    {
        if (s < levelMidpoint - hysteresis)
            armed = true;
    }
    else if (haveLastSample && lastSample < levelMidpoint && s >= levelMidpoint)
    {
        armed = false;

        // Linear interpolation between the two samples straddling the
        // midpoint. The previous sample sits at sampleCounter - 1.
        //   correct:  x0 = (sampleCounter - 1) + f
        // The old code computed (sampleCounter - 1) + (1 - f), mirroring the
        // fraction within the interval and roughly doubling the jitter versus
        // no interpolation at all.
        const double slope = s - lastSample;               // > 0 by the branch
        const double f = (slope != 0.0)
                       ? (levelMidpoint - lastSample) / slope
                       : 0.0;
        recordCrossing ((double) (sampleCounter - 1) + f);
    }

    lastSample = s;
    haveLastSample = true;
}

void PeriodDetector::recordCrossing (double position)
{
    if ((int) periods.size() >= cfg.maxPeriods)
        return;

    if (lastCrossing >= 0.0)
        periods.push_back (position - lastCrossing);

    lastCrossing = position;

    updateStability();
}

int PeriodDetector::numValidPeriods() const noexcept
{
    if (firstValidIndex < 0) return 0;
    return (int) periods.size() - firstValidIndex;
}

const double* PeriodDetector::validPeriods() const noexcept
{
    if (firstValidIndex < 0) return periods.data();
    return periods.data() + firstValidIndex;
}

void PeriodDetector::updateStability()
{
    const int n = (int) periods.size();

    if (firstValidIndex < 0 && n >= cfg.stabilityWindow)
    {
        double sum = 0.0;
        for (int i = n - cfg.stabilityWindow; i < n; ++i)
            sum += periods[(size_t) i];
        const double average  = sum / cfg.stabilityWindow;
        const double boundary = average * cfg.stabilityTolerance;

        bool steady = true;
        for (int i = n - cfg.stabilityWindow; i < n; ++i)
            if (std::abs (periods[(size_t) i] - average) >= boundary)
                steady = false;

        if (steady)
            firstValidIndex = n;
    }

    if (firstValidIndex >= 0 && numValidPeriods() >= cfg.requiredPeriods)
    {
        currentStatus = DetectorStatus::stable;
        return;
    }

    // Storage exhausted before collecting what we need. This must be a
    // terminal state: the old code left the measurement running here, so it
    // stalled until the top level timed out and then blamed the wrong thing.
    // Distinguish the two causes - never steady at all, versus steady but not
    // for long enough - because they need different advice to the user.
    if (n >= cfg.maxPeriods)
        currentStatus = (firstValidIndex < 0) ? DetectorStatus::failedUnstable
                                              : DetectorStatus::failedBufferFull;
}

} // namespace vcotuner
