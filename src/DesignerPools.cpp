#include "DesignerView.h"
#include <set>
namespace sp
{
namespace
{
class PoolPopupOwner final : public juce::Component
{
  public:
    std::unique_ptr<juce::Component> content;
    std::unique_ptr<juce::CallOutBox> window;
    PoolPopupOwner(std::unique_ptr<juce::Component> c, juce::Rectangle<int> target, juce::LookAndFeel &look,
                   std::function<void()> closed)
        : content(std::move(c)), window(std::make_unique<juce::CallOutBox>(*content, target, nullptr))
    {
        window->setLookAndFeel(&look);
        window->setVisible(true);
        window->enterModalState(true, juce::ModalCallbackFunction::create([closed](int) { closed(); }),
                                false);
    }
    ~PoolPopupOwner() override
    {
        window->exitModalState(0);
        window->setLookAndFeel(nullptr);
        window.reset();
    }
};
void dismiss(std::unique_ptr<juce::Component> &owner)
{
    if (auto *p = dynamic_cast<PoolPopupOwner *>(owner.get()))
        p->window->dismiss();
}
class PoolMappingReview final : public juce::Component
{
  public:
    SourcePoolEntry entry;
    ChannelLayout bus;
    LayerRouting resolved;
    int pair = 0;
    juce::ComboBox physical, interpretation, mapping;
    juce::TextEditor custom, explanation;
    juce::Label title, inputLabel, roleLabel, mapLabel;
    ui::TextActionButton use{"Use source"}, cancel{"Cancel"};
    std::function<void()> onUse, onCancel;
    std::vector<ChannelLayout> choices;
    bool updating = false;
    PoolMappingReview(SourcePoolEntry e, ChannelLayout destination, int version)
        : entry(std::move(e)), bus(std::move(destination))
    {
        for (auto *c : std::initializer_list<juce::Component *>{&title, &inputLabel, &roleLabel, &mapLabel,
                                                                &physical, &interpretation, &mapping, &custom,
                                                                &explanation, &use, &cancel})
            addAndMakeVisible(c);
        title.setText(entry.source.name, juce::dontSendNotification);
        title.setTooltip(entry.source.path);
        inputLabel.setText("Physical inputs", juce::dontSendNotification);
        roleLabel.setText("Input roles", juce::dontSendNotification);
        mapLabel.setText("Mapping to design", juce::dontSendNotification);
        for (auto *l : {&title, &inputLabel, &roleLabel, &mapLabel})
        {
            l->setColour(juce::Label::textColourId, ui::text);
            l->setFont(juce::FontOptions(12.f));
        }
        physical.setTitle("Alternate physical source inputs");
        interpretation.setTitle("Alternate ordered input interpretation");
        mapping.setTitle("Reviewed alternate route mapping");
        bool legacy = bus.id.isEmpty();
        if (entry.source.channels <= 16 && (!legacy || entry.source.channels <= 2))
            physical.addItem("Complete source", 1);
        for (int p = 0; p < (entry.source.channels + 1) / 2; ++p)
            physical.addItem(
                "Channels " + juce::String(p * 2 + 1) +
                    (p * 2 + 2 <= entry.source.channels ? " / " + juce::String(p * 2 + 2) : juce::String()),
                100 + p);
        if (legacy || version == 2)
            physical.addItem(legacy ? "Analysis Mix (all inputs)" : "All physical channels → Mono mean", 2);
        if (!legacy)
            physical.addItem("Custom subset (up to 16)…", 999);
        custom.setTitle("Physical source channels, comma-separated, one-based");
        custom.setTextToShowWhenEmpty("e.g. 1, 3, 7 (at most 16)", ui::muted);
        juce::StringArray numbers;
        for (auto i : entry.routing.inputs)
            numbers.add(juce::String(i + 1));
        custom.setText(numbers.joinIntoString(", "), false);
        int preset = entry.source.channels <= 16 && (!legacy || entry.source.channels <= 2) ? 1
                     : legacy                                                               ? 100
                                                                                            : 999;
        if (entry.reviewed)
        {
            if (entry.routing.legacyAnalysisMean || (legacy && entry.region.auditionPair < 0))
                preset = 2;
            else if (legacy)
                preset = entry.source.channels <= 2 ? 1 : 100 + entry.region.auditionPair;
            else if (!entry.routing.inputs.empty() && entry.routing.inputs.size() <= 2 &&
                     entry.routing.inputs.front() % 2 == 0)
                preset = 100 + entry.routing.inputs.front() / 2;
        }
        physical.setSelectedId(preset, juce::dontSendNotification);
        mapping.addItem("Match named roles", 1);
        mapping.addItem("Physical order (extra inputs muted)", 2);
        if (entry.reviewed && !entry.routing.routes.empty() && entry.reviewedBus == bus)
            mapping.addItem("Existing reviewed matrix", 3);
        mapping.setSelectedId(mapping.getNumItems() == 3 ? 3 : 1, juce::dontSendNotification);
        mapping.setEnabled(!legacy);
        explanation.setReadOnly(true);
        explanation.setMultiLine(true);
        explanation.setScrollbarsShown(true);
        explanation.setTitle("Alternate mapping and source review");
        explanation.setColour(juce::TextEditor::backgroundColourId, ui::background);
        explanation.setColour(juce::TextEditor::textColourId, ui::muted);
        physical.onChange = [this] { refresh(true); };
        custom.onTextChange = [this] { refresh(true); };
        interpretation.onChange = [this] { refresh(false); };
        mapping.onChange = [this] { refresh(false); };
        use.onClick = [this]
        {
            if (use.isEnabled() && onUse)
                onUse();
        };
        cancel.onClick = [this]
        {
            if (onCancel)
                onCancel();
        };
        use.setTooltip("Verify content, then select this source. Owned Designer playback stops; press Play "
                       "to restart. Layer controls are retained.");
        setSize(438, 374);
        refresh(true);
    }
    std::vector<int> inputs() const
    {
        std::vector<int> result;
        int id = physical.getSelectedId();
        if (id == 1 || id == 2)
        {
            for (int i = 0; i < entry.source.channels; ++i)
                result.push_back(i);
        }
        else if (id >= 100 && id < 200)
        {
            int i = (id - 100) * 2;
            result.push_back(i);
            if (i + 1 < entry.source.channels)
                result.push_back(i + 1);
        }
        else if (id == 999)
        {
            auto values = juce::StringArray::fromTokens(custom.getText(), ", ", "");
            values.removeEmptyStrings();
            std::set<int> used;
            for (auto &s : values)
            {
                int i = s.getIntValue() - 1;
                if (!s.containsOnly("0123456789") || i < 0 || i >= entry.source.channels ||
                    !used.insert(i).second)
                    throw std::runtime_error(
                        "Use distinct existing physical channel numbers separated by commas");
                result.push_back(i);
            }
        }
        if (result.empty() || (id != 2 && result.size() > 16))
            throw std::runtime_error("Choose 1–16 physical inputs or the explicit all-channel Mono mean");
        return result;
    }
    void refresh(bool resetRoles)
    {
        if (updating)
            return;
        updating = true;
        use.setEnabled(false);
        custom.setVisible(physical.getSelectedId() == 999);
        try
        {
            auto selected = inputs();
            int count = physical.getSelectedId() == 2 ? 1 : (int)selected.size();
            bool legacy = bus.id.isEmpty();
            if (resetRoles)
            {
                int old = interpretation.getSelectedId();
                juce::String desired = juce::isPositiveAndBelow(old - 1, (int)choices.size())
                                           ? choices[(size_t)old - 1].id
                                           : juce::String();
                choices.clear();
                for (auto l : namedLayouts())
                    if (l.size() == count)
                        choices.push_back(l);
                auto discrete = discreteLayout(count);
                choices.push_back(discrete);
                interpretation.clear(juce::dontSendNotification);
                int choice = 0;
                if (legacy || physical.getSelectedId() == 2)
                    desired = count == 1 ? "mono-v1" : "stereo-v1";
                else if (desired.isEmpty())
                    desired = entry.reviewed && entry.routing.interpretation.size() == count
                                  ? entry.routing.interpretation.id
                              : physical.getSelectedId() != 1 ? (count == 1   ? "mono-v1"
                                                                 : count == 2 ? "stereo-v1"
                                                                              : juce::String())
                              : entry.source.layoutOverride.id.isNotEmpty()
                                  ? entry.source.layoutOverride.id
                                  : entry.source.detectedChannels.id;
                for (size_t i = 0; i < choices.size(); ++i)
                {
                    interpretation.addItem(layoutName(choices[i]), (int)i + 1);
                    if (choices[i].id == desired)
                        choice = (int)i + 1;
                }
                interpretation.setSelectedId(choice, juce::dontSendNotification);
                interpretation.setTextWhenNothingSelected("Choose ordered interpretation");
                interpretation.setEnabled(!legacy);
            }
            int role = interpretation.getSelectedId() - 1;
            if (!juce::isPositiveAndBelow(role, (int)choices.size()))
                throw std::runtime_error(
                    "Channel count is not an interpretation. Choose ordered roles explicitly.");
            auto layout = choices[(size_t)role];
            resolved = {};
            resolved.inputs = selected;
            resolved.interpretation = layout;
            resolved.legacyAnalysisMean = physical.getSelectedId() == 2;
            if (entry.reviewed && entry.reviewedBus == bus && entry.routing.inputs == selected &&
                entry.routing.interpretation == layout && mapping.getSelectedId() == 3)
                resolved = entry.routing;
            else if (mapping.getSelectedId() == 2 && !legacy)
                for (int i = 0; i < std::min(layout.size(), bus.size()); ++i)
                    resolved.routes.push_back({i, i, 0, false, false});
            pair = physical.getSelectedId() == 2 ? -1
                   : physical.getSelectedId() >= 100 && physical.getSelectedId() < 200
                       ? physical.getSelectedId() - 100
                       : 0;
            auto checked = entry;
            checked.region.auditionPair = pair;
            reviewPoolEntry(checked, legacy ? LayerRouting{} : resolved, bus);
            if (legacy)
                resolved = {};
            juce::String text = "Original frames [" + juce::String(entry.region.startSample) + ", " +
                                juce::String(entry.region.endSample) + ") at " +
                                juce::String(entry.source.sampleRate, 0) + " Hz.\n";
            juce::StringArray channels;
            for (auto i : selected)
                channels.add(juce::String(i + 1));
            text += "Physical: " + channels.joinIntoString(", ") +
                    "\nInput: " + layout.roles.joinIntoString(", ") + "\n";
            if (legacy)
                text += "Legacy stereo audition: " +
                        juce::String(pair < 0 ? "all-channel arithmetic Analysis Mix"
                                              : "explicit physical pair") +
                        ".\n";
            else
            {
                auto matrix = routingMatrix(resolved, bus, 0);
                text += "Design: " + bus.roles.joinIntoString(", ") + "\n";
                for (int out = 0; out < bus.size(); ++out)
                    for (int in = 0; in < layout.size(); ++in)
                        if (matrix[out][in] != 0)
                            text += layout.roles[in] + " → " + bus.roles[out] + "  " +
                                    juce::String(matrix[out][in], 4) + "\n";
                text += "Unlisted routes are muted; LFE follows this explicit matrix.\n";
            }
            text += "Source overview before effects. Placement, reverse, fades and effects are retained.";
            explanation.setText(text, false);
            use.setEnabled(true);
        }
        catch (const std::exception &error)
        {
            explanation.setText(error.what(), false);
        }
        updating = false;
        resized();
    }
    void paint(juce::Graphics &g) override
    {
        g.fillAll(ui::surface);
    }
    void resized() override
    {
        title.setBounds(12, 8, getWidth() - 24, 28);
        inputLabel.setBounds(12, 44, 112, 28);
        physical.setBounds(128, 44, getWidth() - 140, 28);
        roleLabel.setBounds(12, 78, 112, 28);
        interpretation.setBounds(128, 78, getWidth() - 140, 28);
        mapLabel.setBounds(12, 112, 112, 28);
        mapping.setBounds(128, 112, getWidth() - 140, 28);
        custom.setBounds(12, 148, getWidth() - 24, 26);
        int top = custom.isVisible() ? 182 : 148;
        explanation.setBounds(12, top, getWidth() - 24, 322 - top);
        cancel.setBounds(getWidth() - 208, 336, 90, 28);
        use.setBounds(getWidth() - 110, 336, 98, 28);
    }
};
} // namespace
void DesignerView::choosePoolFiles(const juce::String &layer, const juce::String &relocate)
{
    if (session.takeBusy() || session.document.activeTake.isNotEmpty())
        return;
    auto token = session.beginDialog();
    auto document = session.document.id, variation = session.document.activeVariation;
    poolChooser = std::make_unique<juce::FileChooser>(
        relocate.isEmpty() ? "Add alternatives (64 per layer maximum)"
                           : "Relocate identical alternate content",
        juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.m4a;*.caf;*.ogg", true);
    juce::Component::SafePointer<DesignerView> safe(this);
    poolChooser->launchAsync(
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles |
            (relocate.isEmpty() ? juce::FileBrowserComponent::canSelectMultipleItems : 0),
        [safe, token, document, variation, layer, relocate](const juce::FileChooser &choice)
        {
            if (!safe || !safe->session.accepts(token) || safe->session.document.id != document ||
                safe->session.document.activeVariation != variation)
                return;
            if (relocate.isNotEmpty())
            {
                if (choice.getResult() != juce::File())
                    safe->session.relinkPoolEntry(layer, relocate, choice.getResult());
            }
            else
            {
                juce::StringArray paths;
                for (auto &f : choice.getResults())
                    paths.add(f.getFullPathName());
                safe->session.addPoolFiles(layer, paths);
            }
        });
}
void DesignerView::reviewPoolSource(const juce::String &id)
{
    auto *layer = session.selectedLayer();
    if (!layer || !layer->pool || session.takeBusy() || session.document.activeTake.isNotEmpty())
        return;
    auto at = std::find_if(layer->pool->entries.begin(), layer->pool->entries.end(),
                           [&](auto &e) { return e.id == id; });
    if (at == layer->pool->entries.end())
        return;
    auto original = juce::JSON::toString(poolEntryToJson(*at), true);
    auto document = session.document.id, variation = session.document.activeVariation, layerId = layer->id;
    auto bus = session.document.activeMix().layout;
    auto token = session.beginDialog();
    auto serial = ++poolReviewSerial;
    auto panel = std::make_unique<PoolMappingReview>(*at, bus, session.document.activeMix().routingVersion);
    auto *review = panel.get();
    juce::Component::SafePointer<DesignerView> safe(this);
    panel->onCancel = [safe, serial]
    {
        if (safe && safe->poolReviewSerial == serial)
            dismiss(safe->poolReview);
    };
    panel->onUse = [safe, review, token, document, variation, layerId, id, original, bus, serial]
    {
        if (!safe || safe->poolReviewSerial != serial || !safe->session.accepts(token) ||
            safe->session.document.id != document || safe->session.document.activeVariation != variation ||
            safe->session.document.activeMix().layout != bus)
            return;
        auto *l = safe->session.selectedLayer();
        if (!l || l->id != layerId || !l->pool)
            return;
        auto e = std::find_if(l->pool->entries.begin(), l->pool->entries.end(),
                              [&](auto &v) { return v.id == id; });
        if (e == l->pool->entries.end() || juce::JSON::toString(poolEntryToJson(*e), true) != original)
        {
            safe->session.message = "Alternate changed during review; reopen its mapping";
            dismiss(safe->poolReview);
            return;
        }
        safe->session.reviewAndSelectPoolEntry(layerId, id, review->resolved, review->pair);
        dismiss(safe->poolReview);
    };
    poolReview = std::make_unique<PoolPopupOwner>(std::move(panel),
                                                  getScreenBounds().withSizeKeepingCentre(20, 20), look,
                                                  [safe, serial]
                                                  {
                                                      if (safe && safe->poolReviewSerial == serial)
                                                          safe->poolReview.reset();
                                                  });
}
void DesignerView::showPoolPopup(const juce::String &id)
{
    session.selectLayer(id);
    auto serial = ++poolPopoverSerial;
    auto panel = std::make_unique<SourcePoolView>(session, peakPool, preferences, 7);
    auto *view = panel.get();
    panel->expandForPopup();
    panel->setSize(306, panel->preferredHeight());
    juce::Component::SafePointer<SourcePoolView> weak(view);
    juce::Component::SafePointer<DesignerView> safe(this);
    panel->onHeightChanged = [weak]
    {
        if (weak)
            weak->setSize(306, weak->preferredHeight());
    };
    panel->onAdd = [safe, id]
    {
        if (safe)
            safe->choosePoolFiles(id);
    };
    panel->onReview = [safe](const auto &entry)
    {
        if (safe)
            safe->reviewPoolSource(entry);
    };
    panel->onRelocate = [safe, id](const auto &entry)
    {
        if (safe)
            safe->choosePoolFiles(id, entry);
    };
    poolPopover = std::make_unique<PoolPopupOwner>(
        std::move(panel), perform->getScreenBounds().withSizeKeepingCentre(20, 20), look,
        [safe, serial]
        {
            if (safe && safe->poolPopoverSerial == serial)
                safe->poolPopover.reset();
        });
}
} // namespace sp
