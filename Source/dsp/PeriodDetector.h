// Source/dsp/PeriodDetector.h
#pragma once

#include <vector>

namespace vcotuner
{

struct PeriodDetectorConfig
{
    double sampleRate         = 48000.0;
    double hysteresisFraction = 0.1;    // of measured amplitude
    int    stabilityWindow    = 5;      // consecutive periods compared
    double stabilityTolerance = 0.1;    // 10% spread allowed
    int    maxPeriods         = 600;    // storage limit
    int    warmupSamples      = 2048;   // level-tracking window
    double silenceFloor       = 1e-4;   // amplitude below this => silent
    int    requiredPeriods    = 10;     // valid periods needed for 'stable'
};

enum class DetectorStatus
{
    collecting,         // still gathering
    stable,             // enough valid periods collected
    failedUnstable,     // never reached a steady rate
    failedNoCrossings,  // silent, or no crossings at all
    failedBufferFull    // ran out of storage before stabilising
};

class PeriodDetector
{
public:
    void reset (const PeriodDetectorConfig& config);
    void processBlock (const float* samples, int numSamples);

    DetectorStatus status() const noexcept { return currentStatus; }

private:
    PeriodDetectorConfig cfg {};
    DetectorStatus currentStatus = DetectorStatus::collecting;
};

} // namespace vcotuner
