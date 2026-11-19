#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include <array>
namespace sp
{
constexpr int maxBusChannels = 16;
struct ChannelLayout
{
    juce::String id;
    juce::StringArray roles;
    int size() const
    {
        return roles.size();
    }
    bool operator==(const ChannelLayout &) const = default;
};
ChannelLayout channelLayout(const juce::String &id);
std::vector<ChannelLayout> namedLayouts();
ChannelLayout discreteLayout(int count, bool unknown = false);
ChannelLayout detectedLayout(const juce::AudioFormatReader &);
juce::String detectedLayoutConfidence(const juce::AudioFormatReader &);
juce::String layoutName(const ChannelLayout &);
juce::String layoutBadge(const ChannelLayout &);
juce::var layoutJson(const ChannelLayout &);
ChannelLayout layoutFromJson(const juce::var &);
void validateLayout(const ChannelLayout &, int maximum = 16);
juce::AudioChannelSet juceChannelSet(const ChannelLayout &);
std::vector<int> wavOrder(const ChannelLayout &);
using ChannelMatrix = std::array<std::array<float, 16>, 16>; // output,input
ChannelMatrix conversionMatrix(const ChannelLayout &from, const ChannelLayout &to);
std::array<float, 16> spatialGains(const ChannelLayout &, double degrees, double divergence = 0,
                                   int routingVersion = 2);
bool horizontalLayout(const ChannelLayout &);
struct ChannelRoute
{
    int input = 0, output = 0;
    double gainDb = 0;
    bool invert = false, mute = false;
};
struct LayerRouting
{
    ChannelLayout interpretation; // Explicit override, separate from detected source metadata.
    std::vector<int> inputs;      // Physical source indices in interpreted order, <=16 of1..64.
    std::vector<ChannelRoute> routes;
    bool spatial = false, needsSelection = false;
    double azimuth = 0, width = 60, divergence = 0, lfeDb = -90, lfeCutoff = 120;
};
juce::var routingJson(const LayerRouting &);
LayerRouting routingFromJson(const juce::var &);
void validateRouting(const LayerRouting &, int sourceCount, const ChannelLayout &bus);
ChannelMatrix routingMatrix(const LayerRouting &, const ChannelLayout &bus, double pan);
} // namespace sp
