/*
  ==============================================================================

    Visualizer.cpp
    Created: 22 May 2016 2:45:34pm
    Author:  Johannes Neumann

  ==============================================================================
*/

#include "../JuceLibraryCode/JuceHeader.h"
#include "Visualizer.h"


Visualizer::Visualizer(VCOTuner* t)
{
    tuner = t;
}

Visualizer::~Visualizer()
{
    
}


void Visualizer::paint(juce::Graphics &g, int width, int height)
{
    if (measurements.size() == 0)
    {
        g.drawText("No Data", 0, 0, width, height, juce::Justification::centred);
        return;
    }
    
    // calculate display range
    double max = 0;
    double min = 0;
    for (int i = 0; i < measurements.size(); i++)
    {
        if (failedPitches.contains (measurements[i].midiPitch))
            continue;

        double value = measurements[i].pitchOffset;
        double deviation = measurements[i].pitchDeviation;
        if (value - deviation < min)
            min = value - deviation;
        if (value + deviation > max)
            max = value + deviation;
    }
    double expandAmount = (max - min) * 0.2;
    min -= expandAmount;
    max += expandAmount;
    
    paintWithFixedScaling(g, width, height, min, max);
}

void Visualizer::paintWithFixedScaling(Graphics& g, int width, int height, double min, double max)
{
    if (measurements.size() == 0)
    {
        g.drawText("No Data", 0, 0, width, height, juce::Justification::centred);
        return;
    }
    
    const int bottomBarHeight = 20;
    
    int imageHeight = height - bottomBarHeight;
    
    // prepare coordinate transformation (flipping the y axis)
    heightForFlipping = (float) imageHeight;
    
    const float sidebarWidth = 75;
    double columnWidth = (double) (width - sidebarWidth) / (double) measurements.size();
    const double allowedPitchOffset = 0.02; // 5 cents allowed
    
    if (min > -allowedPitchOffset)
        min = -allowedPitchOffset;
    if (max < allowedPitchOffset)
        max = allowedPitchOffset;
    if (max < min)
        return;
    
    double vertScaling = (double) imageHeight / (max - min);
    
    // draw maximum "in-tune" pitch offset and center line
    g.setColour(Colours::grey);
    const float dashLengths[] = {4, 4};
    double position = (allowedPitchOffset - min) * vertScaling;
    g.drawDashedLine(Line<float>(sidebarWidth, yFlip((float) position), (float) width, yFlip((float) position)), dashLengths, 2);
    position = (-allowedPitchOffset - min) * vertScaling;
    g.drawDashedLine(Line<float>(sidebarWidth, yFlip((float) position), (float) width, yFlip((float) position)), dashLengths, 2);
    position = (- min) * vertScaling;
    g.drawLine(sidebarWidth, yFlip((float) position), (float) width, yFlip((float) position));
    
    // draw scales
    g.setColour(Colours::grey);
    const int numIntervals = 13;
    const double allowedIntervals[numIntervals] = {0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 20, 50 };
    double lineInterval = allowedIntervals[0];
    int currentIntervalIndex = 0;
    
    int numLinesAllowed = imageHeight / 30;
    while (((max - min) / lineInterval) > numLinesAllowed)
    {
        lineInterval = allowedIntervals[++currentIntervalIndex];
        if (currentIntervalIndex >= numIntervals)
            break;
    }
    bool useSemitoneTexts = lineInterval >= 1.0;
    
    int numPosLines = (int) trunc(max/lineInterval);
    int numNegLines = (int) trunc(-min/lineInterval);
    for (double y = numPosLines; y > -numNegLines; y--)
    {
        double linePos = (y*lineInterval - min) * vertScaling;
        double left = sidebarWidth - 2;
        double number = y*lineInterval*((useSemitoneTexts)?1.0:100.0);
        String numberString = (std::abs(number - round(number)) > 0.1)?String(number, 1):String((int)round(number));
        String lineText = numberString;
        if (!useSemitoneTexts)
            lineText += " cents";
        g.drawText(lineText, juce::Rectangle<float>(0.0f, yFlip(float(linePos) + g.getCurrentFont().getHeight()/2.0f), float(left) - 4, g.getCurrentFont().getHeight()), Justification::centredRight);
        
        // don't overwrite maximum "in-tune" lines
        if (y*lineInterval == allowedPitchOffset || y*lineInterval == -allowedPitchOffset)
            continue;
        
        const float lineDashLengths[] = {4, 20};
        g.drawDashedLine(Line<float>((float) left, yFlip((float) linePos), (float) width, yFlip((float) linePos)), lineDashLengths, 2);
        
    }
    
    // draw pitch measurement points
    for (int i = 0; i < measurements.size(); i++)
    {
        float left = sidebarWidth + i*(float)columnWidth;

        if (failedPitches.contains (measurements[i].midiPitch))
        {
            // no usable measurement for this note: a point/band at offset
            // zero would misleadingly look "in tune", so mark the whole
            // column instead with a shape that doesn't depend on hue.
            g.setColour (Colours::orangered.withAlpha (0.15f));
            g.fillRect (left, 0.0f, (float) columnWidth, (float) imageHeight);

            g.setColour (Colours::orangered);
            const float margin = (float) columnWidth * 0.25f;
            const float crossTop = (float) imageHeight * 0.25f;
            const float crossBottom = (float) imageHeight * 0.75f;
            g.drawLine (left + margin, crossTop, left + (float) columnWidth - margin, crossBottom, 2.0f);
            g.drawLine (left + margin, crossBottom, left + (float) columnWidth - margin, crossTop, 2.0f);
            continue;
        }

        // draw deviation
        float maxPosition = (float) ((measurements[i].pitchOffset + measurements[i].pitchDeviation - min) * vertScaling);
        float minPosition = (float) ((measurements[i].pitchOffset - measurements[i].pitchDeviation - min) * vertScaling);

        g.setColour(Colours::springgreen.withAlpha(0.4f));
        g.fillRect(left, yFlip(maxPosition), (float) columnWidth, maxPosition - minPosition);

        // draw average value
        float pointPosition = (float) ((measurements[i].pitchOffset - min) * vertScaling);
        g.setColour(Colours::green);
        g.drawLine(left, yFlip(pointPosition), left + (float) columnWidth, yFlip(pointPosition));
    }
    
    // draw the X-Axis label
    g.setColour(Colours::black);
    g.drawText("MIDI note", 0, imageHeight, juce::roundToInt(sidebarWidth) - 10, bottomBarHeight, Justification::centredRight);
    
    // draw the corresponding note values to the X axis
    const int numPitchTextIntervals = 5;
    const int pitchTextIntervals[numPitchTextIntervals] = {1, 2, 5, 10, 20};
    int currentPitchTextIntervalIndex = 0;
    while (GlyphArrangement::getStringWidth(g.getCurrentFont(), "123.") > pitchTextIntervals[currentPitchTextIntervalIndex] * columnWidth)
    {
        currentPitchTextIntervalIndex++;
        if (currentPitchTextIntervalIndex >= numPitchTextIntervals)
            break;
    }
    int pitchTextInterval = pitchTextIntervals[currentPitchTextIntervalIndex];
    int startLine = 0;
    int endLine = measurements.size() - 1;
    while (measurements[startLine].midiPitch % pitchTextInterval != 0)
    {
        startLine++;
        if (startLine >= measurements.size())
            return;
    }
    while (measurements[endLine].midiPitch % pitchTextInterval != 0)
    {
        endLine--;
        if (endLine < 0 || endLine < startLine)
            return;
    }
    
    for (int i = startLine; i <= endLine; i += pitchTextInterval)
    {
        g.setColour(Colours::black);
        float textWidth = GlyphArrangement::getStringWidth(g.getCurrentFont(), String(measurements[i].midiPitch));
        float left = sidebarWidth + i * float(columnWidth);
        float x = left + float(columnWidth)/2.0f - textWidth/2.0f;
        float y = imageHeight;
        g.drawText(String(measurements[i].midiPitch), juce::Rectangle<float>(x, y, textWidth, bottomBarHeight), Justification::centred);
        
        // the line for the reference pitch will be drawn later
        if (measurements[i].midiPitch == tuner->getReferencePitch())
            continue;
        
        // also draw dim vertical lines for the larger devisions or if the columns get very narrow
        if (pitchTextInterval >= 2)
        {
            g.setColour(Colours::blue.withAlpha(0.05f));
            g.fillRect(Rectangle<float>(left, 0, float(columnWidth), imageHeight));
        }
        if (pitchTextInterval == 1 && columnWidth < GlyphArrangement::getStringWidth(g.getCurrentFont(), "123."))
        {
            if (i % 2 == 0)
            {
                g.setColour(Colours::blue.withAlpha(0.05f));
                g.fillRect(Rectangle<float>(left, 0, float(columnWidth), imageHeight));
            }
        }
    }
    
    // draw a blue indicator for the reference pitch (if included in the measurements)
    if (measurements[0].midiPitch < tuner->getReferencePitch()
        && measurements.getLast().midiPitch > tuner->getReferencePitch())
    {
        for (int i = 0; i < measurements.size(); i++)
        {
            if (measurements[i].midiPitch == tuner->getReferencePitch())
            {
                float left = sidebarWidth + i * float(columnWidth);
                g.setColour(Colours::blue.withAlpha(0.15f));
                g.fillRect(Rectangle<float>(left, 0, float(columnWidth), imageHeight));
            }
        }
    }
}

void Visualizer::paint(Graphics& g)
{
    paint(g, getWidth(), getHeight());
}

float Visualizer::yFlip(float y)
{
    return heightForFlipping - y;
}

void Visualizer::upsertMeasurement (const VCOTuner::measurement_t& m)
{
    for (int i = 0; i < measurements.size(); i++)
    {
        if (measurements[i].midiPitch == m.midiPitch)
        {
            measurements.set (i, m);
            return;
        }
    }

    measurements.add (m);
}

void Visualizer::newMeasurementReady(const VCOTuner::measurement_t& m)
{
    // a later cycle may have re-measured a note that previously failed;
    // a fresh successful reading means it is no longer failed.
    failedPitches.removeFirstMatchingValue (m.midiPitch);

    upsertMeasurement (m);

    repaint();
}

void Visualizer::measurementFailed (int midiPitch, vcotuner::MeasurementError)
{
    failedPitches.addIfNotAlreadyThere (midiPitch);

    // give the pitch a column even if it has never produced a measurement,
    // so a first-attempt failure is visible instead of just closing the gap.
    // All numeric fields stay at zero; numMeasurements == 0 marks this as a
    // placeholder. A later successful reading overwrites it via upsertMeasurement.
    VCOTuner::measurement_t placeholder {};
    placeholder.midiPitch = midiPitch;
    upsertMeasurement (placeholder);

    repaint();
}
