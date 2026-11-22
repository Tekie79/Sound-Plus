#include "SourceCard.h"
#include "CanvasEditing.h"
namespace sp
{
namespace
{
ChannelLayout observedSource(const DesignLayer &l)
{
    if (l.source.layoutOverride.id.isNotEmpty())
        return l.source.layoutOverride;
    if (l.source.detectedChannels.id.isNotEmpty())
        return l.source.detectedChannels;
    return l.source.channels <= 2 ? channelLayout(l.source.channels == 1 ? "mono-v1" : "stereo-v1")
                                  : discreteLayout(l.source.channels, true);
}
juce::String cardDuration(double seconds, bool accessible = false)
{
    const bool milliseconds = seconds < .01;
    return juce::String(seconds * (milliseconds ? 1000 : 1), milliseconds || seconds < 1 ? 2 : 1) +
           (accessible ? (milliseconds ? " milliseconds" : " seconds") : (milliseconds ? " ms" : " s"));
}
} // namespace
SourceCard::SourceCard(juce::Label &t, juce::ComboBox &c, std::shared_ptr<WaveformPool> p)
    : title(t), chooser(c), pool(std::move(p))
{
    addAndMakeVisible(title);
    addAndMakeVisible(chooser);
    setWantsKeyboardFocus(true);
    title.setMinimumHorizontalScale(1.f);
}
void SourceCard::bind(const DesignLayer *value, const juce::String &id, const juce::String &name,
                      bool isSelected, double contribution, bool readOnly)
{
    assigned = id;
    alias = name;
    const auto key = value ? value->id + ":" + waveformSourceKey(value->source) + ":" +
                                 juce::String(value->region.startSample) + ":" +
                                 juce::String(value->region.endSample) + (value->reverse ? ":r" : ":f")
                           : id;
    if (key != binding)
    {
        binding = key;
        peaks.reset();
        geometry.reset();
        playhead.reset();
    }
    layer = value ? std::optional<DesignLayer>(*value) : std::nullopt;
    selected = isSelected;
    weight = contribution;
    const auto nameToShow = layer                   ? (alias.isNotEmpty() ? alias : layer->name)
                            : alias.isNotEmpty()    ? alias
                            : assigned.isNotEmpty() ? "Missing layer"
                                                    : "Drop sound";
    title.setText(nameToShow, juce::dontSendNotification);
    filename = layer && nameToShow != layer->source.name ? layer->source.name : juce::String();
    const auto description =
        layer ? nameToShow + "\n" + layer->source.path +
                    "\nAll-channel source overview before fades, gain and inserts. " +
                    juce::String(readOnly
                                     ? "Frozen take source. Choose Live Design to edit."
                                     : "Double-click waveform or press Return to open in Edit. Drag to "
                                       "assign another corner. Right-click or Shift-Return for Alternatives.")
              : "Drop a Library sound or design layer, or choose a layer with the arrow.";
    title.setTooltip(description + (readOnly ? "" : "\nDouble-click title to edit alias."));
    auto channels =
        layer ? "\nSource: " + layoutBadge(observedSource(*layer)) +
                    "; confidence: " + layer->source.layoutConfidence + "; input: " +
                    (layer->routing.interpretation.id.isEmpty() ? juce::String("Legacy pair")
                                                                : layoutBadge(layer->routing.interpretation))
              : juce::String();
    setTooltip(description + channels);
    chooser.setTooltip(readOnly ? "Frozen take source assignment"
                                : "Choose, replace or clear this blend assignment");
    setTitle(nameToShow + (layer ? ", " + layoutBadge(observedSource(*layer)) : juce::String()) +
             (layer ? ", " +
                          cardDuration((layer->region.endSample - layer->region.startSample) /
                                           (layer->source.sampleRate * layer->rate),
                                       true) +
                          juce::String(readOnly ? ". Read-only take source overview."
                                                : ". All-channel source overview. Return opens Edit.")
                    : ". Choose layer or drop sound."));
    refreshPeaks();
    repaint();
}
juce::Rectangle<float> SourceCard::waveBounds() const
{
    return {8, compact ? 25.f : 27.f, std::max(1.f, getWidth() - 16.f),
            compact ? std::max(2.f, getHeight() - 27.f - (getHeight() >= 52 ? 15.f : 0.f)) : 26.f};
}
void SourceCard::setCompact(bool value)
{
    compact = value;
    title.setFont(juce::FontOptions(compact ? 13.f : 14.f));
    resized();
    repaint();
}
void SourceCard::refreshPeaks()
{
    state = assigned.isEmpty() ? "Drop sound" : !layer ? "Missing layer" : "Loading";
    silent = false;
    if (!layer)
    {
        geometry.reset();
        setDescription("Waveform: " + state);
        return;
    }
    peaks = pool ? pool->get(layer->source) : nullptr;
    // A document may have been opened while its drive was absent. Current
    // worker-validated peaks supersede that old file-offline observation only;
    // catalog/revision/replacement errors still require explicit resolution.
    const bool reconnected = layer->source.error.startsWith("Offline or missing") && peaks &&
                             peaks->status == WaveformData::Status::ready && peaks->error.isEmpty();
    if (layer->source.error.isNotEmpty() && !reconnected)
    {
        state = layer->source.error.containsIgnoreCase("offline") ||
                        layer->source.error.containsIgnoreCase("missing")
                    ? "Offline source"
                    : "Source changed";
        geometry.reset();
        setDescription("Waveform: " + state);
        return;
    }
    if (!peaks)
        geometry.reset();
    if (peaks)
    {
        if (peaks->status != WaveformData::Status::ready || peaks->peaks.empty())
        {
            state = peaks->status == WaveformData::Status::offline   ? "Offline source"
                    : peaks->status == WaveformData::Status::changed ? "Source changed"
                                                                     : "Decode error";
            geometry.reset();
        }
        else
        {
            geometry = pool->columns(layer->source, layer->region.startSample, layer->region.endSample,
                                     layer->reverse, (int)waveBounds().getWidth());
            silent = geometry && std::all_of(geometry->begin(), geometry->end(),
                                             [](auto &p) { return p.first == 0 && p.second == 0; });
            state = silent ? "Silent source" : "Ready";
        }
    }
    setDescription("Waveform: " + state);
    repaint();
}
void SourceCard::transport(bool owned, int64_t frame, double rate)
{
    std::optional<double> next;
    if (owned && layer && frame >= layer->placement && frame < layer->placement + layerDuration(*layer, rate))
    {
        const auto source = sourceAtTimeline(*layer, frame, rate);
        next = (double)(source - layer->region.startSample) /
               std::max<int64_t>(1, layer->region.endSample - layer->region.startSample - 1);
        if (layer->reverse)
            next = 1 - *next;
    }
    if (playhead != next)
    {
        playhead = next;
        repaint();
    }
}
void SourceCard::resized()
{
    const int height = compact ? 22 : 23, chooserWidth = compact ? 22 : 26, top = compact ? 3 : 2;
    title.setBounds(6, top, std::max(1, getWidth() - chooserWidth - 12), height);
    chooser.setBounds(getWidth() - chooserWidth - 4, top, chooserWidth, height);
    refreshPeaks();
}
void SourceCard::paint(juce::Graphics &g)
{
    const auto colour = layer ? ui::trackColour(layer->colour) : ui::muted;
    g.setColour(ui::background.withAlpha(.55f));
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 6);
    g.setColour(colour.withAlpha(selected ? .8f : .28f));
    g.fillRoundedRectangle(1, 5, 2, getHeight() - 10.f, 1);
    if (geometry)
        paintCompactWaveform(g, waveBounds(), *geometry, colour.withAlpha(.85f), playhead);
    else
    {
        g.setFont(juce::FontOptions(11.f));
        g.setColour(ui::muted);
        g.drawText(state, waveBounds(), juce::Justification::centred);
    }
    g.setFont(juce::FontOptions(compact ? 10.f : 10.5f));
    g.setColour(ui::muted);
    if (layer && (!compact || getHeight() >= 52))
    {
        const auto seconds =
            (layer->region.endSample - layer->region.startSample) / (layer->source.sampleRate * layer->rate);
        const int metaY = compact ? getHeight() - 15 : 54;
        g.drawText(cardDuration(seconds), 8, metaY, 52, 14, juce::Justification::left);
        auto sourceLayout = observedSource(*layer);
        g.drawText(silent ? "Silent source" : layoutBadge(sourceLayout), 60, metaY,
                   std::max(0, getWidth() - 98), 14, juce::Justification::left, true);
        g.setColour(ui::line);
        g.fillRoundedRectangle(getWidth() - 33.f, metaY + 6.f, 23, 2, 1);
        g.setColour(ui::accent);
        g.fillRoundedRectangle(getWidth() - 33.f, metaY + 6.f, (float)weight * 23, 2, 1);
    }
    if (hasKeyboardFocus(false))
    {
        g.setColour(ui::button::focus);
        g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(.75f), 6, 1);
    }
}
void SourceCard::mouseDown(const juce::MouseEvent &e)
{
    grabKeyboardFocus();
    if (e.mods.isPopupMenu() && layer && onAlternatives)
    {
        onAlternatives(layer->id);
        return;
    }
    if (layer && onSelect)
        onSelect(layer->id);
    capturedDrag = dragPayload.clone();
}
void SourceCard::mouseDrag(const juce::MouseEvent &e)
{
    if (!layer || e.mods.isPopupMenu() || e.getDistanceFromDragStart() < 5)
        return;
    if (capturedDrag.isObject() &&
        (capturedDrag["document"] != dragPayload["document"] ||
         capturedDrag["variation"] != dragPayload["variation"] || capturedDrag["layer"] != layer->id))
        return;
    if (auto *container = juce::DragAndDropContainer::findParentDragContainerFor(this))
        if (!container->isDragAndDropActive())
            container->startDragging(
                capturedDrag.isVoid() ? juce::var("SoundPlus layer:" + layer->id) : capturedDrag, this);
}
void SourceCard::mouseDoubleClick(const juce::MouseEvent &)
{
    if (layer && onEdit)
        onEdit(layer->id);
}
bool SourceCard::keyPressed(const juce::KeyPress &k)
{
    if (k.isKeyCode(juce::KeyPress::returnKey) && k.getModifiers().isShiftDown() && layer && onAlternatives)
    {
        onAlternatives(layer->id);
        return true;
    }
    if (k.isKeyCode(juce::KeyPress::returnKey) && layer && onEdit)
    {
        onEdit(layer->id);
        return true;
    }
    return false;
}
} // namespace sp
