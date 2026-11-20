#pragma once
#include "ChannelLayout.h"
namespace sp
{
// Opaque state/settings are serialized immutable values, not StockEffect parameters.
struct HostedSlot
{
    juce::String id = juce::Uuid().toString(), settings;
    bool bypass = false;
};
struct HostedRack
{
    juce::String id = juce::Uuid().toString();
    bool bypass = false;
    std::vector<HostedSlot> slots;
};
juce::var hostedRackJson(const HostedRack &);
HostedRack hostedRackFromJson(const juce::var &);
void validateHostedRack(const HostedRack &);
class HostedRackClient
{
  public:
    HostedRackClient(const HostedRack &, const ChannelLayout &, double rate,
                     const std::function<bool()> &cancelled);
    ~HostedRackClient();
    void process(juce::AudioBuffer<float> &, int frames);
    int latency() const;
    void update(const HostedRack &);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace sp
