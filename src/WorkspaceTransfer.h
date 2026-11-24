#pragma once
#include "DesignAudio.h"

namespace sp
{
enum class TransferScope
{
    original = 1, sourceRange, clip, track, trackRange, mixRange, mix, pad, padRange, allPads, take
};
enum class TransferAction { resolve = 1, drag };

// A value snapshot, bound to the source or document at request time.
struct TransferRequest
{
    juce::String id = juce::Uuid().toString();
    TransferScope scope = TransferScope::original;
    TransferAction action = TransferAction::drag;
    std::optional<Asset> source;
    std::optional<DesignDocument> design;
    int64_t first = 0, last = -1;
    juce::File catalog, durableFolder;
    juce::String copyBookmark;
    bool saveCopy = false;
    int resolveTrack = 0; // Zero selects a dedicated compatible track.
};
struct TransferArtifact
{
    juce::File file;
    juce::String sha256;
    int64_t samples = 0;
    double rate = 0;
    ChannelLayout layout;
    bool copied = false;
    FileStamp stamp;
    int64_t first = 0, last = 0;
    juce::String processing;
};

juce::String transferScopeName(TransferScope);
juce::String makeTransferFolderBookmark(const juce::File &);
TransferArtifact prepareTransfer(const TransferRequest &, const std::function<bool()> &cancelled);
void verifyTransferArtifact(const TransferArtifact &);
bool transferArtifactUnchanged(const TransferArtifact &);
juce::var transferReceipt(const TransferRequest &, const TransferArtifact &);
void writeTransferReceipt(const juce::File &directory, const juce::String &id, const juce::var &);
juce::var callResolveTransfer(const juce::File &script, juce::var request, const std::function<bool()> &cancelled);
} // namespace sp
