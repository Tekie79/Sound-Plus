#pragma once
#include <atomic>
#include <cstdint>
namespace sp
{
// One request/response slot, accessed only by the graph producer and persistent
// plugin service. The device callback consumes its existing independent PCM ring.
struct HostedRackExchange
{
    static constexpr uint32_t magic = 0x53505233;
    uint32_t protocol = magic;
    std::atomic<uint64_t> lease{0}, resetRequested{0}, resetAcknowledged{0};
    std::atomic<int64_t> heartbeatTicks{0}, lastRequestTicks{0};
    std::atomic<uint32_t> request{0}, response{0}, closing{0}, ready{0}, failed{0};
    int32_t frames = 0, channels = 0, latency = 0;
    uint32_t controls = 0;
    uint32_t bypass = 0;
    int32_t parameterCount[8]{};
    int32_t parameterIndex[8][512]{};
    float parameterValue[8][512]{};
    char reason[512]{};
    float input[16][512]{}, output[16][512]{};
};
static_assert(std::atomic<uint32_t>::is_always_lock_free);
} // namespace sp
