#pragma once
#include "ChannelLayout.h"
namespace sp
{
// A host/insert must accept the ordered input/output contracts before graph adoption.
// A channel-count match alone is insufficient. Future AU/VST3 negotiation uses the
// same boundary; an adapter must be explicit when a processor changes its bus.
struct ProcessorBusContract
{
    ChannelLayout input, output;
    bool legacyStereo = false;
};
inline void negotiateStockBus(const ProcessorBusContract &bus)
{
    validateLayout(bus.input);
    validateLayout(bus.output);
    if (bus.input != bus.output || (bus.legacyStereo && bus.input != channelLayout("stereo-v1")))
        throw std::runtime_error("Insert bus mismatch: choose an explicit layout adapter");
}
} // namespace sp
