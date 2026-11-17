#pragma once
#include <juce_core/juce_core.h>
#include <atomic>
#include <memory>
#include <vector>
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
class SourceWatch
{
  public:
    SourceWatch(const std::vector<juce::String> &paths, std::atomic<bool> &dirty,
                const juce::String &excludedDirectory = {});
    ~SourceWatch();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace sp
