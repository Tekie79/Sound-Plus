#include "CanvasEditing.h"
#include <cmath>
#include <limits>
namespace sp
{
float TimelineViewport::rowHeight(int row) const
{
    return row >= 0 && row < (int)rowHeights.size() ? rowHeights[(size_t)row] : 78.f;
}
float TimelineViewport::rowTop(int row) const
{
    float top = 32;
    for (int i = 0; i < row; ++i)
        top += rowHeight(i);
    return top;
}
int TimelineViewport::rowAt(float y, int count) const
{
    for (int i = 0; i < count; ++i)
        if (y >= rowTop(i) && y < rowTop(i + 1))
            return i;
    return -1;
}
int TimelineViewport::insertionAt(float y, int count) const
{
    for (int i = 0; i < count; ++i)
        if (y < rowTop(i) + rowHeight(i) * .5f)
            return i;
    return count;
}
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
    pixelsPerSecond = std::clamp(pixelsPerSecond * factor, .01, 200000.);
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
        std::clamp((width - header - 20) / std::max(.1, (last - first) / rate + .06), .01, 200000.);
}
CanvasClipGeometry clipGeometry(const DesignLayer &layer, int row, const TimelineViewport &view, double rate)
{
    auto duration = layerDuration(layer, rate);
    float top = view.rowTop(row), height = view.rowHeight(row);
    CanvasClipGeometry shape{view.x(layer.placement, rate),
                             view.x(layer.placement + duration, rate),
                             top,
                             view.x(layer.placement + std::min(duration, layer.fadeIn), rate),
                             view.x(layer.placement + duration - std::min(duration, layer.fadeOut), rate),
                             view.x(layerSyncFrame(layer, rate), rate),
                             top + (height - 6) * .5f - (float)std::clamp(layer.gainDb, -40., 24.) * .65f};
    shape.height = height;
    shape.clipTop = top + 5;
    shape.clipBottom = top + height - 11;
    shape.centre = (shape.clipTop + shape.clipBottom) * .5f;
    shape.amplitude = (shape.clipBottom - shape.clipTop) * .37f;
    shape.fadeInY = top + 10;
    // Stagger the OUT grip vertically when both fades meet, including tiny clips.
    shape.fadeOutY = shape.fadeInY + (std::abs(shape.fadeIn - shape.fadeOut) < 15 ? 17.f : 0.f);
    return shape;
}
CanvasHit canvasHitTest(const DesignDocument &document, const TimelineViewport &view, float x, float y,
                        bool preferOut)
{
    if (x < 0 || y < 0 || x > view.width)
        return {};
    if (y < 30)
        return {x >= view.header ? CanvasTarget::playhead : CanvasTarget::none, -1};
    auto &layers = document.activeMix().layers;
    int row = view.rowAt(y, (int)layers.size());
    if (row < 0 || row >= (int)layers.size())
        return {x >= view.header ? CanvasTarget::empty : CanvasTarget::none, -1};
    if (std::abs(y - (view.rowTop(row + 1) - 3)) <= 3)
        return {CanvasTarget::rowResize, row};
    if (x < view.header - 8)
        return {CanvasTarget::header, row};
    if (x < view.header)
        return {CanvasTarget::none, row};
    auto clip = clipGeometry(layers[(size_t)row], row, view, document.sampleRate);
    auto &layer = layers[(size_t)row];
    CanvasHit pointHit;
    float nearestPoint = 6;
    for (bool in : {true, false})
        if ((in ? layer.fadeInShape : layer.fadeOutShape) == FadeShape::custom &&
            (in ? layer.fadeIn : layer.fadeOut) > 0)
        {
            auto &points = in ? layer.fadeInPoints : layer.fadeOutPoints;
            for (int i = 0; i < (int)points.size(); ++i)
            {
                auto position = fadePointPosition(layer, row, in, i, view, document.sampleRate);
                if (position.x < clip.left || position.x > clip.right)
                    continue;
                auto distance = position.getDistanceFrom({x, y});
                if (distance < nearestPoint ||
                    (preferOut && !in && distance < 6 && distance <= nearestPoint + .001f))
                {
                    nearestPoint = distance;
                    pointHit = {in ? CanvasTarget::fadeInPoint : CanvasTarget::fadeOutPoint, row, i};
                }
            }
        }
    auto gripDistance = std::min(juce::Point<float>(clip.fadeIn, clip.fadeInY).getDistanceFrom({x, y}),
                                 juce::Point<float>(clip.fadeOut, clip.fadeOutY).getDistanceFrom({x, y}));
    if (pointHit.point >= 0 && nearestPoint < gripDistance)
        return pointHit;
    bool inside = x >= clip.left && x <= clip.right;
    // Fade corners have precedence over trim edges. Sync lives below the waveform
    // so a zero fade and a source sync at IN remain individually reachable.
    if (std::abs(y - clip.fadeInY) <= 7 || std::abs(y - clip.fadeOutY) <= 7)
    {
        bool in = std::abs(x - clip.fadeIn) <= 7 && std::abs(y - clip.fadeInY) <= 7;
        bool out = std::abs(x - clip.fadeOut) <= 7 && std::abs(y - clip.fadeOutY) <= 7;
        if (in || out)
            return {in && (!out || std::abs(x - clip.fadeIn) <= std::abs(x - clip.fadeOut))
                        ? CanvasTarget::fadeIn
                        : CanvasTarget::fadeOut,
                    row};
    }
    if (y >= clip.clipTop + 13 && y <= clip.clipBottom - 6)
    {
        bool left = std::abs(x - clip.left) <= 6, right = std::abs(x - clip.right) <= 6;
        if (left || right)
            return {left && (!right || x < (clip.left + clip.right) * .5f) ? CanvasTarget::trimIn
                                                                           : CanvasTarget::trimOut,
                    row};
    }
    if (std::abs(x - clip.sync) <= 7 && y >= clip.clipBottom - 3 && y <= clip.top + clip.height - 5)
        return {CanvasTarget::sync, row};
    if (inside && std::abs(y - clip.gain) <= 4)
        return {CanvasTarget::gain, row};
    if (std::abs(x - view.x(document.playhead, document.sampleRate)) <= 3)
        return {CanvasTarget::playhead, row};
    if (inside && y >= clip.clipTop && y <= clip.clipBottom + 2)
        return {CanvasTarget::body, row};
    return {CanvasTarget::empty, row};
}
juce::Point<float> fadePointPosition(const DesignLayer &layer, int row, bool in, int index,
                                     const TimelineViewport &view, double rate)
{
    auto &point = (in ? layer.fadeInPoints : layer.fadeOutPoints)[(size_t)index];
    auto shape = clipGeometry(layer, row, view, rate);
    auto duration = in ? layer.fadeIn : layer.fadeOut;
    auto offset = (int64_t)std::llround(point.x * duration);
    auto frame = in ? layer.placement + offset : layer.placement + layerDuration(layer, rate) - offset;
    return {view.x(frame, rate),
            shape.clipBottom - 1 - (float)point.y * (shape.clipBottom - 1 - shape.fadeInY)};
}
FadePoint fadePointAt(const DesignLayer &layer, int row, bool in, juce::Point<float> position,
                      const TimelineViewport &view, double rate)
{
    auto shape = clipGeometry(layer, row, view, rate);
    auto frame = view.frame(position.x, rate);
    auto duration = std::max<int64_t>(1, in ? layer.fadeIn : layer.fadeOut);
    double x = (double)(in ? frame - layer.placement : layer.placement + layerDuration(layer, rate) - frame) /
               duration;
    double y = (shape.clipBottom - 1 - position.y) / (shape.clipBottom - 1 - shape.fadeInY);
    return {std::clamp(x, 2 * fadePointEpsilon, 1 - 2 * fadePointEpsilon), std::clamp(y, 0., 1.)};
}
int addFadePoint(DesignLayer &layer, bool in, FadePoint point)
{
    auto &shape = in ? layer.fadeInShape : layer.fadeOutShape;
    auto &points = in ? layer.fadeInPoints : layer.fadeOutPoints;
    if (shape != FadeShape::custom)
        points.clear();
    shape = FadeShape::custom;
    point.x = std::clamp(point.x, 2 * fadePointEpsilon, 1 - 2 * fadePointEpsilon);
    point.y = std::clamp(point.y, 0., 1.);
    for (size_t i = 0; i < points.size(); ++i)
        if (std::abs(points[i].x - point.x) < fadePointEpsilon)
        {
            points[i].y = point.y;
            return (int)i;
        }
    if (points.size() >= maxFadePoints)
        throw std::runtime_error("At most 32 interior points per fade");
    auto found =
        std::lower_bound(points.begin(), points.end(), point.x, [](auto &p, double x) { return p.x < x; });
    auto index = (int)std::distance(points.begin(), found);
    points.insert(found, point);
    return index;
}
void moveFadePoint(DesignLayer &layer, const DesignLayer &before, bool in, int index, FadePoint delta)
{
    auto &original = in ? before.fadeInPoints : before.fadeOutPoints;
    if (index < 0 || index >= (int)original.size())
        return;
    auto &points = in ? layer.fadeInPoints : layer.fadeOutPoints;
    points = original;
    auto low = index == 0 ? 2 * fadePointEpsilon : original[(size_t)index - 1].x + 2 * fadePointEpsilon;
    auto high = index + 1 == (int)original.size() ? 1 - 2 * fadePointEpsilon
                                                  : original[(size_t)index + 1].x - 2 * fadePointEpsilon;
    points[(size_t)index] = {low <= high ? std::clamp(original[(size_t)index].x + delta.x, low, high)
                                         : original[(size_t)index].x,
                             std::clamp(original[(size_t)index].y + delta.y, 0., 1.)};
}
void removeFadePoint(DesignLayer &layer, bool in, int index)
{
    auto &points = in ? layer.fadeInPoints : layer.fadeOutPoints;
    if (index >= 0 && index < (int)points.size())
        points.erase(points.begin() + index);
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
