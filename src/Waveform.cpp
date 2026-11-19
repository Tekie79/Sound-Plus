#include "Waveform.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <cmath>
namespace sp
{
struct WaveformCache::State
{
    juce::File directory;
    std::function<void()> beforeRead;
    Checkpoint checkpoint;
    std::mutex mutex;
    std::condition_variable wake, completed;
    std::optional<Asset> pending;
    std::shared_ptr<const WaveformData> result;
    std::atomic<uint64_t> generation{0};
    std::atomic<bool> stopping{false};
    bool done = false;
};
WaveformCache::WaveformCache(juce::File folder, std::function<void()> beforeRead, Checkpoint checkpoint)
    : state(std::make_shared<State>())
{
    state->directory = std::move(folder);
    state->beforeRead = std::move(beforeRead);
    state->checkpoint = std::move(checkpoint);
    worker = std::thread(
        [shared = state]
        {
            run(shared);
            {
                std::lock_guard lock(shared->mutex);
                shared->done = true;
            }
            shared->completed.notify_all();
        });
}
WaveformCache::~WaveformCache()
{
    bool finished;
    {
        std::unique_lock lock(state->mutex);
        state->stopping = true;
        ++state->generation;
        state->pending.reset();
        state->wake.notify_all();
        finished =
            state->completed.wait_for(lock, std::chrono::milliseconds(100), [&] { return state->done; });
    }
    // Peaks are rebuildable. A kernel/decoder I/O call cannot always be interrupted.
    // The worker owns only State, never the cache/view; let it finish independently
    // rather than preventing safe application shutdown after document retention.
    if (finished)
        worker.join();
    else
        worker.detach();
}
uint64_t WaveformCache::request(const Asset &asset)
{
    std::lock_guard lock(state->mutex);
    auto id = ++state->generation;
    state->pending = asset;
    state->wake.notify_all();
    return id;
}
uint64_t WaveformCache::cancel()
{
    std::lock_guard lock(state->mutex);
    state->pending.reset();
    auto id = ++state->generation;
    state->wake.notify_all();
    return id;
}
std::shared_ptr<const WaveformData> WaveformCache::snapshot()
{
    std::lock_guard lock(state->mutex);
    return state->result;
}
void WaveformCache::run(const std::shared_ptr<State> &state)
{
    auto &directory = state->directory;
    auto &mutex = state->mutex;
    auto &wake = state->wake;
    auto &pending = state->pending;
    auto &result = state->result;
    auto &generation = state->generation;
    auto &stopping = state->stopping;
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    while (!stopping)
    {
        Asset a;
        uint64_t token;
        {
            std::unique_lock lock(mutex);
            wake.wait(lock, [&] { return stopping || pending.has_value(); });
            if (stopping)
                return;
            a = *pending;
            pending.reset();
            token = generation;
        }
        auto data = std::make_shared<WaveformData>();
        data->assetId = a.id;
        data->revision = a.revision;
        data->generation = token;
        data->samples = a.samples;
        data->rate = a.sampleRate;
        try
        {
            FileStamp before, after;
            juce::File source(a.path);
            auto same = [&](const FileStamp &s)
            { return s.size == a.size && s.modified == a.modified && s.inode == a.inode; };
            if (!fileStamp(source, before))
            {
                data->status = WaveformData::Status::offline;
                throw std::runtime_error("Source offline");
            }
            if (!same(before))
            {
                data->status = WaveformData::Status::changed;
                throw std::runtime_error("Recording changed or is offline");
            }
            directory.createDirectory();
            auto cache = directory.getChildFile(a.id + "-" + juce::String(a.revision) + "-peaks-v1.bin");
            if (cache.existsAsFile() && cache.getSize() <= 600000)
            {
                auto in = cache.createInputStream();
                if (in && in->readInt() == 0x53505746 && in->readInt() == 1)
                {
                    auto count = in->readInt();
                    auto samples = in->readInt64();
                    auto rate = in->readDouble();
                    bool valid = count > 0 && count <= 65536 && samples == a.samples && rate == a.sampleRate;
                    valid &= in->readInt64() == a.revision;
                    valid &= in->readInt64() == a.size;
                    valid &= in->readInt64() == a.modified;
                    valid &= in->readInt64() == a.inode;
                    if (valid && in->getNumBytesRemaining() == (int64_t)count * 8)
                    {
                        data->peaks.resize((size_t)count);
                        for (auto &peak : data->peaks)
                        {
                            peak = {in->readFloat(), in->readFloat()};
                            valid &= std::isfinite(peak.first) && std::isfinite(peak.second) &&
                                     peak.first <= peak.second;
                        }
                        data->cacheHit = valid;
                        if (!valid)
                            data->peaks.clear();
                    }
                }
            }
            if (!data->cacheHit)
            {
                if (state->beforeRead)
                    state->beforeRead();
                if (stopping || generation != token)
                    continue;
                std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(source));
                if (!reader || reader->sampleRate <= 0 || reader->lengthInSamples <= 0 ||
                    reader->numChannels == 0 || reader->numChannels > 64)
                    throw std::runtime_error("Cannot decode waveform");
                if (reader->lengthInSamples != a.samples || reader->sampleRate != a.sampleRate)
                    throw std::runtime_error("Recording changed; refresh before preparing its waveform");
                auto count = (int)std::min<int64_t>(
                    65536, std::max<int64_t>(1, a.samples / 256 + (a.samples % 256 != 0)));
                double binScale = (double)count / a.samples;
                data->peaks.assign((size_t)count,
                                   {std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()});
                juce::AudioBuffer<float> block((int)reader->numChannels, 65536);
                for (int64_t at = 0; at < a.samples && generation == token && !stopping;
                     at += block.getNumSamples())
                {
                    if (state->checkpoint)
                        state->checkpoint(a, at, [&] { return stopping || generation != token; });
                    if (stopping || generation != token)
                        break;
                    int n = (int)std::min<int64_t>(block.getNumSamples(), a.samples - at);
                    if (!reader->read(&block, 0, n, at, true, true))
                        throw std::runtime_error("Waveform decoding failed");
                    for (int j = 0; j < n; ++j)
                    {
                        auto bin = (size_t)std::min<int64_t>(count - 1, (int64_t)((at + j) * binScale));
                        for (int c = 0; c < block.getNumChannels(); ++c)
                        {
                            float value = block.getSample(c, j);
                            if (!std::isfinite(value))
                                value = 0;
                            data->peaks[bin].first = std::min(data->peaks[bin].first, value);
                            data->peaks[bin].second = std::max(data->peaks[bin].second, value);
                        }
                    }
                }
                if (generation != token || stopping)
                    continue;
                if (!fileStamp(source, after) || !same(after))
                    throw std::runtime_error("Recording changed while building waveform");
                juce::TemporaryFile temporary(cache);
                auto out = temporary.getFile().createOutputStream();
                if (!out)
                    throw std::runtime_error("Cannot cache waveform");
                out->writeInt(0x53505746);
                out->writeInt(1);
                out->writeInt(count);
                out->writeInt64(a.samples);
                out->writeDouble(a.sampleRate);
                out->writeInt64(a.revision);
                out->writeInt64(a.size);
                out->writeInt64(a.modified);
                out->writeInt64(a.inode);
                for (auto [minimum, maximum] : data->peaks)
                {
                    out->writeFloat(minimum);
                    out->writeFloat(maximum);
                }
                out->flush();
                out.reset();
                if (!temporary.overwriteTargetFileWithTemporary())
                    throw std::runtime_error("Cannot publish waveform cache");
            }
            if (!fileStamp(source, after) || !same(after))
                throw std::runtime_error("Recording changed while loading waveform");
        }
        catch (const std::exception &e)
        {
            if (data->status == WaveformData::Status::ready)
                data->status = WaveformData::Status::decodeError;
            data->error = e.what();
            data->peaks.clear();
        }
        std::lock_guard lock(mutex);
        if (generation == token && !stopping)
            result = std::move(data);
    }
}
std::pair<int64_t, int64_t> markRangeAtPlayhead(int64_t first, int64_t last, int64_t cursor, int64_t total,
                                                bool markIn)
{
    if (total <= 0)
        return {0, 0};
    first = std::clamp<int64_t>(first, 0, total - 1);
    last = std::clamp<int64_t>(last, first + 1, total);
    if (markIn)
    {
        first = std::clamp<int64_t>(cursor, 0, total - 1);
        if (first >= last)
            last = total;
    }
    else
    {
        last = std::clamp<int64_t>(cursor, 1, total);
        if (last <= first)
            first = 0;
    }
    return {first, last};
}
WaveformView::WaveformView()
{
    setTitle("Recording waveform. Click to seek; drag brackets to trim; I marks IN, O marks OUT. Right-click "
             "for exact range.");
    setWantsKeyboardFocus(true);
    addAndMakeVisible(zoom);
    addAndMakeVisible(all);
    addAndMakeVisible(in);
    addAndMakeVisible(out);
    zoom.onClick = [this] { zoomRange(); };
    all.onClick = [this] { showWhole(); };
    in.onClick = [this]
    {
        markAtCursor(true);
        grabKeyboardFocus();
    };
    out.onClick = [this]
    {
        markAtCursor(false);
        grabKeyboardFocus();
    };
    in.setTooltip("Set IN at playhead (I)");
    out.setTooltip("Set OUT at playhead, exclusive (O)");
    setData({});
}
void WaveformView::setData(std::shared_ptr<const WaveformData> value)
{
    bool different =
        !waveform || !value || waveform->assetId != value->assetId || waveform->revision != value->revision;
    waveform = std::move(value);
    if (different)
    {
        cursor = 0;
        showWhole();
    }
    bool available = waveform && !waveform->peaks.empty();
    for (auto *button : {&zoom, &all, &in, &out})
        button->setEnabled(available);
    repaint();
}
void WaveformView::setRange(int64_t start, int64_t end, bool zoomIn)
{
    first = start;
    last = end;
    if (zoomIn)
        zoomRange();
    repaint();
}
void WaveformView::setCursor(int64_t sample)
{
    if (cursor != sample)
    {
        cursor = sample;
        repaint();
    }
}
void WaveformView::markAtCursor(bool markIn)
{
    if (!waveform || waveform->samples <= 0 || waveform->peaks.empty())
        return;
    auto range = markRangeAtPlayhead(first, last, cursorSample(), waveform->samples, markIn);
    first = range.first;
    last = range.second;
    if (onRange)
        onRange(first, last);
    repaint();
}
void WaveformView::showWhole()
{
    viewFirst = 0;
    viewLast = waveform ? std::max<int64_t>(1, waveform->samples) : 1;
    repaint();
}
void WaveformView::zoomRange()
{
    if (!waveform || last <= first)
        return;
    auto margin = (int64_t)std::llround(waveform->rate * 2);
    viewFirst = std::max<int64_t>(0, first - margin);
    viewLast = std::min(waveform->samples, last + margin);
    if (viewLast <= viewFirst)
        viewLast = viewFirst + 1;
    repaint();
}
int64_t WaveformView::sampleAt(float x) const
{
    return std::clamp<int64_t>(
        viewFirst + (int64_t)std::llround(std::clamp(x / getWidth(), 0.0f, 1.0f) * (viewLast - viewFirst)), 0,
        waveform ? std::max<int64_t>(0, waveform->samples - 1) : 0);
}
float WaveformView::xAt(int64_t sample) const
{
    return (float)((double)(sample - viewFirst) * getWidth() / std::max<int64_t>(1, viewLast - viewFirst));
}
void WaveformView::resized()
{
    if (in.getParentComponent() == this)
    {
        in.setBounds(4, 1, 28, 25);
        out.setBounds(36, 1, 28, 25);
    }
    zoom.setBounds(getWidth() - 65, 0, 28, 28);
    all.setBounds(getWidth() - 33, 0, 28, 28);
}
void WaveformView::paint(juce::Graphics &g)
{
    g.fillAll(juce::Colour(0xff151b22));
    auto area = getLocalBounds().toFloat();
    area.removeFromTop(28);
    area.removeFromBottom(14);
    if (!waveform || waveform->peaks.empty())
    {
        g.setColour(juce::Colour(0xff9baabc));
        g.setFont(juce::FontOptions(13.0f));
        g.drawText(waveform && waveform->error.isNotEmpty() ? waveform->error
                                                            : "Select a recording to prepare its waveform",
                   area, juce::Justification::centred);
        return;
    }
    g.setColour(ui::accent.withAlpha(.2f));
    g.fillRect(juce::Rectangle<float>(xAt(first), area.getY(), xAt(last) - xAt(first), area.getHeight())
                   .getIntersection(area));
    g.setColour(ui::accent.withAlpha(.9f));
    for (int x = 0; x < getWidth(); ++x)
    {
        auto start = sampleAt((float)x), end = std::min(waveform->samples, sampleAt((float)x + 1) + 1);
        size_t a = (size_t)((double)start / waveform->samples * waveform->peaks.size());
        size_t b = (size_t)std::min<int64_t>(
            (int64_t)waveform->peaks.size(),
            (int64_t)((double)end / waveform->samples * waveform->peaks.size()) + 1);
        float minimum = 0, maximum = 0;
        for (size_t n = a; n < b; ++n)
        {
            minimum = std::min(minimum, waveform->peaks[n].first);
            maximum = std::max(maximum, waveform->peaks[n].second);
        }
        float y = area.getCentreY(), scale = area.getHeight() * 0.48f;
        g.drawVerticalLine(x, y - std::min(1.0f, maximum) * scale, y - std::max(-1.0f, minimum) * scale + 1);
    }
    g.setColour(ui::accent);
    for (auto sample : {first, last})
        if (sample >= viewFirst && sample <= viewLast)
            g.fillRect(xAt(sample) - 1, area.getY(), 2.0f, area.getHeight());
    // Bracket handles stay inside the view at the first and final source samples.
    g.setFont(juce::FontOptions(22.0f));
    if (first >= viewFirst && first <= viewLast)
        g.drawText("[",
                   juce::Rectangle<float>(
                       std::clamp(xAt(first) - 7, 0.0f, std::max(0.0f, getWidth() - 14.0f)), 25, 14, 24),
                   juce::Justification::centred);
    if (last >= viewFirst && last <= viewLast)
        g.drawText("]",
                   juce::Rectangle<float>(std::clamp(xAt(last) - 7, 0.0f, std::max(0.0f, getWidth() - 14.0f)),
                                          25, 14, 24),
                   juce::Justification::centred);
    if (cursor >= viewFirst && cursor <= viewLast)
    {
        g.setColour(ui::playhead);
        auto x = xAt(cursor);
        g.fillRect(x, area.getY(), 1.5f, area.getHeight());
        g.fillPath(
            [](float x)
            {
                juce::Path p;
                p.addTriangle(x - 4, 27, x + 4, 27, x, 32);
                return p;
            }(x));
    }
    g.setColour(juce::Colour(0xff9baabc));
    g.setFont(juce::FontOptions(11.0f));
    g.drawText(juce::String(viewFirst / waveform->rate, 2) + " s", 2, getHeight() - 14, 100, 14,
               juce::Justification::left);
    g.drawText(juce::String(viewLast / waveform->rate, 2) + " s", getWidth() - 102, getHeight() - 14, 100, 14,
               juce::Justification::right);
    int readoutX = in.getParentComponent() == this ? 76 : 8;
    g.drawText(juce::String(cursorSample() / waveform->rate, 3) + " s", readoutX, 4,
               getWidth() - readoutX - 76, 20, juce::Justification::left);
}
void WaveformView::mouseDown(const juce::MouseEvent &e)
{
    if (!waveform || waveform->peaks.empty())
        return;
    if (e.mods.isPopupMenu())
    {
        if (onEditRange)
            onEditRange();
        return;
    }
    grabKeyboardFocus();
    anchor = sampleAt(e.position.x);
    dragging = false;
    dragMode = std::abs(e.position.x - xAt(first)) < 8 ? 1 : std::abs(e.position.x - xAt(last)) < 8 ? 2 : 3;
}
void WaveformView::mouseDrag(const juce::MouseEvent &e)
{
    if (!waveform || waveform->peaks.empty() || e.getDistanceFromDragStart() < 3)
        return;
    dragging = true;
    auto sample = sampleAt(e.position.x);
    if (dragMode == 1)
        first = std::min(sample, last - 1);
    else if (dragMode == 2)
        last = std::max(first + 1, sample);
    else
    {
        first = std::min(anchor, sample);
        last = std::max(anchor + 1, sample);
    }
    if (onRange)
        onRange(first, last);
    repaint();
}
void WaveformView::mouseUp(const juce::MouseEvent &e)
{
    if (!waveform || waveform->peaks.empty())
        return;
    if (e.mods.isPopupMenu())
        return;
    if (!dragging)
    {
        setCursor(sampleAt(e.position.x));
        if (onSeek)
            onSeek(cursor);
    }
}
} // namespace sp
