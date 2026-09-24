#include "MeasurementTiming.h"

#include <algorithm>
#include <cmath>

namespace vcotuner
{

int computeTimeoutCycles (double expectedFrequency,
                          int numPeriods,
                          double timerIntervalSeconds,
                          double latencyAllowanceSeconds)
{
    constexpr int minimumCycles = 50;   // 500 ms at the default 10 ms timer

    if (expectedFrequency <= 0.0 || numPeriods <= 0 || timerIntervalSeconds <= 0.0)
        return minimumCycles;

    const double measurementTime = (numPeriods / expectedFrequency) * 2.0;
    const double totalTime = measurementTime + std::max (0.0, latencyAllowanceSeconds);
    const int cycles = (int) std::ceil (totalTime / timerIntervalSeconds);

    return std::max (cycles, minimumCycles);
}

} // namespace vcotuner
