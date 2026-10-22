#pragma once
#include <juce_core/juce_core.h>

// How much of the computer the app is using: CPU across all cores and the memory
// the app holds (the same "Memory" figure Activity Monitor shows on a Mac).
class AppUsage
{
public:
    // Share of the whole machine's CPU used since the last call, 0-100.
    double cpuPercent();
    static juce::int64 memoryBytes();

private:
    double lastCpuSeconds = -1.0, lastWall = 0.0;
};
