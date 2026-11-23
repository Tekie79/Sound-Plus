#pragma once
#include "NativeEffects.h"
namespace sp
{
bool isAdvancedEffect(const juce::String &);
bool hasAdvancedEffects(const DesignMix &);
bool hasAdvancedEffects(const DesignDocument &);
void appendAdvancedDefinitions(std::vector<NativeEffectDefinition> &);
double nativeValue(const StockEffect &, const juce::String &);
void nativeValue(StockEffect &, const juce::String &, double);
juce::var impulseJson(const ImpulseReference &);
ImpulseReference impulseFromJson(const juce::var &);
void validateImpulse(const ImpulseReference &);
std::vector<StockEffect *> advancedInserts(DesignMix &);
std::vector<const StockEffect *> advancedInserts(const DesignMix &);
void validateAdvancedClock(const MidiTiming &);
juce::var advancedClockJson(const MidiTiming &);
MidiTiming advancedClockFromJson(const juce::var &);
double delayBeats(int division);
double advancedTailSeconds(const StockEffect &, const MidiTiming &);
uint64_t advancedMemoryBytes(const StockEffect &, const ChannelLayout &, double rate);
inline constexpr uint64_t maxAdvancedBytes = 96ull * 1024 * 1024;
inline constexpr double maxImpulseSeconds = 10;
} // namespace sp
