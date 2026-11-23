#pragma once
#include "AdvancedEffects.h"
namespace sp
{
class AdvancedEffectProcessor
{
  public:
    AdvancedEffectProcessor(
        const StockEffect &, const ChannelLayout &, double rate, MidiTiming,
        const std::function<bool()> &cancelled = [] { return false; });
    ~AdvancedEffectProcessor();
    void update(const StockEffect &);
    void process(juce::AudioBuffer<float> &, int count);
    int latency() const;
    uint64_t memoryBytes() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
bool advancedDSPAvailable(const juce::String &);
} // namespace sp
