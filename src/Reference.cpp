#include "Reference.h"
#include "ModelPack.h"
#include "AudioIPC.h"
#include "Performance.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_cryptography/juce_cryptography.h>
#include <filesystem>
#include <cmath>
namespace sp
{
std::vector<std::pair<int64_t,int64_t>> referenceWindows(int64_t first,int64_t last,double rate)
{
    if (rate<=0 || first<0 || last<=first) throw std::runtime_error("Choose a nonempty reference range");
    auto window=(int64_t)std::llround(rate*10),stride=(int64_t)std::llround(rate*5);
    auto length=last-first;
    int64_t count=length<=window ? 1 : 1+(length-window+stride-1)/stride;
    std::vector<std::pair<int64_t,int64_t>> ranges;
    for (int64_t i=0;i<std::min<int64_t>(12,count);++i)
    {
        auto start=count<=12 ? first+i*stride : first+(int64_t)std::llround((long double)(length-window)*i/11);
        ranges.emplace_back(start,std::min(last,start+window));
    }
    return ranges;
}
ReferenceEmbedding ReferenceEncoder::encode(AudioReference reference,AIProcess &worker,
    const std::function<bool()> &cancelled,const std::function<void(const AudioReference &,int,int)> &progress)
{
    auto begin=monotonicMs();
    auto check=[&] { if (cancelled()) throw QueryCancelled(); };
    check();
    if (reference.channelPolicy!=analysisChannelPolicy) throw std::runtime_error("Unsupported reference channel-analysis policy");
    auto &a=reference.source;
    juce::File file(a.path);FileStamp stamp,after;
    if (!fileStamp(file,stamp)) throw std::runtime_error("Reference is offline or missing. Reconnect its source or choose another file.");
    if (!reference.external && (stamp.size!=a.size || stamp.modified!=a.modified || stamp.inode!=a.inode))
        throw std::runtime_error("Reference changed. Refresh the Library and select its new revision.");
    juce::AudioFormatManager formats;formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (!reader || reader->lengthInSamples<=0 || reader->sampleRate<=0 || reader->sampleRate>384000 || reader->numChannels==0 || reader->numChannels>64)
        throw std::runtime_error("Reference is empty, unreadable or unsupported. Choose a supported audio file.");
    if (!reference.external && (reader->sampleRate!=a.sampleRate || reader->lengthInSamples!=a.samples))
        throw std::runtime_error("Reference header changed. Refresh the Library and choose the new revision.");
    if (reference.external)
    {
        a.name=file.getFileName();a.size=stamp.size;a.modified=stamp.modified;a.inode=stamp.inode;
        a.sampleRate=reader->sampleRate;a.samples=reader->lengthInSamples;a.channels=(int)reader->numChannels;
        a.duration=a.samples/a.sampleRate;a.online=true;a.state="ready";
        if (a.id.isEmpty()) a.id="external-"+juce::SHA256(file.getFullPathName().toRawUTF8(),file.getFullPathName().getNumBytesAsUTF8()).toHexString();
    }
    reader.reset();
    if (!fileStamp(file,after) || after.size!=stamp.size || after.modified!=stamp.modified || after.inode!=stamp.inode)
        throw std::runtime_error("Reference changed while reading its header. Choose it again.");
    if (reference.endSample<0) reference.endSample=a.samples;
    if (reference.startSample<0 || reference.endSample<=reference.startSample || reference.endSample>a.samples)
        throw std::runtime_error("Reference range is outside the recording. Set valid IN and OUT positions.");
    auto windows=referenceWindows(reference.startSample,reference.endSample,a.sampleRate);
    progress(reference,0,(int)windows.size());check();
    auto *keyObject=new juce::DynamicObject();juce::var identity(keyObject);
    keyObject->setProperty("policy",reference.channelPolicy);keyObject->setProperty("model",modelId());
    keyObject->setProperty("preprocessing",preprocessingId());keyObject->setProperty("pooling","mean-nonsilent-max12-even-v2");
    keyObject->setProperty("asset",a.id);keyObject->setProperty("path",reference.external ? a.path : juce::String());keyObject->setProperty("revision",(juce::int64)a.revision);
    keyObject->setProperty("size",(juce::int64)a.size);keyObject->setProperty("modified",(juce::int64)a.modified);keyObject->setProperty("inode",(juce::int64)a.inode);
    keyObject->setProperty("rate",a.sampleRate);keyObject->setProperty("samples",(juce::int64)a.samples);
    keyObject->setProperty("first",(juce::int64)reference.startSample);keyObject->setProperty("last",(juce::int64)reference.endSample);
    auto serialized=juce::JSON::toString(identity,true);
    auto key=juce::SHA256(serialized.toRawUTF8(),serialized.getNumBytesAsUTF8()).toHexString();
    auto target=cache.getChildFile(key+".json");
    ReferenceEmbedding result;result.reference=reference;result.windows=(int)windows.size();
    if (target.existsAsFile() && target.getSize()<20000)
    {
        auto value=juce::JSON::parse(target);
        if (value["key"].toString()==key)
            if (auto *v=value["vector"].getArray();v && v->size()==512)
            {
                double norm=0;
                for (auto &x:*v) { auto f=(float)(double)x;result.vector.push_back(f);norm+=f*f; }
                result.silentWindows=(int)value["silentWindows"];
                if (std::isfinite(norm) && std::abs(norm-1)<1e-4 && result.silentWindows>=0 && result.silentWindows<result.windows) result.cacheHit=true;
                else result.vector.clear();
            }
    }
    if (!result.cacheHit)
    {
        result.silentWindows=0;
        result.vector.assign(512,0);
        for (size_t i=0;i<windows.size();++i)
        {
            check();auto command=message("audio");auto *o=command.getDynamicObject();
            o->setProperty("interactive",true);o->setProperty("path",a.path);
            o->setProperty("startSample",(juce::int64)windows[i].first);o->setProperty("endSample",(juce::int64)windows[i].second);
            o->setProperty("sourceRate",a.sampleRate);o->setProperty("sourceSamples",(juce::int64)a.samples);
            o->setProperty("size",(juce::int64)a.size);o->setProperty("modified",(juce::int64)a.modified);o->setProperty("inode",(juce::int64)a.inode);
            auto response=worker.call(command,true);check();
            if (response["error"].toString().isNotEmpty()) throw std::runtime_error(response["error"].toString().toStdString());
            if (response["model"].toString()!=modelId() || response["preprocessing"].toString()!=preprocessingId())
                throw std::runtime_error("Reference/query embedding-space mismatch");
            if ((double)response["analysisRms"]<1e-6)
            {++result.silentWindows;progress(reference,(int)i+1,(int)windows.size());continue;}
            auto *v=response["embedding"].getArray();if (!v || v->size()!=512) throw std::runtime_error("Invalid reference embedding");
            for (int d=0;d<512;++d) result.vector[(size_t)d]+=(float)(double)(*v)[d];
            progress(reference,(int)i+1,(int)windows.size());
        }
        if (result.silentWindows==result.windows)
            throw std::runtime_error("The analysis mono mix is silent (opposing channel polarity can cancel). Choose another range or recording.");
        double norm=0;for (float v:result.vector) norm+=v*v;
        if (!std::isfinite(norm) || norm<1e-20) throw std::runtime_error("Reference contains no usable sound");
        for (float &v:result.vector) v=(float)(v/std::sqrt(norm));
    }
    check();
    if (!fileStamp(file,after) || after.size!=stamp.size || after.modified!=stamp.modified || after.inode!=stamp.inode)
        throw std::runtime_error("Reference changed during analysis. Choose it again before searching.");
    if (!result.cacheHit)
    {
        cache.createDirectory();juce::Array<juce::var> values;for (auto v:result.vector) values.add((double)v);
        auto *o=new juce::DynamicObject();juce::var data(o);o->setProperty("key",key);o->setProperty("identity",identity);o->setProperty("vector",values);o->setProperty("silentWindows",result.silentWindows);
        juce::TemporaryFile temporary(target);
        if (!temporary.getFile().replaceWithText(juce::JSON::toString(data,true)) || !temporary.overwriteTargetFileWithTemporary())
            throw std::runtime_error("Cannot save reference cache. Check free disk space and folder access.");
        auto files=cache.findChildFiles(juce::File::findFiles,false,"*.json");
        if (files.size()>512)
        {
            std::sort(files.begin(),files.end(),[](auto &a,auto &b){return a.getLastModificationTime()<b.getLastModificationTime();});
            for (int i=0;i<files.size()-512;++i) if (files[i]!=target) files[i].deleteFile();
        }
    }
    progress(reference,(int)windows.size(),(int)windows.size());
    result.encodingMs=monotonicMs()-begin;return result;
}
}
