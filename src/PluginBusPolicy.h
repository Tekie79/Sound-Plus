#pragma once
#include "ChannelLayout.h"
#include <juce_audio_processors/juce_audio_processors.h>
namespace sp
{
// Preparation only. Retain declared buses and require exact main roles with
// every auxiliary disabled; a processor may explicitly refuse that topology.
inline bool negotiatePluginMainBus(juce::AudioProcessor &p, const ChannelLayout &layout)
{
    auto buses = p.getBusesLayout();
    if (buses.inputBuses.isEmpty() || buses.outputBuses.isEmpty())
        return false;
    buses.inputBuses.set(0, juceChannelSet(layout));
    buses.outputBuses.set(0, juceChannelSet(layout));
    for (int i = 1; i < buses.inputBuses.size(); ++i)
        buses.inputBuses.set(i, juce::AudioChannelSet::disabled());
    for (int i = 1; i < buses.outputBuses.size(); ++i)
        buses.outputBuses.set(i, juce::AudioChannelSet::disabled());
    return p.checkBusesLayoutSupported(buses) && p.setBusesLayout(buses) && p.getBusesLayout() == buses &&
           p.getTotalNumInputChannels() == layout.size() && p.getTotalNumOutputChannels() == layout.size();
}
} // namespace sp
