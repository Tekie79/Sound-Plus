#pragma once
#include "Catalog.h"
#include <functional>
#include <optional>

namespace sp
{
using SampleSpan = juce::Range<int64_t>;
struct EventWindow
{
    Asset source;
    SampleSpan range;
    int pair = -1; // Negative means all physical channels. Otherwise use this zero-based stereo pair.
};
struct EventSuggestions
{
    SampleSpan coarse;
    std::vector<SampleSpan> events;
    juce::String reason;
    bool cacheHit = false;
};
// Call on the analysis worker. Outputs refer to original frames, OUT exclusive.
EventSuggestions refineEvents(const EventWindow &, const std::optional<EventWindow> &reference,
                             const juce::File &cache, const std::function<bool()> &cancelled);
}
