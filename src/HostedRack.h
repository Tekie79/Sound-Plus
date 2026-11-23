#pragma once
#include "ChannelLayout.h"
#include "MidiModel.h"
#include "HostedParameterEvents.h"
#include <map>
namespace sp
{
// Opaque state/settings are serialized immutable values, not StockEffect parameters.
struct HostedSlot
{
    juce::String id = juce::Uuid().toString(), settings;
    bool bypass = false;
    // Main-thread intent clocks; transient, never part of saved recipes.
    std::map<int, int64_t> runtimeParameterOrigins;
    int64_t runtimeRestoreOrigin = 0;
};
struct HostedRack
{
    juce::String id = juce::Uuid().toString();
    bool bypass = false;
    bool instrument = false; // Explicit dedicated MIDI instrument purpose; old effect racks omit it.
    // Transient audition connection; intentionally omitted from saved JSON.
    juce::File runtimeEndpoint;
    juce::String runtimeOwnerRack;
    int64_t runtimeControlTicks = 0;
    bool runtimeFixedState = false; // Frozen take processor; never borrowed or exposed to native edits.
    std::vector<HostedSlot> slots;
};
inline bool newerHostedIntents(const HostedSlot &before, const HostedSlot &after)
{
    if (after.runtimeRestoreOrigin > before.runtimeRestoreOrigin)
        return true;
    for (const auto &[index, origin] : after.runtimeParameterOrigins)
    {
        auto at = before.runtimeParameterOrigins.find(index);
        if (origin > (at == before.runtimeParameterOrigins.end() ? 0 : at->second))
            return true;
    }
    return false;
}
juce::var hostedRackJson(const HostedRack &);
HostedRack hostedRackFromJson(const juce::var &);
void validateHostedRack(const HostedRack &);
class HostedRackClient
{
  public:
    HostedRackClient(const HostedRack &, const ChannelLayout &, double rate,
                     const std::function<bool()> &cancelled, const juce::File &liveDirectory = {},
                     const juce::var &binding = {});
    ~HostedRackClient();
    void process(juce::AudioBuffer<float> &, int frames, const std::vector<MidiPacket> *midi = nullptr,
                 int64_t first = 0, const MidiTiming &timing = {}, bool notesAllowed = true,
                 bool panic = false, int64_t inputEnd = -1,
                 std::span<const HostedParameterEvent> automation = {});
    int latency() const;
    void update(const HostedRack &);
    void acceptCapture(const HostedRack &);
    uint64_t modelSequence() const;
    void checkHealth() const;
    juce::var liveState() const;
    void editor(const juce::String &slot, bool open);
    void capture(uint64_t sequence);
    juce::String instanceGeneration() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace sp
