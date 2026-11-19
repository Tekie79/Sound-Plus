#pragma once
#include "DesignModel.h"
namespace sp
{
inline constexpr auto poolChoicePolicy = "pool-fnv1a64-smix-v1";
SourcePoolEntry poolEntry(const DesignLayer &, const ChannelLayout &bus);
SourcePoolEntry inspectPoolEntry(const Asset &, int64_t first, int64_t last, bool external);
void verifyPoolEntry(SourcePoolEntry &, bool relocation = false, const std::function<bool()> &cancelled = {});
void reviewPoolEntry(SourcePoolEntry &, const LayerRouting &, const ChannelLayout &bus);
void activatePoolEntry(DesignLayer &, const juce::String &entry, const ChannelLayout &bus);
void syncPoolSelection(DesignMix &);
void validatePool(const DesignLayer &, const ChannelLayout &);
juce::String choosePoolEntry(const SourcePool &, uint64_t seed, const juce::String &layer, uint64_t ordinal);
void resolvePoolChoices(DesignMix &, uint64_t seed, uint64_t ordinal, double designRate);
juce::var sourcePoolToJson(const SourcePool &);
SourcePool sourcePoolFromJson(const juce::var &);
juce::var poolEntryToJson(const SourcePoolEntry &);
SourcePoolEntry poolEntryFromJson(const juce::var &);
bool hasSourcePools(const DesignDocument &);
bool pooledSourcesDiffer(const DesignMix &, const DesignMix &);
DesignDocument audioSourceProjection(const DesignDocument &);
void freezePoolOrigins(PerformTake &, DesignMix &);
void validatePoolOrigins(const PerformTake &);
juce::var poolOriginsForSource(const PerformTake &, const juce::String &source);
} // namespace sp
