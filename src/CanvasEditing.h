#pragma once
#include "DesignModel.h"
#include <juce_graphics/juce_graphics.h>
#include <set>
namespace sp
{
struct TimelineViewport
{
    double startSeconds = 0, pixelsPerSecond = 100;
    float header = 130, width = 700;
    std::vector<float> rowHeights;
    float rowTop(int row) const;
    float rowHeight(int row) const;
    int rowAt(float y, int count) const;
    int insertionAt(float y, int count) const;
    float contentHeight(int count) const
    {
        return rowTop(count);
    }
    float x(int64_t frame, double rate) const;
    int64_t frame(float x, double rate) const;
    int64_t delta(float pixels, double rate) const;
    void zoom(float anchor, double factor);
    void pan(double pixels);
    void fit(int64_t first, int64_t last, double rate);
};
enum class CanvasTarget
{
    none,
    empty,
    header,
    rowResize,
    body,
    trimIn,
    trimOut,
    fadeIn,
    fadeOut,
    fadeInPoint,
    fadeOutPoint,
    gain,
    sync,
    playhead
};
struct CanvasHit
{
    CanvasTarget target = CanvasTarget::none;
    int row = -1;
    int point = -1;
};
struct CanvasClipGeometry
{
    float left = 0, right = 0, top = 0, fadeIn = 0, fadeOut = 0, sync = 0, gain = 0;
    float height = 78, clipTop = 0, clipBottom = 0, centre = 0, amplitude = 0;
    float fadeInY = 0, fadeOutY = 0;
};
CanvasClipGeometry clipGeometry(const DesignLayer &, int row, const TimelineViewport &, double rate);
CanvasHit canvasHitTest(const DesignDocument &, const TimelineViewport &, float x, float y,
                        bool preferOut = false);
juce::Point<float> fadePointPosition(const DesignLayer &, int row, bool in, int point,
                                     const TimelineViewport &, double rate);
FadePoint fadePointAt(const DesignLayer &, int row, bool in, juce::Point<float>, const TimelineViewport &,
                      double rate);
int addFadePoint(DesignLayer &, bool in, FadePoint);
void moveFadePoint(DesignLayer &, const DesignLayer &before, bool in, int point, FadePoint delta);
void removeFadePoint(DesignLayer &, bool in, int point);
struct SnapSettings
{
    bool enabled = true, grid = true, playhead = true, edges = true, sync = true;
    double gridSeconds = .1;
};
int64_t snapFrame(int64_t desired, const DesignDocument &, const std::set<juce::String> &excluded,
                  const TimelineViewport &, const SnapSettings &);
// Edit commands are shared by real pointer gestures and invariant tests. Every
// command starts from captured source/model values, never from the last move event.
void moveLayers(DesignDocument &, const DesignDocument &original, const std::set<juce::String> &,
                int64_t delta);
void trimLayer(DesignLayer &, const DesignLayer &original, bool left, int64_t timelineDelta, double rate);
void slipLayer(DesignLayer &, const DesignLayer &original, int64_t timelineDelta, double rate);
void fadeLayer(DesignLayer &, const DesignLayer &original, bool in, int64_t timelineDelta, double rate);
void gainLayer(DesignLayer &, const DesignLayer &original, float verticalPixels, bool fine);
void syncLayer(DesignLayer &, const DesignLayer &original, int64_t timelineFrame, double rate);
void reorderLayer(DesignDocument &, const juce::String &, int insertion);
int64_t sourceAtTimeline(const DesignLayer &, int64_t frame, double rate);
} // namespace sp
