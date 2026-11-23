#pragma once
#include <juce_cryptography/juce_cryptography.h>
namespace sp
{
// Exact field framing prevents delimiter collisions. Index and display name do
// not participate. Full fields are retained and compared in saved bindings.
inline uint64_t hostedParameterToken(const juce::String &slot, const juce::var &plugin,
                                     const juce::String &nativeId)
{
    juce::MemoryOutputStream bytes;
    for (auto field : {slot, plugin["format"].toString(), plugin["id"].toString(),
                       plugin["fingerprint"].toString(), nativeId})
    {
        bytes.writeIntBigEndian((int)field.getNumBytesAsUTF8());
        bytes.write(field.toRawUTF8(), field.getNumBytesAsUTF8());
    }
    auto hash = juce::SHA256(bytes.getData(), bytes.getDataSize()).getRawData();
    uint64_t result = 0;
    for (int i = 0; i < 8; ++i)
        result = (result << 8) | ((const uint8_t *)hash.getData())[i];
    return result ? result : 1;
}
} // namespace sp
