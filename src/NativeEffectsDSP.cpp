#include "NativeEffectsDSP.h"
namespace sp
{
struct NativeEffectProcessor::Impl
{
};
NativeEffectProcessor::NativeEffectProcessor(const StockEffect &e, const ChannelLayout &, double)
{
    validateNativeEffect(e);
    throw std::runtime_error("Native DSP is not available for this definition yet");
}
NativeEffectProcessor::~NativeEffectProcessor() = default;
void NativeEffectProcessor::update(const StockEffect &)
{
    throw std::runtime_error("Unprepared native DSP");
}
void NativeEffectProcessor::process(juce::AudioBuffer<float> &, int)
{
    throw std::runtime_error("Unprepared native DSP");
}
int NativeEffectProcessor::latency() const
{
    return 0;
}
double NativeEffectProcessor::gainReductionDb() const
{
    return 0;
}
bool nativeDSPAvailable(const juce::String &)
{
    return false;
}
} // namespace sp
