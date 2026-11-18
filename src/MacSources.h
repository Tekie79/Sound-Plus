#pragma once
#include <juce_core/juce_core.h>
#include <atomic>
#include <memory>
#include <vector>
#include <set>
#include <mutex>
namespace sp
{
void makeAudioProcessBackground();
void activateDesktopApplication();
struct VolumeLocation
{
    juce::String identity, relative;
};
VolumeLocation volumeLocation(const juce::File &);
juce::String resolveRoot(const juce::String &path, const juce::String &identity,
                         const juce::String &relative);
class DirectoryEvents
{
  public:
    void push(const juce::String &path, bool recovery = false)
    {
        std::lock_guard lock(mutex);
        overflow |= recovery;
        if (paths.size() < 2048)
            paths.insert(path);
        else
            overflow = true;
    }
    std::pair<std::set<juce::String>, bool> take()
    {
        std::lock_guard lock(mutex);
        std::set<juce::String> out;
        out.swap(paths);
        bool full = overflow;
        overflow = false;
        return {std::move(out), full};
    }

  private:
    std::mutex mutex;
    std::set<juce::String> paths;
    bool overflow = false;
};
class SourceWatch
{
  public:
    SourceWatch(const std::vector<juce::String> &paths, std::atomic<bool> &dirty,
                const juce::String &excludedDirectory = {});
    SourceWatch(const std::vector<juce::String> &paths, DirectoryEvents &events,
                const juce::String &excluded = {});
    ~SourceWatch();
    bool started() const;

  private:
    struct Impl;
    SourceWatch(const std::vector<juce::String> &, std::atomic<bool> *, DirectoryEvents *,
                const juce::String &);
    std::unique_ptr<Impl> impl;
};
} // namespace sp
