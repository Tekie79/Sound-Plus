#pragma once
#include <juce_core/juce_core.h>
#include <atomic>
#include <mutex>
#include <sys/types.h>
namespace sp
{
// Two callers at most: one audio-analysis worker and one latest-query worker.
// A query has priority at the next inference boundary. No request backlog is retained here.
class AIProcess
{
  public:
    AIProcess(juce::File applicationExecutable, juce::File modelDirectory);
    ~AIProcess();
    juce::var call(juce::var command, bool queryPriority = false);
    void cancel() { quitting = true; }
    void retry();
    std::atomic<int64_t> generation{0};
    std::atomic<double> lastLoadMs{0};
  private:
    juce::var exchange(const juce::var &, int timeoutMs);
    void start();
    void terminate();
    juce::File helper, model;
    std::mutex mutex;
    std::atomic<bool> quitting{false};
    std::atomic<int> waitingQueries{0};
    int fd = -1, failures = 0;
    pid_t pid = -1;
    int64_t sequence = 0;
};
} // namespace sp
