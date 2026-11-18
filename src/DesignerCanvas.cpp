#include "DesignerCanvas.h"
#include "Performance.h"
#include "UIComponents.h"
#include <cmath>
namespace sp
{
namespace
{
juce::String seconds(int64_t frames, double rate)
{
    return juce::String(frames / rate, 4) + " s";
}
DesignLayer *findLayer(DesignDocument &d, const juce::String &id)
{
    for (auto &layer : d.activeMix().layers)
        if (layer.id == id)
            return &layer;
    return nullptr;
}
} // namespace
struct DesignerCanvas::Header : juce::Button
{
    DesignerCanvas &canvas;
    IconButton colour{"Track colour", ActionIcon::colour};
    Header(DesignerCanvas &owner, const juce::String &name, const juce::String &id)
        : Button(name), canvas(owner)
    {
        addAndMakeVisible(colour);
        colour.onClick = [this, id] { canvas.showColours(id); };
    }
    void resized() override
    {
        colour.setBounds(getWidth() - 26, 5, 24, 24);
    }
    void paintButton(juce::Graphics &, bool, bool) override {}
    void mouseDown(const juce::MouseEvent &event) override
    {
        canvas.mouseDown(event.getEventRelativeTo(&canvas));
    }
    void mouseDrag(const juce::MouseEvent &event) override
    {
        canvas.mouseDrag(event.getEventRelativeTo(&canvas));
    }
    void mouseUp(const juce::MouseEvent &event) override
    {
        canvas.mouseUp(event.getEventRelativeTo(&canvas));
    }
    void mouseMove(const juce::MouseEvent &event) override
    {
        canvas.mouseMove(event.getEventRelativeTo(&canvas));
    }
    void mouseDoubleClick(const juce::MouseEvent &event) override
    {
        canvas.mouseDoubleClick(event.getEventRelativeTo(&canvas));
    }
};
DesignerCanvas::DesignerCanvas(DesignSession &owner, Peaks provider, juce::PropertiesFile &prefs)
    : session(owner), peaks(std::move(provider)), preferences(prefs)
{
    setWantsKeyboardFocus(true);
    setTitle("Layer canvas. Drag to seek on ruler; drag visible fade, trim, gain and sync handles. Option "
             "body drag slips source. Control is fine; Command bypasses snapping.");
    restoredViewport = preferences.containsKey("timelineScale");
    viewport.startSeconds = preferences.getDoubleValue("timelineStart", 0);
    viewport.pixelsPerSecond = preferences.getDoubleValue("timelineScale", 100);
    verticalScale = std::clamp(preferences.getDoubleValue("timelineVerticalScale", 1), .5, 3.);
    snapping.enabled = preferences.getBoolValue("timelineSnap", true);
    startTimerHz(30);
}
DesignerCanvas::~DesignerCanvas()
{
    stopTimer();
    saveViewport();
}
void DesignerCanvas::saveViewport()
{
    preferences.setValue("timelineStart", viewport.startSeconds);
    preferences.setValue("timelineScale", viewport.pixelsPerSecond);
    preferences.setValue("timelineSnap", snapping.enabled);
    preferences.setValue("timelineVerticalScale", verticalScale);
}
void DesignerCanvas::resized()
{
    if (pointerDown && viewport.width != getWidth())
        cancelDrag();
    viewport.width = (float)getWidth();
    viewport.header = getWidth() < 540 ? 110.f : 150.f;
    syncSelectors();
}
void DesignerCanvas::syncSelectors()
{
    auto &document = session.document;
    auto &layers = document.activeMix().layers;
    if (documentId != document.id)
    {
        needsInitialFit = !restoredViewport || documentId.isNotEmpty();
        documentId = document.id;
    }
    viewport.rowHeights.clear();
    for (auto &layer : layers)
        viewport.rowHeights.push_back((float)std::clamp(
            preferences.getDoubleValue(heightKey(layer.id), 78) * verticalScale, 64., 400.));
    juce::String signature;
    for (auto &layer : layers)
        signature += layer.id;
    if (headerSignature.isEmpty() && signature.isNotEmpty() && !restoredViewport)
        needsInitialFit = true;
    if (signature != headerSignature)
    {
        headers.clear();
        for (auto &layer : layers)
        {
            auto *header = headers.add(new Header(*this, "Select / reorder layer " + layer.name, layer.id));
            addAndMakeVisible(header);
            auto id = layer.id;
            header->onClick = [this, id]
            {
                clipSelection = false;
                session.selectLayer(id);
                grabKeyboardFocus();
            };
        }
        headerSignature = signature;
    }
    for (int i = 0; i < headers.size(); ++i)
    {
        headers[i]->setBounds(4, (int)viewport.rowTop(i), (int)viewport.header - 12,
                              (int)viewport.rowHeight(i) - 1);
        headers[i]->setTitle("Select / reorder layer " + layers[(size_t)i].name);
        headers[i]->colour.setTitle("Track colour for " + layers[(size_t)i].name);
        headers[i]->setTooltip(
            "Drag to reorder; Shift-click selects multiple tracks; drag the lower edge to resize");
    }
}
void DesignerCanvas::fitDesign()
{
    viewport.width = (float)getWidth();
    viewport.fit(0, designPreviewEnd(session.document), session.document.sampleRate);
    saveViewport();
    repaint();
}
void DesignerCanvas::fitSelection()
{
    int64_t first = INT64_MAX, last = 0;
    for (auto &layer : session.document.activeMix().layers)
        if (session.selectedIds.contains(layer.id))
        {
            first = std::min(first, layer.placement);
            last = std::max(last, layer.placement + layerDuration(layer, session.document.sampleRate));
        }
    if (first != INT64_MAX)
    {
        viewport.fit(first, last, session.document.sampleRate);
        saveViewport();
        repaint();
    }
}
void DesignerCanvas::zoom(double factor)
{
    viewport.zoom((viewport.header + getWidth()) * .5f, factor);
    saveViewport();
    repaint();
}
juce::String DesignerCanvas::heightKey(const juce::String &id) const
{
    return "trackHeight." + session.document.id + "." + id;
}
void DesignerCanvas::updateRows()
{
    syncSelectors();
    if (onRowsChanged)
        onRowsChanged();
    repaint();
}
void DesignerCanvas::zoomVertical(double factor)
{
    verticalScale = std::clamp(verticalScale * factor, .5, 3.);
    updateRows();
    saveViewport();
}
void DesignerCanvas::showColours(const juce::String &id)
{
    static constexpr uint32_t palette[]{0,          0xff79b6d2, 0xff72b8a2, 0xffb4a0d4,
                                        0xffd4a27c, 0xffd38393, 0xffd0bc79};
    static const char *names[]{"Default", "Blue", "Sage", "Lavender", "Amber", "Rose", "Sand"};
    juce::PopupMenu menu;
    auto *layer = findLayer(session.document, id);
    if (!layer)
        return;
    for (int i = 0; i < 7; ++i)
        menu.addColouredItem(i + 1, names[i], palette[i] ? juce::Colour(palette[i]) : ui::text, true,
                             layer->colour == palette[i]);
    auto token = session.beginDialog();
    juce::Component::SafePointer<DesignerCanvas> safe(this);
    auto found = std::find_if(session.document.activeMix().layers.begin(),
                              session.document.activeMix().layers.end(), [&](auto &l) { return l.id == id; });
    auto index = (int)std::distance(session.document.activeMix().layers.begin(), found);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&headers[index]->colour),
                       [safe, token, id](int choice)
                       {
                           if (safe && safe->session.accepts(token) && choice > 0 && choice <= 7)
                               safe->session.edit("Track colour",
                                                  [=](auto &d)
                                                  {
                                                      if (auto *selected = findLayer(d, id))
                                                          selected->colour = palette[choice - 1];
                                                  });
                       });
}
void DesignerCanvas::setDropGhost(std::optional<int64_t> frame)
{
    dropGhost = frame;
    repaint();
}
void DesignerCanvas::paint(juce::Graphics &graphics)
{
    auto &d = session.document;
    auto rate = d.sampleRate;
    graphics.fillAll(ui::background);
    auto left = viewport.header;
    auto timeline = juce::Rectangle<int>((int)left, 0, getWidth() - (int)left, getHeight());
    {
        juce::Graphics::ScopedSaveState saved(graphics);
        graphics.reduceClipRegion(timeline);
        double raw = 80 / viewport.pixelsPerSecond;
        double unit = std::pow(10., std::floor(std::log10(std::max(.0001, raw))));
        double step = unit * (raw / unit > 5 ? 10 : raw / unit > 2 ? 5 : raw / unit > 1 ? 2 : 1);
        auto first = std::floor(viewport.startSeconds / step) * step;
        graphics.setFont(juce::FontOptions(11.f));
        for (double time = first;
             time <= viewport.startSeconds + (getWidth() - left) / viewport.pixelsPerSecond; time += step)
        {
            auto x = viewport.x((int64_t)std::llround(time * rate), rate);
            graphics.setColour(ui::line);
            graphics.drawVerticalLine((int)x, 28, (float)getHeight());
            graphics.setColour(ui::muted);
            graphics.drawText(juce::String(time, step < .1 ? 3 : 2) + " s", (int)x + 4, 3, 82, 22,
                              juce::Justification::left);
        }
    }
    auto &layers = d.activeMix().layers;
    for (size_t row = 0; row < layers.size(); ++row)
    {
        auto &layer = layers[row];
        auto shape = clipGeometry(layer, (int)row, viewport, rate);
        bool selected = session.selectedIds.contains(layer.id);
        bool selectedClip = selected && clipSelection, selectedTrack = selected && !clipSelection;
        auto colour = layer.colour ? juce::Colour(layer.colour) : ui::accent;
        graphics.setColour(selectedTrack ? juce::Colour(0xff1d2b39) : ui::surface);
        graphics.fillRoundedRectangle(left, shape.top, getWidth() - left - 4, shape.height - 5, 4);
        {
            juce::Graphics::ScopedSaveState saved(graphics);
            graphics.reduceClipRegion((int)left, (int)shape.top, getWidth() - (int)left, (int)shape.height);
            auto rectangle =
                juce::Rectangle<float>(shape.left, shape.clipTop, std::max(2.f, shape.right - shape.left),
                                       shape.clipBottom - shape.clipTop);
            auto fill = layer.source.error.isEmpty()
                            ? ui::surface.interpolatedWith(colour, selectedClip ? .40f : .22f)
                            : juce::Colour(0xff724a35);
            graphics.setColour(fill);
            graphics.fillRoundedRectangle(rectangle, 3);
            if (selectedClip)
            {
                graphics.setColour(juce::Colour(0xff8ae1ce));
                graphics.drawRoundedRectangle(rectangle.reduced(.75f), 3, 1.5f);
            }
            if (auto wave = peaks(layer.source); wave && !wave->peaks.empty())
            {
                graphics.setColour(layer.mute ? ui::muted
                                              : colour.interpolatedWith(juce::Colours::white, .22f));
                for (int x = (int)std::max(left, shape.left); x < std::min((float)getWidth(), shape.right);
                     ++x)
                {
                    auto source = sourceAtTimeline(layer, viewport.frame((float)x, rate), rate);
                    auto bin = (size_t)std::clamp<int64_t>(
                        (int64_t)((double)source / layer.source.samples * wave->peaks.size()), 0,
                        (int64_t)wave->peaks.size() - 1);
                    auto peak = wave->peaks[bin];
                    graphics.drawVerticalLine(x, shape.centre - peak.second * shape.amplitude,
                                              shape.centre - peak.first * shape.amplitude);
                }
            }
            graphics.setColour(colour.withAlpha(selected ? .9f : .55f));
            graphics.drawLine(shape.left, shape.clipBottom - 1, shape.fadeIn, shape.fadeInY, 1.3f);
            graphics.drawLine(shape.fadeOut, shape.fadeInY, shape.right, shape.clipBottom - 1, 1.3f);
            graphics.drawLine(shape.left, shape.gain, shape.right, shape.gain,
                              hover.target == CanvasTarget::gain && hover.row == (int)row ? 2.f : 1.f);
            graphics.drawVerticalLine((int)shape.left, shape.clipTop + 13, shape.clipBottom - 6);
            graphics.drawVerticalLine((int)shape.right, shape.clipTop + 13, shape.clipBottom - 6);
            for (bool in : {true, false})
            {
                auto target = in ? CanvasTarget::fadeIn : CanvasTarget::fadeOut;
                auto x = in ? shape.fadeIn : shape.fadeOut;
                auto y = in ? shape.fadeInY : shape.fadeOutY;
                graphics.setColour(hover.target == target && hover.row == (int)row ? juce::Colours::white
                                                                                   : colour);
                if (!in && shape.fadeOutY != shape.fadeInY)
                    graphics.drawLine(x, shape.fadeInY, x, y, 1.f);
                graphics.fillRoundedRectangle(x - 4, y - 4, 8, 8, 2);
            }
            juce::Path sync;
            sync.addTriangle(shape.sync - 5, shape.top + shape.height - 5, shape.sync + 5,
                             shape.top + shape.height - 5, shape.sync, shape.clipBottom - 3);
            graphics.setColour(
                hover.target == CanvasTarget::sync && hover.row == (int)row ? juce::Colours::white : colour);
            graphics.fillPath(sync);
        }
        // A gutter and separator keep the fixed track headers separate from clips.
        graphics.setColour(selectedTrack ? juce::Colour(0xff263d54) : ui::surface);
        graphics.fillRoundedRectangle(4, shape.top, left - 16, shape.height - 5, 4);
        graphics.setColour(colour);
        graphics.fillRoundedRectangle(4, shape.top + 4, 3, shape.height - 13, 1);
        graphics.setColour(ui::line);
        graphics.drawVerticalLine((int)left - 8, shape.top, shape.top + shape.height - 5);
        graphics.setColour(layer.mute ? ui::muted : ui::text);
        graphics.setFont(juce::FontOptions(12.f, juce::Font::bold));
        graphics.drawText(layer.name, 13, (int)shape.top + 8, (int)left - 52, 21, juce::Justification::left,
                          true);
        graphics.setColour(ui::muted);
        graphics.setFont(juce::FontOptions(10.f));
        graphics.drawText((layer.mute ? "MUTE " : "") +
                              (layer.solo ? juce::String("SOLO ") : juce::String()) +
                              (layer.locked ? "LOCKED" : ""),
                          13, (int)shape.top + 35, (int)left - 25, 20, juce::Justification::left, true);
        if (hover.target == CanvasTarget::rowResize && hover.row == (int)row)
        {
            graphics.setColour(ui::accent.withAlpha(.65f));
            graphics.drawHorizontalLine((int)(shape.top + shape.height - 3), 4, (float)getWidth() - 4);
        }
    }
    if (layers.empty())
    {
        graphics.setColour(ui::muted);
        graphics.setFont(juce::FontOptions(16.f));
        graphics.drawFittedText("Drop a recording or add a Library range", getLocalBounds().reduced(20),
                                juce::Justification::centred, 3);
    }
    {
        juce::Graphics::ScopedSaveState saved(graphics);
        graphics.reduceClipRegion(timeline);
        float head = viewport.x(d.playhead, rate);
        graphics.setColour(juce::Colour(0xffffc384));
        graphics.drawVerticalLine((int)head, 25, (float)getHeight());
        juce::Path handle;
        handle.addTriangle(head - 6, 23, head + 6, 23, head, 31);
        graphics.fillPath(handle);
        if (dropGhost)
        {
            auto x = viewport.x(*dropGhost, rate);
            graphics.setColour(ui::accent.withAlpha(.2f));
            graphics.fillRect(x, 30.f, 90.f, getHeight() - 30.f);
            graphics.setColour(ui::accent);
            graphics.drawVerticalLine((int)x, 28, (float)getHeight());
        }
    }
    if (insertion >= 0)
    {
        graphics.setColour(ui::accent);
        graphics.fillRect(4, (int)viewport.rowTop(insertion) - 1, getWidth() - 8, 2);
    }
    if (feedback.isNotEmpty())
    {
        auto bounds = juce::Rectangle<int>(
            std::clamp((int)pointer.x + 12, 8, std::max(8, getWidth() - 250)),
            std::clamp((int)pointer.y + 15, 30, std::max(30, getHeight() - 28)), 240, 25);
        graphics.setColour(ui::raised);
        graphics.fillRoundedRectangle(bounds.toFloat(), 4);
        graphics.setColour(ui::text);
        graphics.setFont(juce::FontOptions(12.f));
        graphics.drawText(feedback, bounds.reduced(7, 0), juce::Justification::left);
    }
    if (inputTime > 0)
    {
        if (visualSamples.size() < 2048)
            visualSamples.push_back(monotonicMs() - inputTime);
        inputTime = 0;
    }
}
void DesignerCanvas::mouseDown(const juce::MouseEvent &event)
{
    if (session.transitionPending())
        return;
    grabKeyboardFocus();
    pointer = anchor = event.position;
    modifiers = event.mods;
    hit = canvasHitTest(session.document, viewport, pointer.x, pointer.y);
    original = session.document;
    originalClipSelection = clipSelection;
    captured = viewport;
    pointerDown = true;
    moved = false;
    slip = event.mods.isAltDown() && hit.target == CanvasTarget::body;
    if (hit.target == CanvasTarget::playhead || hit.target == CanvasTarget::empty)
    {
        session.beginSeekGesture(audibleScrub);
        session.dragSeek(frameAt(pointer.x));
    }
    else if (hit.target == CanvasTarget::rowResize)
    {
        auto id = original.activeMix().layers[(size_t)hit.row].id;
        originalHeight = preferences.getDoubleValue(heightKey(id), 78);
    }
    else if (hit.row >= 0 && hit.target != CanvasTarget::none)
    {
        clipSelection = hit.target != CanvasTarget::header;
        session.beginGesture();
        auto id = original.activeMix().layers[(size_t)hit.row].id;
        if (event.mods.isShiftDown() || !session.selectedIds.contains(id))
            session.selectLayer(id, event.mods.isShiftDown());
        selection = session.selectedIds;
        if (selection.empty())
            selection.insert(id);
    }
    lastTick = monotonicMs();
    updateHover(pointer);
}
void DesignerCanvas::mouseDrag(const juce::MouseEvent &event)
{
    if (!pointerDown)
        return;
    pointer = event.position;
    modifiers = event.mods;
    if (event.getDistanceFromDragStart() < 2 && !moved)
        return;
    moved = true;
    inputTime = monotonicMs();
    applyDrag();
}
void DesignerCanvas::applyDrag()
{
    if (hit.target == CanvasTarget::playhead || hit.target == CanvasTarget::empty)
    {
        session.dragSeek(frameAt(pointer.x));
        feedback = "Playhead  " + seconds(session.document.playhead, session.document.sampleRate);
        repaint();
        return;
    }
    if (hit.row < 0 || hit.row >= (int)original.activeMix().layers.size())
        return;
    auto &before = original.activeMix().layers[(size_t)hit.row];
    if (hit.target == CanvasTarget::rowResize)
    {
        preferences.setValue(heightKey(before.id),
                             std::clamp(originalHeight + (pointer.y - anchor.y) / verticalScale, 64., 320.));
        updateRows();
        return;
    }
    auto rate = original.sampleRate;
    auto delta = captured.delta((pointer.x - anchor.x) * (modifiers.isCtrlDown() ? .1f : 1.f), rate) +
                 (int64_t)std::llround((viewport.startSeconds - captured.startSeconds) * rate);
    auto snap = snapping;
    snap.enabled &= !modifiers.isCommandDown();
    if (hit.target == CanvasTarget::header)
    {
        insertion = viewport.insertionAt(pointer.y, (int)original.activeMix().layers.size());
        feedback = "Reorder layer";
        repaint();
        return;
    }
    if (hit.target == CanvasTarget::body && !slip)
        delta = snapFrame(before.placement + delta, original, selection, viewport, snap) - before.placement;
    if (hit.target == CanvasTarget::trimIn || hit.target == CanvasTarget::trimOut)
    {
        auto boundary =
            before.placement + (hit.target == CanvasTarget::trimOut ? layerDuration(before, rate) : 0);
        delta = snapFrame(boundary + delta, original, selection, viewport, snap) - boundary;
    }
    session.edit("Canvas gesture",
                 [&](auto &document)
                 {
                     if (hit.target == CanvasTarget::body && !slip)
                     {
                         moveLayers(document, original, selection, delta);
                         return;
                     }
                     auto *layer = findLayer(document, before.id);
                     if (!layer)
                         return;
                     if (slip)
                         slipLayer(*layer, before, delta, rate);
                     else if (hit.target == CanvasTarget::trimIn || hit.target == CanvasTarget::trimOut)
                         trimLayer(*layer, before, hit.target == CanvasTarget::trimIn, delta, rate);
                     else if (hit.target == CanvasTarget::fadeIn || hit.target == CanvasTarget::fadeOut)
                         fadeLayer(*layer, before, hit.target == CanvasTarget::fadeIn, delta, rate);
                     else if (hit.target == CanvasTarget::gain)
                         gainLayer(*layer, before, pointer.y - anchor.y, modifiers.isCtrlDown());
                     else if (hit.target == CanvasTarget::sync)
                         syncLayer(*layer, before,
                                   snapFrame(frameAt(pointer.x), original, selection, viewport, snap), rate);
                 });
    if (auto *layer = findLayer(session.document, before.id))
    {
        if (hit.target == CanvasTarget::fadeIn)
            feedback = "Fade in  " + seconds(layer->fadeIn, rate);
        else if (hit.target == CanvasTarget::fadeOut)
            feedback = "Fade out  " + seconds(layer->fadeOut, rate);
        else if (hit.target == CanvasTarget::gain)
            feedback = juce::String(layer->gainDb, 2) + " dB";
        else if (hit.target == CanvasTarget::sync)
            feedback = "Source sync  " + seconds(layer->sync, layer->source.sampleRate);
        else if (slip)
            feedback = "Slip  " + seconds(layer->region.startSample, layer->source.sampleRate) + " – " +
                       seconds(layer->region.endSample, layer->source.sampleRate);
        else if (hit.target == CanvasTarget::trimIn || hit.target == CanvasTarget::trimOut)
            feedback = "IN " + seconds(layer->region.startSample, layer->source.sampleRate) + "  OUT " +
                       seconds(layer->region.endSample, layer->source.sampleRate);
        else
            feedback = "Position  " + seconds(layer->placement, rate);
    }
    repaint();
}
void DesignerCanvas::mouseUp(const juce::MouseEvent &)
{
    if (!pointerDown)
        return;
    if (hit.target == CanvasTarget::header && moved && insertion >= 0)
        session.edit("Reorder layer", [&](auto &document)
                     { reorderLayer(document, original.activeMix().layers[(size_t)hit.row].id, insertion); });
    if (hit.target == CanvasTarget::empty || hit.target == CanvasTarget::playhead)
        session.endSeekGesture();
    else if (hit.target != CanvasTarget::rowResize)
        session.endGesture();
    pointerDown = false;
    insertion = -1;
    updateHover(pointer);
    saveViewport();
    repaint();
}
void DesignerCanvas::cancelDrag()
{
    if (!pointerDown)
        return;
    if (hit.target == CanvasTarget::empty || hit.target == CanvasTarget::playhead)
    {
        session.dragSeek(original.playhead);
        session.endSeekGesture();
    }
    else if (hit.target == CanvasTarget::rowResize)
    {
        preferences.setValue(heightKey(original.activeMix().layers[(size_t)hit.row].id), originalHeight);
        updateRows();
    }
    else
        session.cancelGesture();
    clipSelection = originalClipSelection;
    pointerDown = false;
    insertion = -1;
    feedback.clear();
    repaint();
}
void DesignerCanvas::focusLost(FocusChangeType)
{
    cancelDrag();
}
void DesignerCanvas::mouseMove(const juce::MouseEvent &event)
{
    pointer = event.position;
    updateHover(pointer);
}
void DesignerCanvas::mouseExit(const juce::MouseEvent &)
{
    if (!pointerDown)
    {
        hover = {};
        feedback.clear();
        repaint();
    }
}
void DesignerCanvas::updateHover(juce::Point<float> point)
{
    hover = canvasHitTest(session.document, viewport, point.x, point.y);
    auto cursor = juce::MouseCursor::NormalCursor;
    if (hover.target == CanvasTarget::trimIn || hover.target == CanvasTarget::trimOut ||
        hover.target == CanvasTarget::fadeIn || hover.target == CanvasTarget::fadeOut ||
        hover.target == CanvasTarget::sync || hover.target == CanvasTarget::playhead)
        cursor = juce::MouseCursor::LeftRightResizeCursor;
    if (hover.target == CanvasTarget::gain || hover.target == CanvasTarget::rowResize)
        cursor = juce::MouseCursor::UpDownResizeCursor;
    if (hover.target == CanvasTarget::body || hover.target == CanvasTarget::header)
        cursor = juce::MouseCursor::DraggingHandCursor;
    setMouseCursor(cursor);
    if (!pointerDown)
    {
        feedback.clear();
        if (hover.row >= 0)
        {
            auto &layer = session.document.activeMix().layers[(size_t)hover.row];
            if (hover.target == CanvasTarget::fadeIn || hover.target == CanvasTarget::fadeOut)
                feedback = (hover.target == CanvasTarget::fadeIn ? "Fade in  " : "Fade out  ") +
                           seconds(hover.target == CanvasTarget::fadeIn ? layer.fadeIn : layer.fadeOut,
                                   session.document.sampleRate);
            else if (hover.target == CanvasTarget::gain)
                feedback = juce::String(layer.gainDb, 2) + " dB; double-click resets";
            else if (hover.target == CanvasTarget::sync)
                feedback = "Source sync  " + seconds(layer.sync, layer.source.sampleRate);
        }
    }
    repaint();
}
void DesignerCanvas::mouseDoubleClick(const juce::MouseEvent &event)
{
    auto target = canvasHitTest(session.document, viewport, event.x, event.y);
    if (target.target == CanvasTarget::rowResize && target.row >= 0)
    {
        preferences.removeValue(heightKey(session.document.activeMix().layers[(size_t)target.row].id));
        updateRows();
    }
    if (target.target == CanvasTarget::gain && target.row >= 0)
    {
        auto id = session.document.activeMix().layers[(size_t)target.row].id;
        session.edit("Reset layer gain",
                     [&](auto &d)
                     {
                         if (auto *layer = findLayer(d, id))
                             layer->gainDb = 0;
                     });
    }
}
void DesignerCanvas::mouseWheelMove(const juce::MouseEvent &event, const juce::MouseWheelDetails &wheel)
{
    if (event.mods.isCommandDown() && event.mods.isAltDown())
        zoomVertical(std::exp(wheel.deltaY * 2));
    else if (event.mods.isCommandDown())
        viewport.zoom(event.x, std::exp(wheel.deltaY * 2));
    else if (event.mods.isShiftDown() || std::abs(wheel.deltaX) > .01)
        viewport.pan(-(wheel.deltaX + wheel.deltaY) * 150);
    else if (auto *parent = findParentComponentOfClass<juce::Viewport>())
        parent->mouseWheelMove(event, wheel);
    saveViewport();
    repaint();
}
void DesignerCanvas::mouseMagnify(const juce::MouseEvent &event, float factor)
{
    viewport.zoom(event.x, factor);
    saveViewport();
    repaint();
}
void DesignerCanvas::timerCallback()
{
    if (needsInitialFit && isShowing() && !session.document.activeMix().layers.empty() && !pointerDown)
    {
        fitDesign();
        needsInitialFit = false;
    }
    auto now = monotonicMs();
    auto elapsed = std::clamp((now - lastTick) / 1000., 0., .06);
    lastTick = now;
    if (!pointerDown || !moved)
        return;
    if (hit.target == CanvasTarget::header)
    {
        if (auto *parent = findParentComponentOfClass<juce::Viewport>())
        {
            int before = parent->getViewPositionY(), after = before;
            auto bottom = before + parent->getViewHeight();
            if (pointer.y < before + 18)
                after = std::max(0, before - (int)(elapsed * 180));
            else if (pointer.y > bottom - 18)
                after = std::min(getHeight() - parent->getViewHeight(), before + (int)(elapsed * 180));
            parent->setViewPosition(0, after);
            pointer.y += after - before;
            if (after != before)
                applyDrag();
        }
        return;
    }
    if (hit.target == CanvasTarget::rowResize)
        return;
    double speed = pointer.x < viewport.header + 18 ? -180 : pointer.x > getWidth() - 18 ? 180 : 0;
    if (speed != 0)
    {
        viewport.pan(speed * elapsed);
        applyDrag();
    }
}
juce::var DesignerCanvas::interactionMetrics() const
{
    juce::Array<juce::var> values;
    for (auto value : visualSamples)
        values.add(value);
    auto *object = new juce::DynamicObject();
    object->setProperty("pointerToPaintMs", values);
    return juce::var(object);
}
} // namespace sp
