#include "SourcePoolView.h"
#include "Performance.h"
namespace sp
{
SourcePoolView::SourcePoolView(DesignSession &s, std::shared_ptr<WaveformPool> p, juce::PropertiesFile &prefs,
                               int owner)
    : session(s), peaks(std::move(p)), preferences(prefs), demandOwner(owner)
{
    expanded = preferences.getBoolValue("alternativesOpen", false);
    disclosure.setToggleable(true);
    lock.setToggleable(true);
    for (auto *c : std::initializer_list<juce::Component *>{&disclosure, &add, &remove, &previous, &next,
                                                            &lock, &more, &list})
        addAndMakeVisible(c);
    list.setRowHeight(62);
    list.setMultipleSelectionEnabled(false);
    list.setColour(juce::ListBox::backgroundColourId, ui::background);
    disclosure.onClick = [this]
    {
        expanded = !expanded;
        preferences.setValue("alternativesOpen", expanded);
        bind();
        if (onHeightChanged)
            onHeightChanged();
    };
    add.onClick = [this]
    {
        if (onAdd)
            onAdd();
    };
    remove.onClick = [this] { removeSelected(); };
    previous.onClick = [this] { navigate(-1); };
    next.onClick = [this] { navigate(1); };
    lock.onClick = [this]
    {
        if (auto *l = layer(); l && l->pool)
            session.lockPoolChoice(layerId, !l->pool->choiceLocked);
    };
    more.onClick = [this] { options(); };
    add.setTooltip("Choose alternate files or drag sounds/regions from Library here. Maximum 64 entries per "
                   "layer, including the initial source.");
    lock.setTooltip("Prevent seeded variations from choosing another source. Independent of parameter locks; "
                    "manual choices remain available.");
    list.setTitle("Alternatives. Click or Return selects after verification; Delete removes. Options reviews "
                  "mapping, reorders or relocates.");
    startTimerHz(30);
    bind();
}
SourcePoolView::~SourcePoolView()
{
    stopTimer();
    if (peaks)
        peaks->demand(demandOwner, {});
}
DesignLayer *SourcePoolView::layer() const
{
    for (auto &l : session.document.activeMix().layers)
        if (l.id == session.document.selectedLayer)
            return &l;
    return nullptr;
}
const SourcePoolEntry *SourcePoolView::entry(int row) const
{
    auto *l = layer();
    return l && l->pool && juce::isPositiveAndBelow(row, (int)l->pool->entries.size())
               ? &l->pool->entries[(size_t)row]
               : nullptr;
}
int SourcePoolView::activeIndex() const
{
    auto *l = layer();
    if (l && l->pool)
        for (int i = 0; i < (int)l->pool->entries.size(); ++i)
            if (l->pool->entries[(size_t)i].id == l->pool->selected)
                return i;
    return -1;
}
int SourcePoolView::getNumRows()
{
    auto *l = layer();
    return l && l->pool && !readOnly ? (int)l->pool->entries.size() : 0;
}
int SourcePoolView::preferredHeight() const
{
    auto *l = layer();
    return !expanded || readOnly ? 30 : !l || !l->pool ? 98 : 274;
}
void SourcePoolView::bind()
{
    readOnly = session.takeBusy() || session.document.activeTake.isNotEmpty() || session.transitionPending();
    auto *l = layer();
    layerId = l ? l->id : juce::String();
    auto key = session.document.id + ":" + session.document.activeVariation + ":" +
               session.document.activeTake + ":" + layerId;
    if (key != binding)
    {
        if (selectionStarted > 0)
            ++cancellations;
        binding = key;
        rows.clear();
        measuredSelection.clear();
        measuredViewport.clear();
        selectionStarted = viewportStarted = 0;
        ++bindings;
        updating = true;
        list.deselectAllRows();
        updating = false;
    }
    auto count = getNumRows();
    juce::String sig = key;
    if (l && l->pool)
        for (auto &e : l->pool->entries)
            sig += e.id + waveformSourceKey(e.source) + e.source.path + juce::String(e.source.revision) +
                   juce::String(e.region.startSample) + juce::String(e.region.endSample) +
                   (e.reviewed ? "r" : "u") + (e.enabled ? "e" : "d");
    if (l && l->pool)
        sig += l->pool->selected + (l->reverse ? "reverse" : "forward");
    if (sig != signature)
    {
        signature = sig;
        list.updateContent();
        if (auto index = activeIndex(); index >= 0)
        {
            updating = true;
            list.selectRow(index, false, false);
            updating = false;
        }
        selectionStarted = monotonicMs();
        measuredSelection.clear();
        measuredViewport.clear();
        viewportStarted = selectionStarted;
    }
    disclosure.setButtonText("Alternatives");
    disclosure.setTooltip(
        readOnly ? "Frozen/captured source. Return to Live Design after capture to edit its alternatives."
                 : "Optional alternate sources for this layer. One source remains the default.");
    disclosure.setToggleState(expanded && !readOnly, juce::dontSendNotification);
    disclosure.setEnabled(l != nullptr && !readOnly);
    add.setEnabled(l && !readOnly && count < (int)maxPoolEntries);
    remove.setEnabled(count > 1 && !readOnly);
    previous.setEnabled(count > 1 && !readOnly);
    next.setEnabled(count > 1 && !readOnly);
    lock.setEnabled(l && l->pool && !readOnly);
    lock.setToggleState(l && l->pool && l->pool->choiceLocked, juce::dontSendNotification);
    more.setEnabled(count > 0 && !readOnly);
    list.setEnabled(!readOnly);
    for (auto *c : std::initializer_list<juce::Component *>{&remove, &previous, &next, &lock, &more, &list})
        c->setVisible(expanded && !readOnly);
    add.setVisible(!readOnly);
    resized();
    repaint();
    auto h = preferredHeight();
    if (h != cachedHeight)
    {
        cachedHeight = h;
        if (onHeightChanged)
            onHeightChanged();
    }
}
void SourcePoolView::resized()
{
    disclosure.setBounds(0, 0, expanded ? 116 : getWidth() - 34, 28);
    if (expanded)
    {
        int x = 118;
        for (auto *b : {&add, &remove, &previous, &next, &lock, &more})
        {
            b->setBounds(x, 1, 25, 26);
            x += 29;
        }
    }
    else
        add.setBounds(getWidth() - 28, 1, 26, 26);
    list.setBounds(0, 34, getWidth(), std::max(0, getHeight() - 54));
}
void SourcePoolView::paint(juce::Graphics &g)
{
    if (!expanded || readOnly)
        return;
    g.setColour(ui::muted);
    g.setFont(juce::FontOptions(10.f));
    if (getNumRows() == 0)
        g.drawFittedText("Drop Library sounds or Add alternate files",
                         getLocalBounds().withTrimmedTop(34).reduced(8), juce::Justification::centred, 2);
    else
        g.drawText("64 entries max | Return selects | Options reviews input", 0, getHeight() - 18, getWidth(),
                   16, juce::Justification::centredLeft);
}
juce::String SourcePoolView::rowName(int row) const
{
    auto *e = entry(row);
    if (!e)
        return "Missing alternative";
    auto *l = layer();
    auto found = rows.find(e->id);
    auto state = found == rows.end() ? juce::String("Loading") : found->second.state;
    return (l->pool->selected == e->id ? "Selected alternative: " : "Alternative: ") + e->source.name + ", " +
           state + (e->reviewed ? ", mapping reviewed" : ", review input and mapping") + ", " +
           juce::String(e->source.channels) + " channels, " + e->id;
}
juce::String SourcePoolView::getTooltipForRow(int row)
{
    auto *e = entry(row);
    if (!e)
        return {};
    return rowName(row) + "\n" + e->source.path + "\nSource frames [" + juce::String(e->region.startSample) +
           ", " + juce::String(e->region.endSample) + ") at " + juce::String(e->source.sampleRate, 0) +
           " Hz. " + (layer()->reverse ? "Reversed overview. " : "") +
           "Before gain, fades and effects.\nDetected confidence: " + e->source.layoutConfidence +
           (e->enabled ? "\nEligible for seeded source variation."
                       : "\nExcluded from seeded variation; manual selection remains available.");
}
void SourcePoolView::paintListBoxItem(int row, juce::Graphics &g, int w, int h, bool focused)
{
    auto *e = entry(row);
    auto *l = layer();
    if (!e || !l)
        return;
    bool active = l->pool->selected == e->id;
    g.setColour(active ? ui::accent.withAlpha(.14f) : ui::background.withAlpha(.4f));
    g.fillRoundedRectangle(juce::Rectangle<float>(0, 2, (float)w, (float)h - 4), 4);
    if (focused)
    {
        g.setColour(ui::button::focus.withAlpha(.5f));
        g.drawRoundedRectangle(juce::Rectangle<float>(1, 3, w - 2.f, h - 6.f), 4, 1);
    }
    g.setColour(active ? ui::accent : ui::text);
    g.setFont(juce::FontOptions(11.5f));
    g.drawText(e->source.name, 8, 4, w - 32, 18, juce::Justification::left, true);
    if (active)
        icons::draw(g, ActionIcon::check, {w - 22.f, 6, 14, 14}, icons::Tone::selected);
    auto found = rows.find(e->id);
    auto state = found == rows.end() ? juce::String("Loading") : found->second.state;
    if (found != rows.end() && found->second.geometry)
    {
        paintCompactWaveform(g, {8, 25, w - 16.f, 16}, *found->second.geometry, ui::accent.withAlpha(.8f));
        found->second.painted = monotonicMs();
    }
    else
    {
        g.setColour(ui::muted);
        g.setFont(juce::FontOptions(10.f));
        g.drawText(state, 8, 24, w - 16, 17, juce::Justification::left);
    }
    g.setColour(ui::muted);
    g.setFont(juce::FontOptions(10.f));
    auto input =
        e->routing.interpretation.id.isNotEmpty() ? e->routing.interpretation : e->source.detectedChannels;
    g.drawText(juce::String((e->region.endSample - e->region.startSample) / e->source.sampleRate, 2) +
                   " s · " + layoutBadge(input) +
                   (!e->reviewed  ? " · Review input"
                    : !e->enabled ? " · Variation off"
                                  : ""),
               8, 43, w - 16, 15, juce::Justification::left, true);
}
void SourcePoolView::activate(int row)
{
    auto *e = entry(row);
    auto *l = layer();
    if (!e || !l || readOnly)
        return;
    if (!e->reviewed || e->reviewedBus != session.document.activeMix().layout)
    {
        if (onReview)
            onReview(e->id);
    }
    else
        session.selectPoolEntry(layerId, e->id);
}
void SourcePoolView::listBoxItemClicked(int row, const juce::MouseEvent &e)
{
    if (e.mods.isPopupMenu())
        options();
    else
        activate(row);
}
void SourcePoolView::navigate(int delta)
{
    auto n = getNumRows();
    if (n > 1)
        activate((activeIndex() + delta + n) % n);
}
void SourcePoolView::removeSelected()
{
    auto row = list.getSelectedRow();
    auto *e = entry(row);
    auto *l = layer();
    if (!e || !l || readOnly)
        return;
    auto id = e->id;
    if (l->pool->selected != id)
    {
        session.removePoolEntry(layerId, id);
        return;
    }
    juce::PopupMenu menu;
    std::vector<juce::String> ids;
    for (auto &candidate : l->pool->entries)
        if (candidate.id != id && candidate.reviewed &&
            candidate.reviewedBus == session.document.activeMix().layout)
        {
            ids.push_back(candidate.id);
            menu.addItem((int)ids.size(), "Remove and use " + candidate.source.name);
        }
    if (ids.empty())
    {
        session.message = "Review and select another source before removing the active entry";
        return;
    }
    juce::Component::SafePointer<SourcePoolView> safe(this);
    auto target = layerId;
    auto key = binding;
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&remove),
                       [safe, id, target, key, ids](int choice)
                       {
                           if (safe && safe->binding == key &&
                               juce::isPositiveAndBelow(choice - 1, (int)ids.size()))
                               safe->session.removePoolEntry(target, id, ids[(size_t)choice - 1]);
                       });
}
void SourcePoolView::options()
{
    auto *e = entry(list.getSelectedRow());
    if (!e || readOnly)
        return;
    auto id = e->id;
    auto target = layerId;
    auto key = binding;
    juce::PopupMenu menu;
    menu.addItem(1, juce::String::fromUTF8("Review input and mapping…"));
    menu.addItem(2, "Use in source variations", true, e->enabled);
    menu.addItem(3, "Move up");
    menu.addItem(4, "Move down");
    menu.addSeparator();
    menu.addItem(5, juce::String::fromUTF8("Relocate identical content…"));
    juce::Component::SafePointer<SourcePoolView> safe(this);
    bool enabled = e->enabled;
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&more),
                       [safe, id, target, key, enabled](int n)
                       {
                           if (!safe || safe->binding != key)
                               return;
                           if (n == 1 && safe->onReview)
                               safe->onReview(id);
                           if (n == 2)
                               safe->session.setPoolEntryEnabled(target, id, !enabled);
                           if (n == 3 || n == 4)
                               safe->session.reorderPoolEntry(target, id, n == 3 ? -1 : 1);
                           if (n == 5 && safe->onRelocate)
                               safe->onRelocate(id);
                       });
}
std::optional<Asset> SourcePoolView::foregroundSource() const
{
    if (!expanded || !isShowing() || readOnly)
        return {};
    if (auto *e = entry(list.getSelectedRow()))
        return e->source;
    return {};
}
void SourcePoolView::timerCallback()
{
    bind();
    bool visible = expanded && isShowing() && !readOnly;
    if (auto *viewport = findParentComponentOfClass<juce::Viewport>())
        visible &= viewport->getLocalBounds().intersects(viewport->getLocalArea(this, getLocalBounds()));
    std::vector<Asset> demands;
    std::set<juce::String> visibleIds;
    auto area = list.getViewport()->getViewArea();
    auto first = std::max(0, area.getY() / list.getRowHeight()),
         last = std::min(getNumRows(), (area.getBottom() + 61) / 62);
    if (visible)
        for (int row = first; row < last; ++row)
            if (auto *e = entry(row))
            {
                demands.push_back(e->source);
                visibleIds.insert(e->id);
            }
    peaks->demand(demandOwner, demands);
    peaks->tick();
    bool viewportReady = !visibleIds.empty();
    auto *l = layer();
    for (int row = first; visible && row < last; ++row)
        if (auto *e = entry(row))
        {
            auto &p = rows[e->id];
            p.data = peaks->get(e->source);
            p.geometry.reset();
            p.state = "Loading";
            if (p.data)
            {
                if (p.data->status == WaveformData::Status::ready && !p.data->peaks.empty())
                {
                    p.geometry = peaks->columns(e->source, e->region.startSample, e->region.endSample,
                                                l && l->reverse, std::max(1, list.getWidth() - 30));
                    p.state = "Ready";
                    if (p.geometry && std::all_of(p.geometry->begin(), p.geometry->end(),
                                                  [](auto &v) { return v.first == 0 && v.second == 0; }))
                        p.state = "Silent source";
                }
                else
                    p.state = p.data->status == WaveformData::Status::offline   ? "Offline source"
                              : p.data->status == WaveformData::Status::changed ? "Source changed"
                                                                                : "Decode error";
            }
            viewportReady &= p.state != "Loading" && p.painted >= viewportStarted;
            if (l && l->pool && e->id == l->pool->selected && p.state != "Loading" &&
                p.painted >= selectionStarted && measuredSelection != e->id)
            {
                if (selectionStarted > 0)
                {
                    if (selectedMs.size() == 512)
                        selectedMs.erase(selectedMs.begin());
                    selectedMs.push_back(monotonicMs() - selectionStarted);
                }
                measuredSelection = e->id;
                selectionStarted = 0;
            }
        }
    std::erase_if(rows, [&](auto &item) { return !visibleIds.contains(item.first); });
    auto viewKey = binding + ":" + juce::String(first) + ":" + juce::String(last);
    if (viewKey != measuredViewport)
    {
        if (viewportStarted <= 0)
            viewportStarted = monotonicMs();
        if (viewportReady)
        {
            if (viewportMs.size() == 512)
                viewportMs.erase(viewportMs.begin());
            viewportMs.push_back(monotonicMs() - viewportStarted);
            measuredViewport = viewKey;
            viewportStarted = 0;
        }
    }
    list.repaint();
}
juce::var SourcePoolView::metrics() const
{
    auto o = juce::var(new juce::DynamicObject());
    auto *v = o.getDynamicObject();
    juce::Array<juce::var> a, b;
    for (auto n : selectedMs)
        a.add(n);
    for (auto n : viewportMs)
        b.add(n);
    v->setProperty("selectedPreviewMs", a);
    v->setProperty("viewportMs", b);
    v->setProperty("retainedRows", (int)rows.size());
    juce::Array<juce::var> detail;
    for (auto &[id, row] : rows)
    {
        auto item = juce::var(new juce::DynamicObject());
        auto *o = item.getDynamicObject();
        o->setProperty("entry", id);
        o->setProperty("state", row.state);
        o->setProperty("geometry", row.geometry != nullptr);
        o->setProperty("paintedMs", row.painted);
        detail.add(item);
    }
    v->setProperty("rows", detail);
    v->setProperty("listView", list.getViewport()->getViewArea().toString());
    v->setProperty("contextBindings", (juce::int64)bindings);
    v->setProperty("cancelledBindings", (juce::int64)cancellations);
    return o;
}
bool SourcePoolView::isInterestedInDragSource(const SourceDetails &d)
{
    if (readOnly || !layer() || !d.description.isString())
        return false;
    try
    {
        auto doc = designFromJson(juce::JSON::parse(d.description.toString()));
        return doc.original.layers.size() == 1;
    }
    catch (...)
    {
        return false;
    }
}
void SourcePoolView::itemDropped(const SourceDetails &d)
{
    if (!isInterestedInDragSource(d))
        return;
    auto doc = designFromJson(juce::JSON::parse(d.description.toString()));
    auto &l = doc.original.layers[0];
    session.addPoolSource(layerId, l.source, l.region.startSample, l.region.endSample, l.external);
}
bool SourcePoolView::isInterestedInFileDrag(const juce::StringArray &paths)
{
    if (readOnly || !layer() || paths.isEmpty())
        return false;
    return std::all_of(paths.begin(), paths.end(), [](auto &p) { return supportedExtension(juce::File(p)); });
}
void SourcePoolView::filesDropped(const juce::StringArray &paths, int, int)
{
    if (isInterestedInFileDrag(paths))
        session.addPoolFiles(layerId, paths);
}
} // namespace sp
