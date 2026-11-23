#pragma once
#include "AdvancedEffects.h"
#include <juce_audio_basics/juce_audio_basics.h>
namespace sp
{
using IRCancel = std::function<bool()>;
ImpulseReference inspectImpulse(
    const juce::File &, const ChannelLayout &, const juce::String &mapping,
    const IRCancel &cancelled = [] { return false; });
ImpulseReference relinkImpulse(
    const ImpulseReference &, const juce::File &, const ChannelLayout &,
    const IRCancel &cancelled = [] { return false; });
void validateImpulseBus(const ImpulseReference &, const ChannelLayout &);
void verifyImpulseFile(const ImpulseReference &, const IRCancel &cancelled = [] { return false; });
void verifyMixImpulses(const DesignMix &, const IRCancel &cancelled = [] { return false; });
struct PreparedImpulse
{
    juce::AudioBuffer<float> pcm;
    int resampleLatency = 0;
};
PreparedImpulse prepareImpulse(
    const ImpulseReference &, const ChannelLayout &, double rate,
    const IRCancel &cancelled = [] { return false; });
} // namespace sp
