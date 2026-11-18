#pragma once
#include <juce_core/juce_core.h>
#include <atomic>
#include <functional>
namespace sp
{
// The embedded manifest is the trust root. Imported manifests never select code or URLs.
juce::var modelManifest();
juce::String modelId();
juce::String preprocessingId();
juce::File defaultModelDirectory();
bool verifyModelPack(const juce::File &, juce::String &error, const std::atomic<bool> *cancel = nullptr);
bool installModelPack(const juce::File &destination, const juce::File &offlineSource,
                      const std::atomic<bool> &cancel,
                      const std::function<void(int64_t, int64_t)> &progress, juce::String &error);
} // namespace sp
