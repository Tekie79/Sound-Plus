#pragma once
#include "DesignModel.h"
#include <juce_audio_basics/juce_audio_basics.h>

namespace sp
{
constexpr int maxPreparedPoolEntries = 64;
constexpr int maxCornerVoices = 4;
constexpr size_t poolPcmBudget = 128 * 1024 * 1024;
bool hasSourcePerformance(const DesignDocument &);
std::vector<SourceVoiceBinding> liveSourceBindings(const DesignMix &);
juce::var sourceBindingsJson(const std::vector<SourceVoiceBinding> &);
juce::var sourceEventsJson(const std::vector<SourceVoiceEvent> &);
SourcePerformance readSourcePerformance(const juce::var &bindings, const juce::var &events);
void validateSourcePerformance(const PerformTake &);

// Operates on a single routed clip before its track inserts. Each packed PCM buffer
// contains a single fresh source-chain performance. The producer owns all voice state.
class PoolVoices
{
  public:
    struct Sound
    {
        std::shared_ptr<const SourceVoiceBinding> binding;
        juce::AudioBuffer<float> pcm;
    };
    std::vector<Sound> sounds;
    bool schedule(size_t sound, const SourceVoiceEvent &);
    void mix(juce::AudioBuffer<float> &base, int count, int64_t frame, float gain);
    uint64_t steals() const
    {
        return stolen;
    }

  private:
    struct Event
    {
        int sound = -1;
        SourceVoiceAction action{};
        int64_t frame = 0;
        int overlap = 0;
        float level = 1;
    };
    struct Playing
    {
        int sound = -1;
        int64_t first = 0, release = -1;
        int attack = 0, decay = 0;
        float level = 1, releaseGain = 1;
        bool sustained = false;
        uint64_t order = 0;
    };
    static float envelope(const Playing &, int64_t);
    void start(const Event &);
    std::array<Event, 64> queue{};
    std::array<Playing, maxCornerVoices> playing{};
    size_t read = 0, count = 0;
    int64_t baseRelease = -1;
    int baseFade = 0;
    uint64_t order = 0, stolen = 0;
};
} // namespace sp
