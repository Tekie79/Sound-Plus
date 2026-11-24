#include "WorkspaceTransfer.h"
#include "TrackModel.h"
#include "PerformanceState.h"
#include <iostream>

namespace
{
using namespace sp;
constexpr int sourceFrames = 48000, rate = 48000;
constexpr int first = 377, last = 22187;
void require(bool pass, const char *message)
{
    if (!pass)
        throw std::runtime_error(message);
}
void rejects(const std::function<void()> &attempt)
{
    try { attempt(); }
    catch (const std::exception &) { return; }
    throw std::runtime_error("Expected transfer to fail closed");
}
int16_t sample(int channel, int frame)
{
    return (int16_t)std::lround((2400 + 500 * channel) * std::sin(
        2 * juce::MathConstants<double>::pi * (187 + 223 * channel) * frame / rate));
}
void fixture(const juce::File &file, int channels, int mask)
{
    auto out = file.createOutputStream();
    const int bytes = sourceFrames * channels * 2;
    require(out && out->write("RIFF", 4), "Open independent WAV fixture");
    out->writeInt(60 + bytes);
    out->write("WAVEfmt ", 8);
    out->writeInt(40);
    out->writeShort((short)0xfffe);
    out->writeShort((short)channels);
    out->writeInt(rate);
    out->writeInt(rate * 2 * channels);
    out->writeShort((short)(2 * channels));
    out->writeShort(16);
    out->writeShort(22);
    out->writeShort(16);
    out->writeInt(mask);
    const unsigned char pcm[] = {1, 0, 0, 0, 0, 0, 16, 0, 128, 0, 0, 170, 0, 56, 155, 113};
    out->write(pcm, sizeof pcm);
    out->write("data", 4);
    out->writeInt(bytes);
    for (int n = 0; n < sourceFrames; ++n)
        for (int c = 0; c < channels; ++c)
            out->writeShort(sample(c, n));
    out->flush();
    require(out->getStatus().wasOk(), "Write independent fixture");
}
void exactExcerpt(const juce::File &file, int mask)
{
    auto stream = file.createInputStream();
    stream->skipNextBytes(12);
    bool verifiedMask = !mask, verifiedSamples = false;
    while (!stream->isExhausted())
    {
        const int tag = stream->readIntBigEndian();
        const auto size = (uint32_t)stream->readInt();
        const auto end = stream->getPosition() + size + (size & 1);
        require(end <= stream->getTotalLength(), "Valid chunk size");
        if (tag == 0x666d7420 && mask)
        {
            require(size >= 40 && (uint16_t)stream->readShort() == 0xfffe, "Extensible surround WAV");
            stream->skipNextBytes(18);
            verifiedMask = stream->readInt() == mask;
        }
        if (tag == 0x64617461)
        {
            const int channels = mask ? 6 : 2;
            require(size == (uint32_t)((last - first) * channels * 2), "Exact half-open sample count");
            for (int n = first; n < last; ++n)
                for (int c = 0; c < channels; ++c)
                    require(stream->readShort() == sample(c, n), "Exact PCM and channel order");
            verifiedSamples = true;
        }
        stream->setPosition(end);
    }
    require(verifiedMask && verifiedSamples, "Required WAV mask and PCM");
}
void compareMix(const TransferArtifact &a, int64_t frames, const std::function<float(int, int)> &expected)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> in(formats.createReaderFor(a.file));
    require(in && in->lengthInSamples == frames && in->numChannels == 2, "Transfer render clock and layout");
    juce::AudioBuffer<float> pcm(2, (int)frames);
    require(in->read(&pcm, 0, (int)frames, 0, true, true), "Read transfer render PCM");
    double error = 0;
    for (int n = 0; n < frames; ++n)
        for (int c = 0; c < 2; ++c)
            error = std::max(error, (double)std::abs(pcm.getSample(c, n) - expected(c, n)));
    if (error > 1e-6)
    {
        std::cerr << "Max independent mix error: " << error << " for " << a.processing << "\n";
        throw std::runtime_error("Transfer mix does not match independent PCM");
    }
}
void designScopes(const juce::File &work, const juce::File &stereo)
{
    TransferRequest r;
    r.catalog = work.getChildFile("catalog/library.sqlite");
    require(r.catalog.getParentDirectory().createDirectory().wasOk(), "Create isolated catalog directory");
    Catalog catalog(r.catalog);
    r.durableFolder = work.getChildFile("renders");
    auto &d = r.design.emplace();
    d.name = "Workspace Transfer QA";
    d.outputFolder = r.durableFolder.getFullPathName();
    d.original.layout = channelLayout("stereo-v1");
    d.original.masterDb = -3;
    d.renderSettings.bits = 32;
    d.renderSettings.dither = d.renderSettings.normalize = false;
    d.renderSettings.tailSeconds = 0;
    d.renderSettings.layout = "stereo-v1";
    const auto owner = addTrack(d.original, TrackKind::audio);
    const auto group = addTrack(d.original, TrackKind::groupBus);
    findTrack(d.original, owner)->volumeDb = -6;
    findTrack(d.original, group)->volumeDb = -9;
    routeTrack(d.original, owner, group);
    for (int pad = 0; pad < 4; ++pad)
    {
        auto l = makeDesignLayer(inspectDesignSource(stereo), first, last, true);
        l.fadeIn = l.fadeOut = 0;
        l.gainDb = -3 * pad;
        initialiseRouting(l, d.original.layout);
        d.original.layers.push_back(std::move(l));
        attachClip(d, d.original.layers.back(), owner);
    }
    d.selectedLayer = d.original.layers.front().id;
    d.selectedTrack = owner;
    const auto never = [] { return false; };
    auto gain = [](double db) { return (float)std::pow(10., db / 20.); };
    auto source = [](int c, int n) { return (float)sample(c, first + n) / 32768.f; };
    auto combined = [&](int c, int n)
    {
        return source(c, n) * (gain(0) + gain(-3) + gain(-6) + gain(-9));
    };
    const auto end = mixDuration(d.original, d.sampleRate);
    auto scope = [&](TransferScope s) { r.scope = s; return prepareTransfer(r, never); };
    compareMix(scope(TransferScope::clip), last - first,
               [&](int c, int n) { return source(c, n) * gain(-6); });
    compareMix(scope(TransferScope::track), end,
               [&](int c, int n) { return n < last - first ? combined(c, n) * gain(-6) : 0.f; });
    compareMix(scope(TransferScope::mix), end,
               [&](int c, int n) { return n < last - first ? combined(c, n) * gain(-18) : 0.f; });
    r.first = 41;
    r.last = 1600;
    compareMix(scope(TransferScope::trackRange), r.last - r.first,
               [&](int c, int n) { return combined(c, n + (int)r.first) * gain(-6); });
    compareMix(scope(TransferScope::mixRange), r.last - r.first,
               [&](int c, int n) { return combined(c, n + (int)r.first) * gain(-18); });
    for (int pad = 0; pad < 4; ++pad)
    {
        if (pad > 0)
            addPerformancePad(d);
        assignBlendLayer(d.original, 0, d.original.layers[(size_t)pad].id, pad);
    }
    for (int pad = 0; pad < 4; ++pad)
    {
        d.performPad = pad;
        compareMix(scope(TransferScope::pad), end, [&](int c, int n)
                   { return n < last - first ? source(c, n) * gain(-18 - 3 * pad) : 0.f; });
    }
    compareMix(scope(TransferScope::padRange), r.last - r.first,
               [&](int c, int n) { return source(c, n + (int)r.first) * gain(-27); });
    compareMix(scope(TransferScope::allPads), end,
               [&](int c, int n) { return n < last - first ? combined(c, n) * gain(-18) : 0.f; });
    rejects([&] { scope(TransferScope::take); });
    d.performPad = 0;
    saveDesignAtomic(work.getChildFile("Workspace Transfer QA.soundplus"), d);
}
} // namespace

void runWorkspaceTransferTests(const juce::File &root)
{
    const auto work = root.getChildFile(juce::Uuid().toString());
    require(work.createDirectory().wasOk(), "Create QA directory");
    const auto copies = work.getChildFile("copies"), media = work.getChildFile("media");
    require(copies.createDirectory().wasOk() && media.createDirectory().wasOk(), "Create isolated folders");
    const auto never = [] { return false; };
    const auto bookmark = makeTransferFolderBookmark(copies);
    for (auto mask : {0x3, 0x60f, 0x3f})
    {
        TransferRequest r;
        const auto name = mask == 0x60f ? "5.1 Side" : mask == 0x3f ? "5.1 Rear" : "Stereo";
        const auto source = media.getChildFile(juce::String(name) + ".wav");
        fixture(source, mask == 0x3 ? 2 : 6, mask);
        r.source = inspectDesignSource(source);
        r.durableFolder = work.getChildFile("durable");
        const auto whole = prepareTransfer(r, never);
        require(whole.file == source && whole.samples == sourceFrames && !whole.copied, "Verified original");
        r.saveCopy = true;
        r.copyBookmark = bookmark;
        const auto copy = prepareTransfer(r, never);
        const auto duplicate = prepareTransfer(r, never);
        require(copy.file != duplicate.file && copy.file != source && copy.sha256 == whole.sha256 &&
            duplicate.sha256 == whole.sha256 && copy.copied && duplicate.copied, "Copy collision is nondestructive");
        verifyTransferArtifact(whole);
        verifyTransferArtifact(copy);
        r.scope = TransferScope::sourceRange;
        r.first = first;
        r.last = last;
        const auto crop = prepareTransfer(r, never);
        require(crop.file.isAChildOf(copies) && crop.layout == whole.layout && crop.rate == rate,
                "Copied crop retains layout and clock");
        exactExcerpt(crop.file, mask == 0x3 ? 0 : mask);
        r.saveCopy = false;
        const auto durableCrop = prepareTransfer(r, never);
        require(durableCrop.file.isAChildOf(r.durableFolder) && durableCrop.sha256 == crop.sha256,
                "Uncopied crop is finalized in the durable folder");
        auto record = transferReceipt(r, crop);
        writeTransferReceipt(work.getChildFile("receipts"), r.id, record);
        require(juce::JSON::parse(work.getChildFile("receipts").getChildFile(r.id + ".json"))["path"].toString()
                    == crop.file.getFullPathName(), "Receipt records exact delivery path");
        rejects([&] { prepareTransfer(r, [] { return true; }); });
        r.saveCopy = true;
        r.copyBookmark = "invalid";
        rejects([&] { prepareTransfer(r, never); });
        r.saveCopy = false;
        r.source->inode++;
        rejects([&] { prepareTransfer(r, never); });
    }
    designScopes(work, media.getChildFile("Stereo.wav"));
    std::cout << "Verified original identity, collision-safe copy, exact 21,810-sample ranges, channel PCM, "
                 "0x60f and 0x3f WAV masks, durable path, receipt, cancellation and stale/missing destination.\n";
    std::cout << "Fixture folder " << work.getFullPathName() << "\n";
    std::cout << "Independent PCM matches selected clip, track, track range, mix range, full mix, each pad, "
                 "pad range and all pads with Group/Master gain. Missing take rejected.\n";
}
