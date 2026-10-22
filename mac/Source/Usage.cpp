#include "Usage.h"
#include <sys/resource.h>
#if JUCE_MAC
 #include <mach/mach.h>
#endif

double AppUsage::cpuPercent()
{
    rusage u {};
    getrusage (RUSAGE_SELF, &u);
    const double cpu = (double) u.ru_utime.tv_sec + u.ru_utime.tv_usec / 1e6
                     + (double) u.ru_stime.tv_sec + u.ru_stime.tv_usec / 1e6;
    const double wall = juce::Time::getMillisecondCounterHiRes() / 1000.0;
    double pct = 0.0;
    if (lastCpuSeconds >= 0.0 && wall > lastWall)
        pct = 100.0 * (cpu - lastCpuSeconds) / (wall - lastWall) / juce::jmax (1, juce::SystemStats::getNumCpus());
    lastCpuSeconds = cpu;
    lastWall = wall;
    return juce::jlimit (0.0, 100.0, pct);
}

juce::int64 AppUsage::memoryBytes()
{
   #if JUCE_MAC
    task_vm_info_data_t info {};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info (mach_task_self(), TASK_VM_INFO, (task_info_t) &info, &count) == KERN_SUCCESS)
        return (juce::int64) info.phys_footprint;
    return 0;
   #else
    // Resident memory from /proc (Linux, used for testing).
    auto fields = juce::StringArray::fromTokens (juce::File ("/proc/self/statm").loadFileAsString(), true);
    return fields.size() > 1 ? fields[1].getLargeIntValue() * 4096 : 0;
   #endif
}
