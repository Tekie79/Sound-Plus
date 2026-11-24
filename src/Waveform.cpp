#include "Waveform.h"
#include "Theme.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <cmath>
namespace sp
{
namespace
{
void decodePeaks(juce::AudioFormatManager &formats, const juce::File &source, const Asset &asset,
                 juce::Range<int64_t> samples, int count, WaveformData &data,
                 const WaveformCache::Checkpoint &checkpoint, const std::function<bool()> &obsolete)
{
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(source));
    if (!reader || reader->numChannels == 0 || reader->numChannels > 64)
        throw std::runtime_error("Cannot decode waveform");
    if (reader->lengthInSamples != asset.samples || reader->sampleRate != asset.sampleRate)
        throw std::runtime_error("Recording changed; refresh before preparing its waveform");
    if (asset.waveformChannel >= (int)reader->numChannels)
        throw std::runtime_error("Waveform channel unavailable");
    data.peaks.assign((size_t)count, {std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()});
    juce::AudioBuffer<float> block((int)reader->numChannels, (int)std::min<int64_t>(samples.getLength(), 65536));
    const auto scale = (double)count / samples.getLength();
    for (int64_t at = samples.getStart(); at < samples.getEnd() && !obsolete(); at += block.getNumSamples())
    {
        if (checkpoint)
            checkpoint(asset, at, obsolete);
        if (obsolete())
            return;
        int n = (int)std::min<int64_t>(block.getNumSamples(), samples.getEnd() - at);
        if (!reader->read(&block, 0, n, at, true, true))
            throw std::runtime_error("Waveform decoding failed");
        for (int j = 0; j < n; ++j)
        {
            auto bin = (size_t)std::min<int64_t>(count - 1, (int64_t)((at - samples.getStart() + j) * scale));
            auto &[low, high] = data.peaks[bin];
            for (int c = 0; c < block.getNumChannels(); ++c)
            {
                if (asset.waveformChannel >= 0 && c != asset.waveformChannel)
                    continue;
                const float sample = block.getSample(c, j);
                const float value = std::isfinite(sample) ? sample : 0.f;
                low = std::min(low, value);
                high = std::max(high, value);
            }
        }
    }
}
} // namespace
struct WaveformCache::State
{
    juce::File directory;
    std::function<void()> beforeRead;
    Checkpoint checkpoint;
    std::mutex mutex;
    std::condition_variable wake, completed;
    std::optional<Asset> pending;
    std::optional<juce::Range<int64_t>> range;
    int rangeBins = 0;
    std::shared_ptr<const WaveformData> validation;
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
uint64_t WaveformCache::request(const Asset &asset, std::shared_ptr<const WaveformData> validate)
{
    std::lock_guard lock(state->mutex);
    auto id = ++state->generation;
    state->pending = asset;
    state->range.reset();
    state->validation = std::move(validate);
    state->wake.notify_all();
    return id;
}
uint64_t WaveformCache::requestRange(const Asset &asset, juce::Range<int64_t> samples, int bins)
{
    std::lock_guard lock(state->mutex);
    auto id = ++state->generation;
    state->pending = asset;
    state->range = samples;
    state->rangeBins = std::clamp(bins, 1, 8192);
    state->validation.reset();
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
        std::shared_ptr<const WaveformData> validation;
        std::optional<juce::Range<int64_t>> range;
        int rangeBins;
        uint64_t token;
        {
            std::unique_lock lock(mutex);
            wake.wait(lock, [&] { return stopping || pending.has_value(); });
            if (stopping)
                return;
            a = *pending;
            validation = std::move(state->validation);
            range = state->range;
            rangeBins = state->rangeBins;
            pending.reset();
            token = generation;
        }
        auto data = std::make_shared<WaveformData>();
        data->assetId = a.id;
        data->revision = a.revision;
        data->generation = token;
        data->samples = a.samples;
        data->rate = a.sampleRate;
        const auto obsolete = [&] { return stopping || generation != token; };
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
            if (range)
            {
                if (range->getStart() < 0 || range->getEnd() > a.samples || range->isEmpty())
                    throw std::runtime_error("Invalid waveform viewport");
                data->samples = range->getLength();
                if (state->beforeRead)
                    state->beforeRead();
                if (obsolete())
                    continue;
                decodePeaks(formats, source, a, *range, (int)std::min<int64_t>(rangeBins, data->samples),
                            *data, state->checkpoint, obsolete);
            }
            // Stamp-only service retains immutable overview data without rereading derivatives.
            else if (validation && validation->status == WaveformData::Status::ready)
                data->peaks = validation->peaks;
            else
            {
            directory.createDirectory();
            auto cache = directory.getChildFile(
                a.id + "-" + juce::String(a.revision) +
                (a.waveformChannel < 0 ? "-peaks-v1"
                                       : "-detail-v2-" + juce::String(a.waveformChannel) + "-" +
                                             juce::String::toHexString(a.waveformLayoutKey.hashCode64())) +
                ".bin");
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
                if (a.samples <= 0 || a.sampleRate <= 0)
                    throw std::runtime_error("Cannot decode waveform");
                auto count =
                    (int)std::min<int64_t>(a.waveformChannel >= 0 ? 2048 : 65536,
                                           std::max<int64_t>(1, a.samples / 256 + (a.samples % 256 != 0)));
                decodePeaks(formats, source, a, {0, a.samples}, count, *data, state->checkpoint, obsolete);
                if (obsolete())
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
    setTitle("Recording waveform. Click to seek, drag to mark, double-click to clear the range. "
             "I marks IN, O marks OUT. Right-click for exact range.");
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
void WaveformView::setData(std::shared_ptr<const WaveformData> value, const Asset *source)
{
    bool different =
        !waveform || !value || waveform->assetId != value->assetId || waveform->revision != value->revision;
    waveform = std::move(value);
    if (!source || !waveform || waveform->assetId != source->id || waveform->revision != source->revision)
        detailSource.reset();
    else if (!detailSource || detailSource->id != source->id || detailSource->revision != source->revision ||
             detailSource->path != source->path || detailSource->size != source->size ||
             detailSource->modified != source->modified || detailSource->inode != source->inode ||
             detailSource->waveformChannel != source->waveformChannel)
    {
        detailSource = *source;
        detailBins = 0;
    }
    if (different)
    {
        rangeSelected = false;
        cursor = 0;
        showWhole();
    }
    bool available = waveform && !waveform->peaks.empty();
    for (auto *button : {&zoom, &all, &in, &out})
        button->setEnabled(available);
    refreshDetail();
    repaint();
}
void WaveformView::setRange(int64_t start, int64_t end, bool zoomIn, bool selected)
{
    rangeSelected = selected;
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
    rangeSelected = true;
    if (onRange)
        onRange(first, last);
    repaint();
}
void WaveformView::showWhole()
{
    viewFirst = 0;
    viewLast = waveform ? std::max<int64_t>(1, waveform->samples) : 1;
    refreshDetail();
    repaint();
}
void WaveformView::zoomRange()
{
    if (!waveform || waveform->samples <= 0 || last <= first)
        return;
    viewFirst = std::clamp<int64_t>(first, 0, waveform->samples - 1);
    viewLast = std::clamp<int64_t>(last, viewFirst + 1, waveform->samples);
    refreshDetail();
    repaint();
}
void WaveformView::refreshDetail()
{
    const juce::Range<int64_t> range{viewFirst, viewLast};
    const auto length = range.getLength();
    if (!detailSource || !waveform || waveform->status != WaveformData::Status::ready ||
        waveform->peaks.empty() || length >= waveform->samples || getWidth() <= 0 ||
        (double)length / waveform->samples * waveform->peaks.size() >= getWidth())
    {
        if (detailRequest != 0)
            detailWorker->cancel();
        detailRequest = 0;
        detailBins = 0;
        detailOutline.clear();
        stopTimer();
        return;
    }
    // Sample the envelope at up to two bins per logical pixel for Retina displays.
    const auto bins = (int)std::min<int64_t>(length, std::min(8192, 2 * getWidth()));
    if (detailRange == range && detailBins == bins)
        return;
    if (!detailWorker)
        detailWorker = std::make_unique<WaveformCache>(juce::File());
    detailRange = range;
    detailBins = bins;
    detailOutline.clear();
    detailRequest = detailWorker->requestRange(*detailSource, range, bins);
    startTimerHz(30);
}
void WaveformView::timerCallback()
{
    auto data = detailWorker->snapshot();
    if (!data || data->generation != detailRequest)
        return;
    detailRequest = 0;
    stopTimer();
    if (data->peaks.empty())
    {
        detailBins = 0; // Allow a later zoom or data refresh to retry a failed read.
        return;
    }
    const auto count = data->peaks.size();
    const float inset = std::min(.04f, .65f / std::max(1.f, (getHeight() - 42) * .48f));
    auto top = [&](size_t i) { return -std::clamp(data->peaks[i].second, -1.f, 1.f) - inset; };
    auto bottom = [&](size_t i) { return -std::clamp(data->peaks[i].first, -1.f, 1.f) + inset; };
    detailOutline.startNewSubPath(0, top(0));
    for (size_t i = 1; i < count; ++i)
        detailOutline.lineTo((float)i / count, top(i));
    detailOutline.lineTo(1, top(count - 1));
    detailOutline.lineTo(1, bottom(count - 1));
    for (size_t i = count; i > 0; --i)
        detailOutline.lineTo((float)(i - 1) / count, bottom(i - 1));
    detailOutline.closeSubPath();
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
    refreshDetail();
}
void WaveformView::paint(juce::Graphics &g)
{
    g.fillAll(ui::surface);
    auto area = getLocalBounds().toFloat();
    area.removeFromTop(28);
    area.removeFromBottom(14);
    if (!waveform || waveform->peaks.empty())
    {
        g.setColour(ui::muted);
        g.setFont(juce::FontOptions(13.0f));
        g.drawText(waveform && waveform->error.isNotEmpty() ? waveform->error
                                                            : "Select a recording to prepare its waveform",
                   area, juce::Justification::centred);
        return;
    }
    if (rangeSelected)
    {
        g.setColour(ui::accent.withAlpha(.2f));
        g.fillRect(juce::Rectangle<float>(xAt(first), area.getY(), xAt(last) - xAt(first), area.getHeight())
                       .getIntersection(area));
    }
    g.setColour(ui::accent.withAlpha(.9f));
    if (!detailOutline.isEmpty())
        g.fillPath(detailOutline, juce::AffineTransform::scale(area.getWidth(), area.getHeight() * .48f)
                                      .translated(area.getX(), area.getCentreY()));
    else
    {
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
    }
    g.setColour(ui::accent);
    if (rangeSelected)
        for (auto sample : {first, last})
            if (sample >= viewFirst && sample <= viewLast)
                g.fillRect(xAt(sample) - 1, area.getY(), 2.0f, area.getHeight());
    if (!suggestion.isEmpty())
    {
        auto bounds = area.withLeft(xAt(suggestion.getStart())).withRight(xAt(suggestion.getEnd())).getIntersection(area);
        g.setColour(ui::accent.withAlpha(.16f));
        g.fillRect(bounds);
        g.setColour(ui::accent);
        g.drawRect(bounds, 1.f);
        g.setFont(juce::FontOptions(11.f));
        g.drawText("Suggested", bounds.reduced(4, 2), juce::Justification::topLeft);
    }
    if (cursor >= viewFirst && cursor <= viewLast)
    {
        g.setColour(ui::playhead);
        auto x = xAt(cursor);
        g.fillRect(x - .75f, area.getY(), 1.5f, area.getHeight());
        g.fillPath(
            [](float x)
            {
                juce::Path p;
                p.addTriangle(x - 4, 27, x + 4, 27, x, 32);
                return p;
            }(x));
    }
    g.setColour(ui::muted);
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
    rangeSelected = true;
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
    if (rangeSelected && e.getNumberOfClicks() >= 2)
        return;
    if (!dragging)
    {
        setCursor(sampleAt(e.position.x));
        if (onSeek)
            onSeek(cursor);
    }
}
void WaveformView::mouseDoubleClick(const juce::MouseEvent &e)
{
    if (!waveform || waveform->samples <= 0 || !rangeSelected || dragging || e.mods.isPopupMenu())
        return;
    setRange(0, waveform->samples, false, false);
    showWhole();
    if (onClearRange)
        onClearRange();
}
} // namespace sp
