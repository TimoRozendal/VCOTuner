#pragma once

namespace vcotuner
{

/** How many timer cycles to wait before declaring a measurement timed out.

    Covers the time needed to observe numPeriods cycles of the expected
    frequency, doubled for headroom, plus a fixed allowance for MIDI and
    audio round-trip latency, and floored so it can never reach zero.
*/
int computeTimeoutCycles (double expectedFrequency,
                          int numPeriods,
                          double timerIntervalSeconds,
                          double latencyAllowanceSeconds);

} // namespace vcotuner
