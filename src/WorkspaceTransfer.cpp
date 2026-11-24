#include "WorkspaceTransfer.h"
#include "PerformanceState.h"
#include "PluginProcessing.h"
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

namespace sp
{
TransferArtifact withTransferFolder(const juce::String &, bool,
                                    const std::function<TransferArtifact(const juce::File &)> &);
namespace
{
void require(bool pass, const char *message)
{
    if (!pass)
        throw std::runtime_error(message);
}
void checkCancelled(const std::function<bool()> &cancelled)
{
    if (cancelled())
        throw QueryCancelled();
}
void verifyStamp(const Asset &a)
{
    FileStamp stamp;
    require(a.online && a.deliverable() && fileStamp(juce::File(a.path), stamp), "Source is offline or unresolved");
    require(stamp.size == a.size && stamp.modified == a.modified && stamp.inode == a.inode,
            "Source changed since selection. Refresh the Library and try again.");
}
void syncFile(const juce::File &file)
{
    const int fd = ::open(file.getFullPathName().toRawUTF8(), O_RDONLY | O_NOFOLLOW);
    const bool ok = fd >= 0 && ::fsync(fd) == 0;
    if (fd >= 0)
        ::close(fd);
    require(ok, "Cannot sync the transfer file. Check storage availability and permissions.");
}
juce::File publish(const juce::File &stage, const juce::File &folder, const juce::String &name,
                   const juce::String &extension, const juce::String &id)
{
    syncFile(stage);
    auto base = juce::File::createLegalFileName(name).substring(0, 100).trim();
    if (base.isEmpty())
        base = "SoundPlus";
    for (int i = 0; i < 3; ++i)
    {
        auto suffix = i == 0 ? juce::String() : " - " + (i == 1 ? id : juce::Uuid().toString());
        auto target = folder.getChildFile(base + suffix + extension);
        if (renamex_np(stage.getFullPathName().toRawUTF8(), target.getFullPathName().toRawUTF8(), RENAME_EXCL) == 0)
            return target;
        if (errno != EEXIST)
            throw std::runtime_error("Cannot publish transfer. Check folder permissions and free space.");
    }
    throw std::runtime_error("Cannot choose a unique transfer filename");
}
TransferArtifact examine(const juce::File &file)
{
    TransferArtifact result;
    result.file = file;
    require(fileStamp(file, result.stamp), "Transfer file is offline or missing");
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    require(reader && reader->lengthInSamples > 0 && reader->sampleRate > 0 &&
            reader->numChannels > 0 && reader->numChannels <= 64, "Cannot verify transfer audio");
    result.sha256 = processingHash(file);
    result.samples = result.last = reader->lengthInSamples;
    result.rate = reader->sampleRate;
    result.layout = detectedLayout(*reader);
    require(transferArtifactUnchanged(result), "Transfer file changed during verification");
    return result;
}
void copyBytes(const juce::File &source, const juce::File &destination, const std::function<bool()> &cancelled)
{
    auto in = source.createInputStream();
    auto out = destination.createOutputStream();
    require(in && out, "Cannot open the copy folder or source");
    std::array<char, 65536> block;
    for (;;)
    {
        checkCancelled(cancelled);
        const auto n = in->read(block.data(), (int)block.size());
        if (n == 0)
            break;
        require(n > 0 && out->write(block.data(), (size_t)n), "Copy failed. Check free space and permissions.");
    }
    out->flush();
    require(out->getStatus().wasOk(), "Cannot flush the transfer copy");
}
void excerpt(const Asset &asset, int64_t first, int64_t last, const juce::File &file,
             const std::function<bool()> &cancelled)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(juce::File(asset.path)));
    require(reader && reader->lengthInSamples == asset.samples && reader->sampleRate == asset.sampleRate &&
                (int)reader->numChannels == asset.channels, "Source audio header changed");
    auto layout = detectedLayout(*reader);
    auto requiredChannels = (int)reader->numChannels;
    require(requiredChannels <= maxBusChannels && requiredChannels > 0, "Range transfer supports 1 to 16 channels");
    const int bits = reader->usesFloatingPointData ? 32 : std::clamp((int)reader->bitsPerSample, 16, 24);
    const int waveBits = bits == 32 ? 32 : bits > 16 ? 24 : 16;
    require((long double)(last - first) * requiredChannels * (waveBits / 8) < 4293918720.L,
            "The WAV excerpt exceeds the RIFF size limit");
    juce::WavAudioFormat wav;
    auto out = file.createOutputStream();
    require(out != nullptr, "Cannot stage the WAV excerpt");
    auto roles = juceChannelSet(layout);
    juce::StringPairArray metadata;
    metadata.set(juce::WavAudioFormat::bwavDescription,
                 "SoundPlus layout:" + layout.id + "; order:" + layout.roles.joinIntoString(","));
    std::unique_ptr<juce::AudioFormatWriter> writer(
        wav.createWriterFor(out.release(), reader->sampleRate, roles, waveBits, metadata, 0));
    require(writer != nullptr, "Cannot write this WAV channel layout");
    // Keep the original source clock and physical interleave. WAV metadata declares the verified roles.
    for (auto at = first; at < last;)
    {
        checkCancelled(cancelled);
        const auto n = (int)std::min<int64_t>(16384, last - at);
        require(writer->writeFromAudioReader(*reader, at, n), "Cannot write the WAV excerpt");
        at += n;
    }
}

TransferArtifact renderScope(const TransferRequest &r, const std::function<bool()> &cancelled)
{
    auto design = *r.design;
    resolveDesignSources(design, r.catalog);
    const bool frozen = design.activeTake.isNotEmpty();
    require(!frozen || r.scope == TransferScope::take || r.scope == TransferScope::allPads,
            "Choose Live Design for this scope");
    require(r.scope != TransferScope::take || frozen, "Choose a complete recorded take");
    if (r.scope == TransferScope::pad || r.scope == TransferScope::padRange)
    {
        if (hasMultiPad(design.activeMix()))
            design = padRenderDocument(design, design.performPad);
        else
        {
            require(design.performPad == 0, "Selected pad no longer exists");
            design = resolvePerformance(design);
            for (auto &track : design.activeMix().tracks)
                if (track.kind == TrackKind::midiInstrument)
                    track.mute = true;
        }
    }
    else if (!frozen && !hasMultiPad(design.activeMix()))
        design = resolvePerformance(design);
    auto &s = design.renderSettings;
    s.first = 0;
    s.last = -1;
    s.splitMono = false;
    juce::String stem;
    if (r.scope == TransferScope::track || r.scope == TransferScope::trackRange)
        stem = design.selectedTrack;
    if (r.scope == TransferScope::clip)
    {
        const auto &layers = design.activeMix().layers;
        const auto clip = std::find_if(layers.begin(), layers.end(), [&](const auto &layer)
                                       { return layer.id == design.selectedLayer; });
        require(clip != layers.end(), "Select an audio clip");
        const auto id = clip->id;
        stem = clip->trackId;
        s.first = clip->placement;
        s.last = s.first + layerDuration(*clip, design.sampleRate);
        s.tailSeconds = 0;
        for (auto &layer : design.activeMix().layers)
        {
            layer.mute = layer.id != id;
            layer.solo = false;
        }
        if (!design.activeMix().trackSchema)
        {
            design.activeMix().inserts = {};
            design.activeMix().masterAutomation = {};
            design.activeMix().masterDb = 0;
        }
    }
    if (r.scope == TransferScope::mixRange || r.scope == TransferScope::trackRange || r.scope == TransferScope::padRange)
    {
        require(r.first >= 0 && r.last > r.first && r.last <= mixDuration(design.activeMix(), design.sampleRate),
                "Choose a nonempty project range within the design");
        s.first = r.first;
        s.last = r.last;
        s.tailSeconds = 0;
    }
    if (r.scope == TransferScope::track || r.scope == TransferScope::trackRange)
        require(stem.isNotEmpty(), "Select an audio, instrument or Group track");
    if (r.scope == TransferScope::clip && design.activeMix().trackSchema)
        require(stem.isNotEmpty(), "The selected clip has no audio owner");
    if (!stem.isEmpty())
    {
        auto results = renderMixerStems(design, r.durableFolder, {stem}, cancelled, [](double) {});
        require(results.size() == 1, "Transfer needs one finalized stem");
        auto artifact = examine(juce::File(results.front().path));
        require(artifact.sha256 == results.front().sha256, "Stem checksum mismatch");
        artifact.first = s.first;
        artifact.last = s.last < 0 ? mixDuration(design.activeMix(), design.sampleRate) : s.last;
        artifact.processing = r.scope == TransferScope::clip ? "isolated-clip-through-owner" : "pdc-track-output-pre-downstream";
        return artifact;
    }
    auto output = renderDesign(design, r.durableFolder, s, cancelled, [](double) {});
    auto artifact = examine(juce::File(output.path));
    require(artifact.sha256 == output.sha256, "Render checksum mismatch");
    artifact.first = s.first;
    artifact.last = s.last >= 0 ? s.last : frozen ? selectedTake(design)->bodyEnd
                                                   : mixDuration(design.activeMix(), design.sampleRate);
    artifact.processing = frozen ? "verified-frozen-take"
        : r.scope == TransferScope::pad || r.scope == TransferScope::padRange ? "isolated-pad-through-master"
        : r.scope == TransferScope::clip ? "isolated-legacy-clip" : "resolved-mix-through-master";
    return artifact;
}
} // namespace

juce::String transferScopeName(TransferScope scope)
{
    switch (scope)
    {
    case TransferScope::original: return "Full source";
    case TransferScope::sourceRange: return "Marked range";
    case TransferScope::clip: return "Selected clip";
    case TransferScope::track: return "Entire track";
    case TransferScope::trackRange: return "Track in project range";
    case TransferScope::mixRange: return "Mix in project range";
    case TransferScope::mix: return "Entire mix";
    case TransferScope::pad: return "Selected pad";
    case TransferScope::padRange: return "Pad in project range";
    case TransferScope::allPads: return "All pads";
    case TransferScope::take: return "Complete frozen take";
    }
    throw std::runtime_error("Unknown transfer scope");
}

TransferArtifact prepareTransfer(const TransferRequest &r, const std::function<bool()> &cancelled)
{
    // The native folder resolver keeps its scope alive throughout the worker transaction.
    return withTransferFolder(r.copyBookmark, r.saveCopy, [&](const juce::File &copyFolder)
    {
        checkCancelled(cancelled);
        TransferArtifact artifact;
        if (r.source)
        {
            const auto &source = *r.source;
            verifyStamp(source);
            const auto original = juce::File(source.path);
            if (r.scope == TransferScope::original)
            {
                artifact = examine(original);
                artifact.processing = "verified-source";
            }
            else
            {
                require(r.scope == TransferScope::sourceRange && r.first >= 0 &&
                            r.last <= source.samples && r.first < r.last, "Select a valid marked source range");
                auto folder = r.saveCopy ? copyFolder : r.durableFolder;
                require(r.saveCopy || folder.createDirectory(), "Cannot create the durable transfer folder");
                juce::TemporaryFile stage(folder.getChildFile(r.id + ".wav"));
                excerpt(source, r.first, r.last, stage.getFile(), cancelled);
                artifact = examine(stage.getFile());
                require(artifact.samples == r.last - r.first && artifact.rate == source.sampleRate &&
                            artifact.layout.size() == source.channels, "Excerpt header verification failed");
                verifyStamp(source);
                checkCancelled(cancelled);
                artifact.file = publish(stage.getFile(), folder, original.getFileNameWithoutExtension() + " - Range",
                                        ".wav", r.id);
                artifact.copied = r.saveCopy;
                artifact.first = r.first;
                artifact.last = r.last;
                artifact.processing = "exact-source-excerpt";
                return artifact;
            }
            require(artifact.samples == source.samples && artifact.rate == source.sampleRate &&
                        artifact.layout.size() == source.channels, "Source audio header changed");
            verifyStamp(source);
        }
        else
        {
            require(r.design.has_value(), "Select an audio source or a design");
            artifact = renderScope(r, cancelled);
        }
        if (r.saveCopy)
        {
            juce::TemporaryFile stage(copyFolder.getChildFile(r.id + artifact.file.getFileExtension()));
            copyBytes(artifact.file, stage.getFile(), cancelled);
            require(processingHash(stage.getFile()) == artifact.sha256, "Published copy checksum mismatch");
            require(fileStamp(stage.getFile(), artifact.stamp), "Cannot verify copy identity");
            if (r.source)
                verifyStamp(*r.source);
            checkCancelled(cancelled);
            artifact.file = publish(stage.getFile(), copyFolder, artifact.file.getFileNameWithoutExtension(),
                                    artifact.file.getFileExtension(), r.id);
            artifact.copied = true;
        }
        return artifact;
    });
}

bool transferArtifactUnchanged(const TransferArtifact &artifact)
{
    FileStamp current;
    return artifact.file != juce::File() && fileStamp(artifact.file, current) &&
           current.size == artifact.stamp.size && current.inode == artifact.stamp.inode &&
           current.modified == artifact.stamp.modified;
}
void verifyTransferArtifact(const TransferArtifact &artifact)
{
    const auto actual = examine(artifact.file);
    require(actual.sha256 == artifact.sha256 && actual.samples == artifact.samples &&
            actual.rate == artifact.rate && actual.layout == artifact.layout, "The prepared file changed");
}
juce::var transferReceipt(const TransferRequest &r, const TransferArtifact &a)
{
    auto value = juce::var(new juce::DynamicObject());
    auto *record = value.getDynamicObject();
    record->setProperty("version", 1);
    record->setProperty("id", r.id);
    record->setProperty("scope", transferScopeName(r.scope));
    record->setProperty("action", r.action == TransferAction::resolve ? "Resolve" : "Drag");
    record->setProperty("path", a.file.getFullPathName());
    record->setProperty("sha256", a.sha256);
    record->setProperty("samples", (juce::int64)a.samples);
    record->setProperty("rate", a.rate);
    record->setProperty("layout", layoutJson(a.layout));
    record->setProperty("copied", a.copied);
    record->setProperty("first", (juce::int64)a.first);
    record->setProperty("last", (juce::int64)a.last);
    record->setProperty("processing", a.processing);
    record->setProperty("date", juce::Time::getCurrentTime().toISO8601(true));
    if (r.source)
    {
        record->setProperty("source", r.source->id);
        record->setProperty("revision", (juce::int64)r.source->revision);
    }
    if (r.design)
    {
        const auto &d = *r.design;
        record->setProperty("document", d.id);
        record->setProperty("revision", (juce::int64)d.revision);
        record->setProperty("variation", d.activeVariation);
        record->setProperty("take", d.activeTake);
        record->setProperty("clip", d.selectedLayer);
        record->setProperty("track", d.selectedTrack);
        record->setProperty("pad", d.performPad);
        record->setProperty("projectRate", d.sampleRate);
        if (auto *take = selectedTake(d))
        {
            record->setProperty("recipeSha256", take->recipeHash);
            record->setProperty("eventsSha256", take->eventHash);
        }
    }
    return value;
}
void writeTransferReceipt(const juce::File &folder, const juce::String &id, const juce::var &value)
{
    require(folder.createDirectory(), "Cannot create the transfer receipt folder");
    juce::TemporaryFile temp(folder.getChildFile(id + ".json"));
    require(temp.getFile().replaceWithText(juce::JSON::toString(value)), "Cannot write the transfer receipt");
    syncFile(temp.getFile());
    require(temp.overwriteTargetFileWithTemporary(), "Cannot finalize the transfer receipt");
}
} // namespace sp

namespace sp
{
juce::var callResolveTransfer(const juce::File &script, juce::var request,
                              const std::function<bool()> &cancelled)
{
    const juce::File python("/Applications/DaVinci Resolve/DaVinci Resolve.app/Contents/Applications/ResolvePython");
    if (!python.existsAsFile() || !script.existsAsFile())
        throw std::runtime_error("Resolve Studio 21.1 or later with local scripting is required");
    juce::TemporaryFile input(juce::String(".json")), output(juce::String(".json"));
    if (!input.getFile().replaceWithText(juce::JSON::toString(request, true)))
        throw std::runtime_error("Cannot stage the local Resolve request");
    const auto mutation = request["operation"].toString() == "place";
    juce::ChildProcess bridge;
    juce::StringArray args{python.getFullPathName(), script.getFullPathName(), input.getFile().getFullPathName(),
                           output.getFile().getFullPathName()};
    if (!bridge.start(args, 0))
        throw std::runtime_error("Cannot start Resolve Studio's local scripting process");
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + 30000;
    while (!bridge.waitForProcessToFinish(50))
    {
        const bool timeout = juce::Time::getMillisecondCounterHiRes() >= deadline;
        if ((!mutation && cancelled()) || timeout)
        {
            bridge.kill();
            if (!timeout)
                throw QueryCancelled();
            throw std::runtime_error(mutation ? "Resolve placement could not be verified. Inspect the timeline before retrying."
                                              : "Resolve did not respond to the local connection check.");
        }
    }
    auto result = juce::JSON::parse(output.getFile());
    if (!result.isObject())
        throw std::runtime_error(mutation ? "Resolve returned no placement receipt. Inspect the timeline before retrying."
                                          : "Resolve local scripting returned no response.");
    return result;
}
} // namespace sp
