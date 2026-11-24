#pragma once
#include "WorkspaceTransfer.h"

namespace sp::sfx
{
struct TransformInput
{
    juce::String path, sha256, sourceId, sourcePath, sourceHash, scope, originalLayout;
    int64_t first = 0, last = 0, frames = 0, bytes = 0;
    double rate = 0;
    int channels = 0;
    bool downmixed = false;
    juce::var json() const;
    static TransformInput fromJson(const juce::var &);
};
struct AudioEdit
{
    TransformInput input;
    bool inpaint = false;
    int64_t maskStart = 0, maskEnd = 0, fade = 0; // Relative to the approved upload, original rate.
};
TransformInput freezeTransform(TransferRequest, bool downmix, const juce::File &privateFolder,
                               const std::function<bool()> &cancelled);
void verifyTransform(const TransformInput &);
juce::String validateEdit(const AudioEdit &);
void compositeTransform(const AudioEdit &, const juce::File &returned, const juce::File &output,
                        const std::function<bool()> &cancelled);
}
