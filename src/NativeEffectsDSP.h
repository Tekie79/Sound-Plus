#pragma once
#include "NativeEffects.h"
namespace sp
{
class AdvancedEffectProcessor;
// Prepared/used on the graph producer, never the device callback. Exact bus
// roles are retained. Latency is invariant for this prepared instance.
class NativeEffectProcessor
{
  public:
    NativeEffectProcessor(
        const StockEffect &, const ChannelLayout &, double rate, MidiTiming timing = {},
        const std::function<bool()> &cancelled = [] { return false; });
    ~NativeEffectProcessor();
    void update(const StockEffect &);
    void process(juce::AudioBuffer<float> &, int count);
    int latency() const;
    double gainReductionDb() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    std::unique_ptr<AdvancedEffectProcessor> advanced;
};
bool nativeDSPAvailable(const juce::String &type);
} // namespace sp
