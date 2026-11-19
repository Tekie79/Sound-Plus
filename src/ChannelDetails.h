#pragma once
#include "WaveformPreview.h"
namespace sp
{
// Snapshot identity plus shared scheduler ownership; closing releases all demands.
void showChannelDetails(juce::Component &, juce::Rectangle<int>, std::shared_ptr<WaveformPool>, Asset,
                        ChannelLayout = {}, std::vector<int> = {}, int owner = 4, int64_t first = 0,
                        int64_t last = -1, bool reverse = false);
} // namespace sp
