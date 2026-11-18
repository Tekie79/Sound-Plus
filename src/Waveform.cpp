#include "Waveform.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <cmath>
namespace sp
{
WaveformCache::WaveformCache(juce::File folder) : directory(std::move(folder)) { worker=std::thread([this] { run(); }); }
WaveformCache::~WaveformCache() { stopping = true; ++generation; wake.notify_all(); if (worker.joinable()) worker.join(); }
uint64_t WaveformCache::request(const Asset &a)
{
    std::lock_guard lock(mutex); auto id = ++generation; pending = a; wake.notify_all(); return id;
}
std::shared_ptr<const WaveformData> WaveformCache::snapshot() { std::lock_guard lock(mutex); return result; }
void WaveformCache::run()
{
    juce::AudioFormatManager formats; formats.registerBasicFormats();
    while (!stopping)
    {
        Asset a; uint64_t token;
        {
            std::unique_lock lock(mutex); wake.wait(lock,[&] { return stopping || pending.has_value(); });
            if (stopping) return; a = *pending; pending.reset(); token = generation;
        }
        auto data = std::make_shared<WaveformData>();
        data->assetId = a.id; data->revision = a.revision; data->generation = token;
        data->samples = a.samples; data->rate = a.sampleRate;
        try
        {
            FileStamp before, after; juce::File source(a.path);
            auto same = [&](const FileStamp &s) { return s.size==a.size && s.modified==a.modified && s.inode==a.inode; };
            if (!fileStamp(source,before) || !same(before)) throw std::runtime_error("Recording changed or is offline");
            directory.createDirectory();
            auto cache = directory.getChildFile(a.id+"-"+juce::String(a.revision)+"-peaks-v1.bin");
            if (cache.existsAsFile() && cache.getSize() <= 600000)
            {
                auto in = cache.createInputStream();
                if (in && in->readInt()==0x53505746 && in->readInt()==1)
                {
                    auto count = in->readInt();
                    auto samples = in->readInt64(); auto rate = in->readDouble();
                    bool valid = count>0 && count<=65536 && samples==a.samples && rate==a.sampleRate;
                    valid &= in->readInt64()==a.revision;
                    valid &= in->readInt64()==a.size; valid &= in->readInt64()==a.modified; valid &= in->readInt64()==a.inode;
                    if (valid && in->getNumBytesRemaining()==(int64_t)count*8)
                    {
                        data->peaks.resize((size_t)count);
                        for (auto &peak : data->peaks)
                        { peak = {in->readFloat(),in->readFloat()}; valid &= std::isfinite(peak.first)&&std::isfinite(peak.second)&&peak.first<=peak.second; }
                        data->cacheHit = valid;
                        if (!valid) data->peaks.clear();
                    }
                }
            }
            if (!data->cacheHit)
            {
                std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(source));
                if (!reader || reader->sampleRate<=0 || reader->lengthInSamples<=0 || reader->numChannels==0 || reader->numChannels>64)
                    throw std::runtime_error("Cannot decode waveform");
                if (reader->lengthInSamples!=a.samples || reader->sampleRate!=a.sampleRate)
                    throw std::runtime_error("Recording changed; refresh before preparing its waveform");
                auto count = (int)std::min<int64_t>(65536,std::max<int64_t>(1,a.samples/256+(a.samples%256!=0)));
                double binScale=(double)count/a.samples;
                data->peaks.assign((size_t)count,{std::numeric_limits<float>::max(),-std::numeric_limits<float>::max()});
                juce::AudioBuffer<float> block((int)reader->numChannels,65536);
                for (int64_t at = 0; at<a.samples && generation==token && !stopping; at+=block.getNumSamples())
                {
                    int n = (int)std::min<int64_t>(block.getNumSamples(),a.samples-at);
                    if (!reader->read(&block,0,n,at,true,true)) throw std::runtime_error("Waveform decoding failed");
                    for (int j = 0; j<n; ++j)
                    {
                        auto bin = (size_t)std::min<int64_t>(count-1,(int64_t)((at+j)*binScale));
                        for (int c = 0; c<block.getNumChannels(); ++c)
                        {
                            float value = block.getSample(c,j);
                            if (!std::isfinite(value)) value = 0;
                            data->peaks[bin].first = std::min(data->peaks[bin].first,value);
                            data->peaks[bin].second = std::max(data->peaks[bin].second,value);
                        }
                    }
                }
                if (generation!=token || stopping) continue;
                if (!fileStamp(source,after) || !same(after)) throw std::runtime_error("Recording changed while building waveform");
                juce::TemporaryFile temporary(cache);
                auto out = temporary.getFile().createOutputStream();
                if (!out) throw std::runtime_error("Cannot cache waveform");
                out->writeInt(0x53505746); out->writeInt(1); out->writeInt(count);
                out->writeInt64(a.samples); out->writeDouble(a.sampleRate); out->writeInt64(a.revision);
                out->writeInt64(a.size); out->writeInt64(a.modified); out->writeInt64(a.inode);
                for (auto [minimum,maximum] : data->peaks) { out->writeFloat(minimum); out->writeFloat(maximum); }
                out->flush(); out.reset();
                if (!temporary.overwriteTargetFileWithTemporary()) throw std::runtime_error("Cannot publish waveform cache");
            }
            if (!fileStamp(source,after) || !same(after)) throw std::runtime_error("Recording changed while loading waveform");
        }
        catch (const std::exception &e) { data->error=e.what(); data->peaks.clear(); }
        std::lock_guard lock(mutex);
        if (generation==token && !stopping) result=std::move(data);
    }
}
WaveformView::WaveformView()
{
    setTitle("Recording waveform. Click to seek; drag to select a range."); setWantsKeyboardFocus(true);
    addAndMakeVisible(zoom); addAndMakeVisible(all);
    zoom.onClick=[this] { zoomRange(); }; all.onClick=[this] { showWhole(); };
}
void WaveformView::setData(std::shared_ptr<const WaveformData> value)
{
    bool different = !waveform || !value || waveform->assetId!=value->assetId || waveform->revision!=value->revision;
    waveform=std::move(value);
    if (different) showWhole();
    repaint();
}
void WaveformView::setRange(int64_t start, int64_t end, bool zoomIn)
{
    first=start; last=end; if (zoomIn) zoomRange(); repaint();
}
void WaveformView::setCursor(int64_t sample) { if (cursor!=sample) { cursor=sample; repaint(); } }
void WaveformView::showWhole() { viewFirst=0; viewLast=waveform ? std::max<int64_t>(1,waveform->samples) : 1; repaint(); }
void WaveformView::zoomRange()
{
    if (!waveform || last<=first) return;
    auto margin=(int64_t)std::llround(waveform->rate*2);
    viewFirst=std::max<int64_t>(0,first-margin); viewLast=std::min(waveform->samples,last+margin);
    if (viewLast<=viewFirst) viewLast=viewFirst+1; repaint();
}
int64_t WaveformView::sampleAt(float x) const
{
    return std::clamp<int64_t>(viewFirst+(int64_t)std::llround(std::clamp(x/getWidth(),0.0f,1.0f)*(viewLast-viewFirst)),
        0,waveform ? std::max<int64_t>(0,waveform->samples-1) : 0);
}
float WaveformView::xAt(int64_t sample) const { return (float)((double)(sample-viewFirst)*getWidth()/std::max<int64_t>(1,viewLast-viewFirst)); }
void WaveformView::resized() { zoom.setBounds(getWidth()-183,2,100,23); all.setBounds(getWidth()-77,2,75,23); }
void WaveformView::paint(juce::Graphics &g)
{
    g.fillAll(juce::Colour(0xff171d25));
    auto area=getLocalBounds().toFloat(); area.removeFromTop(28); area.removeFromBottom(14);
    if (!waveform || waveform->peaks.empty())
    {
        g.setColour(juce::Colour(0xff9baabc)); g.setFont(juce::FontOptions(13.0f));
        g.drawText(waveform && waveform->error.isNotEmpty() ? waveform->error : "Select a recording to prepare its waveform",area,juce::Justification::centred);
        return;
    }
    g.setColour(juce::Colour(0x3377bfff));
    g.fillRect(juce::Rectangle<float>(xAt(first),area.getY(),xAt(last)-xAt(first),area.getHeight()).getIntersection(area));
    g.setColour(juce::Colour(0xff8cbed6));
    for (int x=0; x<getWidth(); ++x)
    {
        auto start=sampleAt((float)x),end=std::min(waveform->samples,sampleAt((float)x+1)+1);
        size_t a=(size_t)((double)start/waveform->samples*waveform->peaks.size());
        size_t b=(size_t)std::min<int64_t>((int64_t)waveform->peaks.size(),(int64_t)((double)end/waveform->samples*waveform->peaks.size())+1);
        float minimum=0,maximum=0;
        for (size_t n=a; n<b; ++n) { minimum=std::min(minimum,waveform->peaks[n].first); maximum=std::max(maximum,waveform->peaks[n].second); }
        float y=area.getCentreY(),scale=area.getHeight()*0.48f;
        g.drawVerticalLine(x,y-std::min(1.0f,maximum)*scale,y-std::max(-1.0f,minimum)*scale+1);
    }
    g.setColour(juce::Colour(0xff77bfff));
    for (auto sample : {first,last}) if (sample>=viewFirst && sample<=viewLast) g.fillRect(xAt(sample)-1,area.getY(),2.0f,area.getHeight());
    if (cursor>=viewFirst && cursor<=viewLast)
    { g.setColour(juce::Colour(0xffffc384)); g.fillRect(xAt(cursor),area.getY(),1.5f,area.getHeight()); }
    g.setColour(juce::Colour(0xff9baabc)); g.setFont(juce::FontOptions(11.0f));
    g.drawText(juce::String(viewFirst/waveform->rate,2)+" s",2,getHeight()-14,100,14,juce::Justification::left);
    g.drawText(juce::String(viewLast/waveform->rate,2)+" s",getWidth()-102,getHeight()-14,100,14,juce::Justification::right);
    g.drawText("Click to seek | Drag to set range",8,4,getWidth()-205,20,juce::Justification::left);
}
void WaveformView::mouseDown(const juce::MouseEvent &e)
{
    if (!waveform || waveform->peaks.empty()) return;
    grabKeyboardFocus(); anchor=sampleAt(e.position.x); dragging=false;
    dragMode=std::abs(e.position.x-xAt(first))<8 ? 1 : std::abs(e.position.x-xAt(last))<8 ? 2 : 3;
}
void WaveformView::mouseDrag(const juce::MouseEvent &e)
{
    if (!waveform || waveform->peaks.empty() || e.getDistanceFromDragStart()<3) return;
    dragging=true; auto sample=sampleAt(e.position.x);
    if (dragMode==1) first=std::min(sample,last-1);
    else if (dragMode==2) last=std::max(first+1,sample);
    else { first=std::min(anchor,sample); last=std::max(anchor+1,sample); }
    if (onRange) onRange(first,last); repaint();
}
void WaveformView::mouseUp(const juce::MouseEvent &e)
{
    if (!waveform || waveform->peaks.empty()) return;
    if (!dragging && onSeek) onSeek(sampleAt(e.position.x));
}
} // namespace sp
