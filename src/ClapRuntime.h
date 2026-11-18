#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include <memory>
#include <vector>
namespace sp
{
class ClapRuntime
{
  public:
    explicit ClapRuntime(const juce::File &modelDirectory);
    ~ClapRuntime();
    std::vector<float> text(const juce::String &);
    std::vector<float> audio(const juce::File &, int64_t start, int64_t end, double expectedRate,
                             int64_t expectedLength);
    std::vector<int64_t> tokenize(const juce::String &);
    // Testable preprocessing boundary, matching the pinned HF ClapFeatureExtractor.
    static std::vector<float> features(const std::vector<float> &mono48k);
    double lastPreprocessMs = 0, lastInferenceMs = 0;
  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
std::vector<juce::String> robertaPieces(const juce::String &);
bool localAiSupported();
} // namespace sp
