#pragma once
#include <cstdint>
#include <juce_core/juce_core.h>
namespace sp
{
// Shared Library / future search-by-example / Designer transport boundary.
// Half-open integer samples at the ORIGINAL source rate; -1 end means EOF.
struct PlaybackRegion
{
    int64_t startSample = 0, endSample = -1, positionSample = -1;
    int64_t size = 0, modified = 0, inode = 0, revision = 0;
    juce::String sourceLayoutOverride;
    bool loop = false;
    int auditionPair = 0; // -2: layout-aware full source to monitor; -1: all-channel arithmetic mean; 0..31:
                          // stereo pairs 1/2, 3/4, ...
};
} // namespace sp
