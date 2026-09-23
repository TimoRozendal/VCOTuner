// Source/dsp/MeasurementStatistics.cpp
#include "MeasurementStatistics.h"

#include <cmath>
#include <vector>

namespace vcotuner
{

PeriodFit fitPeriod (const double* periods, int numPeriods)
{
    PeriodFit fit;

    if (periods == nullptr || numPeriods < 2)
        return fit;

    // Cumulate periods back into crossing times: n+1 points for n periods.
    const int n = numPeriods + 1;
    std::vector<double> times ((size_t) n);
    times[0] = 0.0;
    for (int i = 0; i < numPeriods; ++i)
        times[(size_t) (i + 1)] = times[(size_t) i] + periods[i];

    double meanX = 0.0, meanY = 0.0;
    for (int i = 0; i < n; ++i) { meanX += i; meanY += times[(size_t) i]; }
    meanX /= n;
    meanY /= n;

    double sxx = 0.0, sxy = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double dx = i - meanX;
        sxx += dx * dx;
        sxy += dx * (times[(size_t) i] - meanY);
    }

    if (sxx <= 0.0)
        return fit;

    const double slope = sxy / sxx;
    const double intercept = meanY - slope * meanX;

    fit.periodSamples = slope;

    // Standard error of the slope needs at least one degree of freedom.
    if (n > 2)
    {
        double sse = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double residual = times[(size_t) i] - (intercept + slope * i);
            sse += residual * residual;
        }
        fit.periodStdError = std::sqrt (sse / ((n - 2) * sxx));
        fit.valid = true;
    }

    return fit;
}

MeasurementResult computeMeasurement (const double* periods, int numPeriods,
                                      double sampleRate,
                                      double referenceFrequency,
                                      int referencePitch)
{
    MeasurementResult result;

    const auto fit = fitPeriod (periods, numPeriods);
    if (! fit.valid || fit.periodSamples <= 0.0
        || referenceFrequency <= 0.0 || sampleRate <= 0.0)
        return result;

    result.frequency = sampleRate / fit.periodSamples;

    // Relative uncertainty carries straight across from period to frequency.
    const double relative = fit.periodStdError / fit.periodSamples;
    result.frequencyDeviation = result.frequency * relative;

    result.pitch = 12.0 * std::log2 (result.frequency / referenceFrequency)
                 + referencePitch;

    // d(pitch)/d(f) = 12 / (f * ln2), so the semitone uncertainty is just the
    // relative uncertainty scaled by 12 / ln2.
    result.pitchDeviation = 12.0 * relative / std::log (2.0);
    result.valid = true;

    return result;
}

} // namespace vcotuner
