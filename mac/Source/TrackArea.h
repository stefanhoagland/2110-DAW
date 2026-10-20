#pragma once
#include "TrackRow.h"

// The tracks area under the toolbar, like the web Studio view: track headers on
// the left and a timeline with a ruler on the right showing the current take.
class TrackArea : public juce::Component
{
public:
    explicit TrackArea (juce::OwnedArray<TrackRow>& r) : rows (r) {}

    void layout (int width);           // sizes itself and positions the rows
    void setTake (int take, double seconds, bool recording);
    void zoom (double factor);

    void paint (juce::Graphics&) override;

private:
    double secondsShown() const;

    juce::OwnedArray<TrackRow>& rows;
    int take = 0;
    double seconds = 0.0;
    bool recording = false;
    double pxPerSec = 40.0; // same default zoom as the web version
};
