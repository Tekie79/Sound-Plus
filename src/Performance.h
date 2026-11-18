#pragma once
#include <juce_core/juce_core.h>
#include <atomic>
#include <sys/resource.h>
namespace sp
{
inline double monotonicMs()
{
    return juce::Time::getMillisecondCounterHiRes();
}
struct Performance
{
    std::atomic<int64_t> fileStats{0}, directories{0}, entries{0}, discovered{0}, commits{0}, scans{0},
        headers{0}, queries{0}, cancelled{0};
    std::atomic<int64_t> directoryEnumerations{0}, peakBatches{0};
    std::atomic<double> firstEligibleMs{0}, firstCommitMs{0}, discoveryFinishedMs{0};
    juce::String delayedRoot;
    int delayMs = 0;
    int64_t expectedFiles = 0;
    std::atomic<bool> delayed{false};
    juce::var json() const
    {
        auto *o = new juce::DynamicObject();
#define SP_COUNTER(n) o->setProperty(#n, (juce::int64)n.load())
        SP_COUNTER(fileStats);
        SP_COUNTER(directories);
        SP_COUNTER(entries);
        SP_COUNTER(discovered);
        SP_COUNTER(commits);
        SP_COUNTER(scans);
        SP_COUNTER(headers);
        SP_COUNTER(queries);
        SP_COUNTER(cancelled);
        SP_COUNTER(directoryEnumerations);
        SP_COUNTER(peakBatches);
#undef SP_COUNTER
        o->setProperty("firstEligibleMs", firstEligibleMs.load());
        o->setProperty("firstCommitMs", firstCommitMs.load());
        o->setProperty("discoveryFinishedMs", discoveryFinishedMs.load());
        struct rusage usage{};
        getrusage(RUSAGE_SELF, &usage);
        o->setProperty("peakResidentBytes", (juce::int64)usage.ru_maxrss);
        return juce::var(o);
    }
};
inline Performance performance;
} // namespace sp
