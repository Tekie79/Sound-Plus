#include "AudioTransform.h"
#include <juce_cryptography/juce_cryptography.h>
#include <cmath>
#include <cstdio>
namespace sp::sfx
{
namespace
{
void require(bool ok, const char *why) { if (!ok) throw std::runtime_error(why); }
juce::String hashFile(const juce::File &file)
{
    auto in = file.createInputStream();
    require(in && in->getTotalLength() > 0, "Audio file unavailable");
    return juce::SHA256(*in).toHexString();
}
std::unique_ptr<juce::AudioFormatReader> read(const juce::File &file)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(file));
    require(reader && reader->lengthInSamples > 0 && reader->sampleRate >= 8000 && reader->sampleRate <= 192000 &&
            reader->numChannels > 0 && reader->numChannels <= 16, "Unsupported input audio");
    return reader;
}
std::unique_ptr<juce::AudioFormatWriter> write(const juce::File &f, double rate, int channels)
{
    require(!f.exists(), "Output already exists");
    auto stream = f.createOutputStream();
    require(stream != nullptr, "Cannot create transform file");
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> out(wav.createWriterFor(stream.get(), rate, channels, 32, {}, 0));
    require(out != nullptr, "Cannot write transform WAV");
    stream.release();
    return out;
}
void checkCancel(const std::function<bool()> &cancel) { if (cancel()) throw QueryCancelled(); }
void finite(const juce::AudioBuffer<float> &b, int n)
{
    for (int c = 0; c < b.getNumChannels(); ++c)
        for (int s = 0; s < n; ++s) require(std::isfinite(b.getSample(c,s)), "Invalid audio sample");
}
}
juce::var TransformInput::json() const
{
    juce::var v(new juce::DynamicObject);
    auto &o = *v.getDynamicObject();
    o.setProperty("path", path); o.setProperty("sha256", sha256); o.setProperty("sourceId",sourceId);
    o.setProperty("sourcePath",sourcePath); o.setProperty("sourceHash",sourceHash); o.setProperty("scope",scope);
    o.setProperty("originalLayout",originalLayout); o.setProperty("downmixed",downmixed);
    o.setProperty("first",(juce::int64)first); o.setProperty("last",(juce::int64)last);
    o.setProperty("frames",(juce::int64)frames); o.setProperty("bytes",(juce::int64)bytes);
    o.setProperty("rate",rate); o.setProperty("channels",channels);
    return v;
}
TransformInput TransformInput::fromJson(const juce::var &v)
{
    TransformInput i;
    i.path=v["path"]; i.sha256=v["sha256"]; i.sourceId=v["sourceId"]; i.sourcePath=v["sourcePath"];
    i.sourceHash=v["sourceHash"]; i.scope=v["scope"]; i.originalLayout=v["originalLayout"]; i.downmixed=v["downmixed"];
    i.first=(juce::int64)v["first"]; i.last=(juce::int64)v["last"]; i.frames=(juce::int64)v["frames"];
    i.bytes=(juce::int64)v["bytes"]; i.rate=v["rate"]; i.channels=v["channels"];
    return i;
}
TransformInput freezeTransform(TransferRequest r, bool downmix, const juce::File &folder,
                               const std::function<bool()> &cancel)
{
    require(folder.createDirectory().wasOk(), "Cannot create private upload folder");
    r.durableFolder = folder;
    r.saveCopy = false;
    checkCancel(cancel);
    const auto scope = r.scope;
    if (r.source) r.scope = TransferScope::original;
    auto a = prepareTransfer(r, cancel);
    verifyTransferArtifact(a);
    auto reader = read(a.file);
    int inputChannels = (int)reader->numChannels;
    require(inputChannels <= 2 || downmix, "Confirm a stereo downmix for this multichannel input");
    int channels = std::min(2,inputChannels);
    int64_t first = r.source && scope == TransferScope::sourceRange ? r.first : 0;
    int64_t last = r.source && scope == TransferScope::sourceRange ? r.last : reader->lengthInSamples;
    require(first >= 0 && last > first && last <= reader->lengthInSamples, "Invalid source range");
    auto frames = last - first;
    require(frames <= reader->sampleRate * 380 &&
            frames * channels * 4 + 8192 < 100000000, "Choose a shorter range (380 seconds or 100 MB maximum)");
    // 32-bit integer PCM cannot always be represented as float without losing low bits.
    require(reader->usesFloatingPointData || reader->bitsPerSample <= 24, "Choose a 24-bit or float source for exact PCM preservation");
    auto layout = r.source && r.source->layoutOverride.size() == inputChannels ? r.source->layoutOverride : a.layout;
    ChannelMatrix matrix{};
    if (inputChannels > 2) matrix = conversionMatrix(layout, channelLayout("stereo-v1"));
    auto dest = folder.getChildFile(juce::Uuid().toString() + "-input.wav");
    juce::TemporaryFile stage(dest);
    auto output = write(stage.getFile(), reader->sampleRate, channels);
    juce::AudioBuffer<float> block(inputChannels, 4096), stereo(channels, 4096);
    for (int64_t at = first; at < last; at += 4096)
    {
        checkCancel(cancel);
        int n = (int)std::min<int64_t>(4096,last-at);
        require(reader->read(&block,0,n,at,true,true), "Input decode failed");
        finite(block,n);
        if (inputChannels > 2)
        {
            stereo.clear();
            for (int to=0;to<2;++to) for (int from=0;from<inputChannels;++from)
                stereo.addFrom(to,0,block,from,0,n,matrix[to][from]);
        }
        require(output->writeFromAudioSampleBuffer(inputChannels>2 ? stereo : block,0,n), "Upload WAV write failed");
    }
    require(output->flush(), "Upload WAV flush failed");
    output.reset();
    verifyTransferArtifact(a);
    TransformInput i;
    i.sourceId = r.source ? r.source->id + "/" + juce::String(r.source->revision) :
                 r.design->id + "/" + juce::String(r.design->revision) + "/" + r.design->selectedTrack + "/" +
                 r.design->selectedLayer + "/pad-" + juce::String(r.design->performPad) + "/" + r.design->activeTake;
    if (r.source) { i.sourcePath = r.source->path; i.sourceHash = a.sha256; }
    else i.sourceHash = a.sha256;
    i.scope = transferScopeName(scope); i.originalLayout=layout.id;
    i.first=r.source ? first : a.first; i.last=r.source ? last : a.last; i.frames=frames; i.rate=reader->sampleRate;
    i.channels=channels; i.downmixed=inputChannels>2; i.path=dest.getFullPathName();
    i.sha256=hashFile(stage.getFile()); i.bytes=stage.getFile().getSize();
    checkCancel(cancel);
    require(renamex_np(stage.getFile().getFullPathName().toRawUTF8(),dest.getFullPathName().toRawUTF8(),RENAME_EXCL) == 0, "Cannot finalize upload");
    return i;
}
void verifyTransform(const TransformInput &i)
{
    require(i.sha256.length()==64 && hashFile(juce::File(i.path))==i.sha256, "Prepared audio changed. Prepare it again.");
    if (i.sourcePath.isNotEmpty())
        require(hashFile(juce::File(i.sourcePath))==i.sourceHash, "Original source changed. Prepare it again.");
}
juce::String validateEdit(const AudioEdit &e)
{
    auto &i=e.input;
    if (i.rate < 8000 || i.rate > 192000 || i.channels < 1 || i.channels > 2 || i.bytes <= 0 || i.bytes >= 100000000 ||
        i.frames < 6*i.rate || i.frames > 380*i.rate || i.sha256.length()!=64)
        return "Choose 6 to 380 seconds of mono or stereo audio under 100 MB";
    if (e.inpaint && (e.maskStart<0 || e.maskEnd<=e.maskStart || e.maskEnd>i.frames || e.fade<0 ||
        e.fade > std::min<int64_t>((e.maskEnd-e.maskStart)/2,(int64_t)(i.rate*.1))))
        return "Choose a valid mask with a crossfade up to 100 ms";
    return {};
}
void compositeTransform(const AudioEdit &e, const juce::File &returned, const juce::File &output,
                        const std::function<bool()> &cancel)
{
    verifyTransform(e.input);
    auto a=read(juce::File(e.input.path)), b=read(returned);
    const auto frames=a->lengthInSamples;
    require(e.maskStart>=0 && e.maskEnd<=frames && e.maskEnd>e.maskStart && e.fade>=0 &&
            e.fade<=(e.maskEnd-e.maskStart)/2, "Invalid inpaint mask");
    require(a->numChannels<=2 && b->numChannels<=2 && a->sampleRate==e.input.rate && frames==e.input.frames,
            "Unexpected inpaint layout");
    const double ratio=b->sampleRate / a->sampleRate;
    require(std::abs(b->lengthInSamples - frames*ratio)<=1, "Provider changed the duration. Inpaint requires aligned audio.");
    juce::TemporaryFile stage(output);
    auto writer=write(stage.getFile(),a->sampleRate,(int)a->numChannels);
    juce::AudioBuffer<float> original(a->numChannels,4096), replacement(b->numChannels, (int)std::ceil(4096*ratio)+3);
    for (int64_t at=0;at<frames;at+=4096)
    {
        checkCancel(cancel);
        int n=(int)std::min<int64_t>(4096,frames-at);
        require(a->read(&original,0,n,at,true,true), "Cannot read original");
        int64_t base=(int64_t)std::floor(at*ratio);
        int count=(int)std::min<int64_t>(replacement.getNumSamples(),b->lengthInSamples-base);
        require(count>0 && b->read(&replacement,0,count,base,true,true), "Cannot read replacement");
        finite(replacement,count);
        for (int j=0;j<n;++j)
        {
            auto t=at+j;
            if (t<e.maskStart || t>=e.maskEnd) continue;
            double pos=t*ratio-base;
            int l=std::min(count-1,(int)pos), r=std::min(count-1,l+1);
            float blend=e.fade ? std::min(1.f,(float)std::min(t-e.maskStart,e.maskEnd-1-t)/e.fade) : 1.f;
            auto from=[&](int c){return juce::jmap((float)(pos-l),replacement.getSample(c,l),replacement.getSample(c,r));};
            for (int c=0;c<original.getNumChannels();++c)
            {
                float wet=original.getNumChannels()==1 && replacement.getNumChannels()==2 ?
                    (from(0)+from(1))*.5f : from(std::min(c,replacement.getNumChannels()-1));
                original.setSample(c,j,juce::jmap(blend,original.getSample(c,j),wet));
            }
        }
        require(writer->writeFromAudioSampleBuffer(original,0,n), "Composite write failed");
    }
    require(writer->flush(), "Composite flush failed"); writer.reset();
    checkCancel(cancel);
    require(renamex_np(stage.getFile().getFullPathName().toRawUTF8(),output.getFullPathName().toRawUTF8(),RENAME_EXCL) == 0, "Cannot finalize composite");
}
}
