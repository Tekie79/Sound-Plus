#include "CanvasEditing.h"
#include <cmath>
#include <limits>
namespace sp
{
float TimelineViewport::x(int64_t frame, double rate) const
{
    return header + (float)((frame / rate - startSeconds) * pixelsPerSecond);
}
int64_t TimelineViewport::frame(float pixel, double rate) const
{
    return std::max<int64_t>(
        0, (int64_t)std::llround((startSeconds + (pixel - header) / pixelsPerSecond) * rate));
}
int64_t TimelineViewport::delta(float pixels, double rate) const
{
    return (int64_t)std::llround(pixels / pixelsPerSecond * rate);
}
void TimelineViewport::zoom(float anchor, double factor)
{
    double time = startSeconds + (anchor - header) / pixelsPerSecond;
    pixelsPerSecond = std::clamp(pixelsPerSecond * factor, 2., 200000.);
    startSeconds = std::max(0., time - (anchor - header) / pixelsPerSecond);
}
void TimelineViewport::pan(double pixels)
{
    startSeconds = std::max(0., startSeconds + pixels / pixelsPerSecond);
}
void TimelineViewport::fit(int64_t first, int64_t last, double rate)
{
    startSeconds = std::max(0., first / rate - .03);
    pixelsPerSecond =
        std::clamp((width - header - 20) / std::max(.1, (last - first) / rate + .06), 2., 200000.);
}
CanvasClipGeometry clipGeometry(const DesignLayer &layer, int row, const TimelineViewport &view, double rate)
{
    auto duration = layerDuration(layer, rate);
    float top = 32 + row * 78.f;
    return {view.x(layer.placement, rate),
            view.x(layer.placement + duration, rate),
            top,
            view.x(layer.placement + std::min(duration, layer.fadeIn), rate),
            view.x(layer.placement + duration - std::min(duration, layer.fadeOut), rate),
            view.x(layerSyncFrame(layer, rate), rate),
            top + 36 - (float)std::clamp(layer.gainDb, -40., 24.) * .65f};
}
CanvasHit canvasHitTest(const DesignDocument &document, const TimelineViewport &view, float x, float y)
{
    if (x < 0 || y < 0 || x > view.width)
        return {};
    if (y < 30)
        return {x >= view.header ? CanvasTarget::playhead : CanvasTarget::none, -1};
    int row = (int)((y - 32) / 78);
    auto &layers = document.activeMix().layers;
    if (row < 0 || row >= (int)layers.size())
        return {x >= view.header ? CanvasTarget::empty : CanvasTarget::none, -1};
    if (x < view.header)
        return {CanvasTarget::header, row};
    auto clip = clipGeometry(layers[(size_t)row], row, view, document.sampleRate);
    bool inside = x >= clip.left && x <= clip.right;
    // Fade corners have precedence over trim edges. Sync lives below the waveform
    // so a zero fade and a source sync at IN remain individually reachable.
    if (std::abs(y - (clip.top + 10)) <= 7)
    {
        bool in = std::abs(x - clip.fadeIn) <= 7, out = std::abs(x - clip.fadeOut) <= 7;
        if (in || out)
            return {in && (!out || std::abs(x - clip.fadeIn) <= std::abs(x - clip.fadeOut))
                        ? CanvasTarget::fadeIn
                        : CanvasTarget::fadeOut,
                    row};
    }
    if (y >= clip.top + 18 && y <= clip.top + 61)
    {
        bool left = std::abs(x - clip.left) <= 6, right = std::abs(x - clip.right) <= 6;
        if (left || right)
            return {left && (!right || x < (clip.left + clip.right) * .5f) ? CanvasTarget::trimIn
                                                                           : CanvasTarget::trimOut,
                    row};
    }
    if (std::abs(x - clip.sync) <= 7 && y >= clip.top + 62 && y <= clip.top + 76)
        return {CanvasTarget::sync, row};
    if (inside && std::abs(y - clip.gain) <= 4)
        return {CanvasTarget::gain, row};
    if (std::abs(x - view.x(document.playhead, document.sampleRate)) <= 3)
        return {CanvasTarget::playhead, row};
    if (inside && y >= clip.top + 5 && y <= clip.top + 69)
        return {CanvasTarget::body, row};
    return {CanvasTarget::empty, row};
}
int64_t snapFrame(int64_t desired, const DesignDocument &document, const std::set<juce::String> &excluded,
                  const TimelineViewport &view, const SnapSettings &settings)
{
    if (!settings.enabled)
        return desired;
    int64_t result = desired;
    double distance = 8 / view.pixelsPerSecond * document.sampleRate;
    auto consider = [&](int64_t point)
    {
        auto difference = std::abs((double)point - desired);
        if (difference < distance)
        {
            distance = difference;
            result = point;
        }
    };
    if (settings.grid)
    {
        auto grid = std::max<int64_t>(1, (int64_t)std::llround(settings.gridSeconds * document.sampleRate));
        consider((int64_t)std::llround((double)desired / grid) * grid);
    }
    if (settings.playhead)
        consider(document.playhead);
    for (auto &layer : document.activeMix().layers)
        if (!excluded.contains(layer.id))
        {
            if (settings.edges)
            {
                consider(layer.placement);
                consider(layer.placement + layerDuration(layer, document.sampleRate));
            }
            if (settings.sync)
                consider(layerSyncFrame(layer, document.sampleRate));
        }
    return result;
}
int64_t sourceAtTimeline(const DesignLayer &layer, int64_t frame, double rate)
{
    auto offset =
        (int64_t)std::llround((frame - layer.placement) * layer.source.sampleRate * layer.rate / rate);
    return std::clamp<int64_t>(layer.reverse ? layer.region.endSample - 1 - offset
                                             : layer.region.startSample + offset,
                               layer.region.startSample, layer.region.endSample - 1);
}
void moveLayers(DesignDocument &document, const DesignDocument &original,
                const std::set<juce::String> &selection, int64_t requested)
{
    int64_t low = 0, high = (int64_t)original.sampleRate * 3600;
    bool first = true;
    for (auto &layer : original.activeMix().layers)
        if (selection.contains(layer.id))
        {
            auto end = layer.placement + layerDuration(layer, original.sampleRate);
            low = first ? -layer.placement : std::max(low, -layer.placement);
            high = std::min(high, (int64_t)original.sampleRate * 3600 - end);
            first = false;
        }
    auto accepted = std::clamp(requested, low, high);
    for (auto &layer : document.activeMix().layers)
        for (auto &before : original.activeMix().layers)
            if (layer.id == before.id && selection.contains(layer.id))
                layer.placement = before.placement + accepted;
}
void trimLayer(DesignLayer &layer, const DesignLayer &before, bool left, int64_t delta, double rate)
{
    layer = before;
    if (delta == 0)
        return;
    auto ratio = before.source.sampleRate * before.rate / rate;
    auto sourceDelta = (int64_t)std::llround(delta * ratio);
    auto end = before.placement + layerDuration(before, rate);
    auto maxDuration = left ? end : (int64_t)rate * 3600 - before.placement;
    auto maxLength = std::max<int64_t>(1, (int64_t)std::floor(maxDuration * ratio));
    if (left)
    {
        if (!before.reverse)
            layer.region.startSample = std::clamp<int64_t>(
                before.region.startSample + sourceDelta,
                std::max<int64_t>(0, before.region.endSample - maxLength), before.region.endSample - 1);
        else
            layer.region.endSample =
                std::clamp<int64_t>(before.region.endSample - sourceDelta, before.region.startSample + 1,
                                    std::min(before.source.samples, before.region.startSample + maxLength));
        layer.placement = end - layerDuration(layer, rate);
    }
    else if (!before.reverse)
        layer.region.endSample =
            std::clamp<int64_t>(before.region.endSample + sourceDelta, before.region.startSample + 1,
                                std::min(before.source.samples, before.region.startSample + maxLength));
    else
        layer.region.startSample = std::clamp<int64_t>(
            before.region.startSample - sourceDelta,
            std::max<int64_t>(0, before.region.endSample - maxLength), before.region.endSample - 1);
    layer.sync = std::clamp(before.sync, layer.region.startSample, layer.region.endSample - 1);
}
void slipLayer(DesignLayer &layer, const DesignLayer &before, int64_t delta, double rate)
{
    layer = before;
    auto requested = (int64_t)std::llround(delta * before.source.sampleRate * before.rate / rate) *
                     (before.reverse ? -1 : 1);
    auto accepted = std::clamp<int64_t>(requested, -before.region.startSample,
                                        before.source.samples - before.region.endSample);
    layer.region.startSample += accepted;
    layer.region.endSample += accepted;
    layer.sync += accepted;
}
void fadeLayer(DesignLayer &layer, const DesignLayer &before, bool in, int64_t delta, double rate)
{
    layer = before;
    auto length = layerDuration(before, rate);
    if (in)
        layer.fadeIn = std::clamp<int64_t>(before.fadeIn + delta, 0, length);
    else
        layer.fadeOut = std::clamp<int64_t>(before.fadeOut - delta, 0, length);
}
void gainLayer(DesignLayer &layer, const DesignLayer &before, float delta, bool fine)
{
    layer.gainDb = std::clamp(before.gainDb - delta * (fine ? .025 : .25), -90., 24.);
}
void syncLayer(DesignLayer &layer, const DesignLayer &before, int64_t frame, double rate)
{
    layer.sync = sourceAtTimeline(before, frame, rate);
}
void reorderLayer(DesignDocument &document, const juce::String &id, int insertion)
{
    auto &layers = document.activeMix().layers;
    auto found = std::find_if(layers.begin(), layers.end(), [&](auto &layer) { return layer.id == id; });
    if (found == layers.end())
        return;
    auto index = (int)std::distance(layers.begin(), found);
    auto copy = *found;
    layers.erase(found);
    if (insertion > index)
        --insertion;
    layers.insert(layers.begin() + std::clamp(insertion, 0, (int)layers.size()), std::move(copy));
}
} // namespace sp
