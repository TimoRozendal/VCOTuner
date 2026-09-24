// Source/dsp/MeasurementStatistics.h
#pragma once

namespace vcotuner
{

struct PeriodFit
{
    bool   valid          = false;
    double periodSamples  = 0.0;
    double periodStdError = 0.0;
};

struct MeasurementResult
{
    bool   valid              = false;
    double frequency          = 0.0;
    double frequencyDeviation = 0.0;
    double pitch              = 0.0;
    double pitchDeviation     = 0.0;
};

/** Least-squares fit of crossing time against crossing index.

    The periods are cumulated back into crossing times, then fitted with a
    straight line. The slope is the period estimate and the standard error of
    that slope is its uncertainty.

    This replaces taking the standard deviation of the individual periods,
    which answers a different question: consecutive periods share a crossing
    time, so their errors are negatively correlated and the spread of the
    periods badly overstates the uncertainty of their mean.

    Requires at least 2 periods (3 crossing times) to return a fit; fewer
    returns valid == false with finite zeroed fields. Exactly 2 periods
    leaves a single degree of freedom (n - 2 == 1), which is still a
    well-defined standard error but a deliberately wide one -- a large
    error bar is precisely how low confidence should be communicated,
    rather than discarding the measurement outright.
*/
PeriodFit fitPeriod (const double* periods, int numPeriods);

/** Converts a period sequence into frequency, pitch and their uncertainties. */
MeasurementResult computeMeasurement (const double* periods, int numPeriods,
                                      double sampleRate,
                                      double referenceFrequency,
                                      int referencePitch);

} // namespace vcotuner
