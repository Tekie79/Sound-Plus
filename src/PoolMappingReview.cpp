#include "PoolMappingReview.h"
#include <set>
namespace sp
{
PoolMappingReview::PoolMappingReview(SourcePoolEntry e, ChannelLayout destination, int version)
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
        physical.addItem(legacy ? "Analysis Mix (all inputs)" : "All physical channels to Mono mean", 2);
    if (!legacy)
        physical.addItem("Custom subset (up to 16)", 999);
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
        else
        {
            // Length or the first index cannot establish an ordered physical selection.
            std::vector<int> full;
            for (int i = 0; i < entry.source.channels; ++i)
                full.push_back(i);
            preset = entry.routing.inputs == full && entry.source.channels <= 16 ? 1 : 999;
            if (preset != 1)
                for (int p = 0; p < (entry.source.channels + 1) / 2; ++p)
                {
                    std::vector<int> pairInputs{p * 2};
                    if (p * 2 + 1 < entry.source.channels)
                        pairInputs.push_back(p * 2 + 1);
                    if (entry.routing.inputs == pairInputs)
                    {
                        preset = 100 + p;
                        break;
                    }
                }
        }
    }
    physical.setSelectedId(preset, juce::dontSendNotification);
    mapping.addItem("Match named roles", 1);
    mapping.addItem("Physical order (extra inputs muted)", 2);
    if (entry.reviewed && entry.reviewedBus == bus)
        mapping.addItem("Existing reviewed routing", 3);
    mapping.setSelectedId(mapping.getNumItems() == 3 ? 3 : 1, juce::dontSendNotification);
    mapping.setEnabled(!legacy);
    mapping.setTextWhenNothingSelected("Choose mapping for changed inputs");
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
    use.setTooltip("Verify and apply this source. A different source or structural mapping stops owned Live "
                   "playback. An unchanged review preserves playback.");
    setSize(438, 374);
    refresh(true);
}
std::vector<int> PoolMappingReview::inputs() const
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
void PoolMappingReview::refresh(bool resetRoles)
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
                          : physical.getSelectedId() != 1               ? (count == 1   ? "mono-v1"
                                                                           : count == 2 ? "stereo-v1"
                                                                                        : juce::String())
                          : entry.source.layoutOverride.id.isNotEmpty() ? entry.source.layoutOverride.id
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
        bool existing = entry.reviewed && entry.reviewedBus == bus && entry.routing.inputs == selected &&
                        entry.routing.interpretation == layout &&
                        entry.routing.legacyAnalysisMean == resolved.legacyAnalysisMean;
        mapping.setItemEnabled(3, existing);
        if (mapping.getSelectedId() == 3 && !existing)
            mapping.setSelectedId(0, juce::dontSendNotification);
        if (!legacy && mapping.getSelectedId() == 0)
            throw std::runtime_error("Inputs or roles changed. Choose and review a new mapping.");
        if (existing && mapping.getSelectedId() == 3)
            resolved = entry.routing;
        else if (mapping.getSelectedId() == 2 && !legacy)
            for (int i = 0; i < std::min(layout.size(), bus.size()); ++i)
                resolved.routes.push_back({i, i, 0, false, false});
        pair = physical.getSelectedId() == 2 ? -1
               : physical.getSelectedId() >= 100 && physical.getSelectedId() < 200
                   ? physical.getSelectedId() - 100
                   : 0;
        if (existing && mapping.getSelectedId() == 3)
            pair = entry.region.auditionPair;
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
            text +=
                "Legacy stereo audition: " +
                juce::String(pair < 0 ? "all-channel arithmetic Analysis Mix" : "explicit physical pair") +
                ".\n";
        else
        {
            if (resolved.spatial)
                text += "Spatial Panner: azimuth " + juce::String(resolved.azimuth, 6) + ", width " +
                        juce::String(resolved.width, 6) + ", divergence " +
                        juce::String(resolved.divergence, 6) + "\nLFE send " +
                        juce::String(resolved.lfeDb, 6) + " dB, lowpass " +
                        juce::String(resolved.lfeCutoff, 6) + " Hz\n";
            auto matrix = routingMatrix(resolved, bus, 0);
            text += "Design: " + bus.roles.joinIntoString(", ") + "\n";
            for (int out = 0; out < bus.size(); ++out)
                for (int in = 0; in < layout.size(); ++in)
                    if (matrix[out][in] != 0)
                        text += layout.roles[in] + " → " + bus.roles[out] + "  " +
                                juce::String(matrix[out][in], 4) + "\n";
            text += resolved.spatial
                        ? "Existing spatial policy retained; coefficients follow this position.\n"
                        : "Unlisted routes are muted; LFE follows this matrix.\n";
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
void PoolMappingReview::paint(juce::Graphics &g)
{
    g.fillAll(ui::surface);
}
void PoolMappingReview::resized()
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
} // namespace sp
