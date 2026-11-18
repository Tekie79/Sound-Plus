#pragma once
#include <cstdint>
namespace sp
{
// Shared Library / future search-by-example / Designer transport boundary.
// Half-open integer samples at the ORIGINAL source rate; -1 end means EOF.
struct PlaybackRegion
{
    int64_t startSample = 0, endSample = -1, positionSample = -1;
    int64_t size = 0, modified = 0, inode = 0, revision = 0;
};
} // namespace sp
