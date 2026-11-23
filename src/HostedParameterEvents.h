#pragma once
#include <cmath>
#include <cstdint>
#include <span>
#include <stdexcept>
namespace sp
{
// SPR6 transport only. Saved projects use the complete string identity, never
// this compact token as an identity authority. Built on the graph producer.
struct HostedParameterEvent
{
    uint64_t identity = 0, sequence = 0;
    int64_t projectFrame = 0, inputFrame = 0;
    int32_t offset = 0, index = 0;
    uint16_t slot = 0, policy = 0; // v1: normalized stepped sub-block boundary.
    float value = 0;
    float touchReturnValue = 0;
};
inline constexpr int hostedEventLimit = 16384;
inline void validateHostedEvents(std::span<const HostedParameterEvent> events, int frames, int slots,
                                 int64_t first, uint64_t prior)
{
    if (events.size() > hostedEventLimit || frames < 1 || frames > 512 || slots < 0 || slots > 8)
        throw std::runtime_error("Hosted automation capacity exceeded");
    int offset = -1;
    for (auto &event : events)
    {
        if (event.offset < 0 || event.offset >= frames || event.offset < offset || event.slot >= slots ||
            event.index < 0 || event.index >= 16384 || event.sequence <= prior || event.identity == 0 ||
            event.policy != 0 || event.projectFrame < 0 || event.inputFrame != first + event.offset ||
            !std::isfinite(event.value) || event.value < 0 || event.value > 1 ||
            !std::isfinite(event.touchReturnValue) || event.touchReturnValue < 0 ||
            event.touchReturnValue > 1)
            throw std::runtime_error("Invalid hosted parameter event/sequence/clock");
        offset = event.offset;
        prior = event.sequence;
    }
}
} // namespace sp
