#pragma once
#include "Catalog.h"

namespace sp
{
struct AcousticPrototype
{
    juce::String category;
    std::array<float, 512> vector{};
};
struct AcousticChoice
{
    juce::String category;
    float similarity;
    MatchRegion moment;
};
struct AcousticResult
{
    juce::String status;
    std::vector<AcousticChoice> choices;
};
const juce::String &acousticPolicy();
bool currentAcousticSource(const Asset &selected, const Asset &catalog);
AcousticResult rankAcoustic(const std::vector<StoredRegion> &, const std::vector<AcousticPrototype> &,
                            const std::array<float, 512> &silence, const std::function<bool()> &cancel);
AcousticResult acousticSuggestions(const juce::File &database, const Asset &, bool modelReady,
                                   const std::function<bool()> &cancel);
} // namespace sp
