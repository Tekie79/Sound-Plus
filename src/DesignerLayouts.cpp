#include "DesignerView.h"
#include "ChannelDetails.h"
#include <sstream>
namespace sp
{
namespace
{
class MatrixPicture final : public juce::Component
{
  public:
    LayerRouting routing;
    ChannelLayout bus;
    double pan = 0;
    void paint(juce::Graphics &g) override
    {
        g.fillAll(ui::background);
        g.setFont(juce::FontOptions(11.f));
        ChannelMatrix matrix{};
        try
        {
            matrix = routingMatrix(routing, bus, pan);
            if (routing.spatial)
            {
                for (size_t i = 0; i < (size_t)routing.interpretation.size(); ++i)
                {
                    auto g =
                        spatialGains(bus,
                                     routing.azimuth + (routing.interpretation.size() == 2
                                                            ? (i == 0 ? -routing.width : routing.width) * .5
                                                            : 0),
                                     routing.divergence);
                    for (int c = 0; c < bus.size(); ++c)
                        matrix[c][i] = g[c] * (routing.interpretation.size() == 2 ? .70710678f : 1.f);
                    int lfe = bus.roles.indexOf("LFE");
                    if (lfe >= 0 && routing.lfeDb > -90)
                        matrix[lfe][i] = (float)juce::Decibels::decibelsToGain(routing.lfeDb) /
                                         routing.interpretation.size();
                }
            }
        }
        catch (...)
        {
        }
        for (int c = 0; c < bus.size(); ++c)
        {
            g.setColour(ui::muted);
            g.drawText(bus.roles[c], 82 + c * 48, 0, 48, 26, juce::Justification::centred);
        }
        for (int i = 0; i < routing.interpretation.size(); ++i)
        {
            g.setColour(ui::muted);
            g.drawText(routing.legacyAnalysisMean ? juce::String("Mean [all]")
                                                  : routing.interpretation.roles[i] + " [" +
                                                        juce::String(routing.inputs[(size_t)i] + 1) + "]",
                       4, 28 + i * 28, 76, 28, juce::Justification::left);
            for (int c = 0; c < bus.size(); ++c)
            {
                auto v = std::abs(matrix[c][i]) < .0005f ? 0.f : matrix[c][i];
                auto cell = juce::Rectangle<int>(82 + c * 48, 28 + i * 28, 46, 26);
                g.setColour(v == 0 ? ui::surface : ui::accent.withAlpha(.15f));
                g.fillRoundedRectangle(cell.toFloat(), 3);
                g.setColour(v == 0 ? ui::muted : ui::text);
                g.drawText(v == 0 ? juce::String::fromUTF8("—") : juce::String(v, 2), cell,
                           juce::Justification::centred);
            }
        }
    }
};
struct RoutingPanel final : juce::Component, private juce::Timer
{
    void timerCallback() override
    {
        refresh();
    }
    DesignerView &view;
    juce::String id;
    juce::ComboBox interpretation, subset, input, output;
    juce::ToggleButton spatial{"Spatial Panner"}, invert{"Invert polarity"}, muted{"Mute route"};
    juce::Slider angle, width, divergence, lfe, cutoff, trim;
    juce::Label title, hint;
    juce::TextButton apply{"Set route"}, defaults{"Role routing"}, byIndex{"Route by index"};
    juce::Viewport viewport;
    MatrixPicture picture;
    std::vector<ChannelLayout> options;
    bool updating = false, gestureActive = false;
    juce::String listSignature, boundContext, editorSignature;
    double loadedTrim = 0, displayedTrim = 0;
    bool loadedInvert = false, loadedMute = true;
    std::array<juce::Label, 5> labels;
    DesignLayer *layer()
    {
        for (auto &l : view.session.document.activeMix().layers)
            if (l.id == id)
                return &l;
        return nullptr;
    }
    const DesignLayer *display()
    {
        for (auto &l : view.session.performanceMix().layers)
            if (l.id == id)
                return &l;
        return nullptr;
    }
    const DesignLayer *desired() const
    {
        auto &mix = view.session.performanceMix();
        for (auto &l : mix.layers)
            if (l.id == view.session.document.selectedLayer)
                return &l;
        return mix.layers.empty() ? nullptr : &mix.layers.front();
    }
    juce::String context(const DesignLayer &l) const
    {
        const auto &d = view.session.document;
        return d.id + ":" + d.activeVariation + ":" + d.activeTake + ":" + l.id + ":" + l.source.id + ":" +
               juce::String(l.source.revision) + ":" + juce::String(l.source.size) + ":" +
               juce::String(l.source.modified) + ":" + juce::String(l.source.inode) + ":" +
               view.session.performanceMix().layout.id;
    }
    bool sameContext() const
    {
        auto *l = desired();
        return l && context(*l) == boundContext;
    }
    struct RouteState
    {
        double gain = 0;
        bool invert = false, mute = true, valid = false;
        juce::String signature, kind;
    };
    RouteState routeState()
    {
        RouteState state;
        auto *l = display();
        auto bus = view.session.performanceMix().layout;
        int i = input.getSelectedId() - 1, o = output.getSelectedId() - 1;
        if (!l || i < 0 || i >= l->routing.interpretation.size() || o < 0 || o >= bus.size())
            return state;
        state.valid = true;
        state.kind = l->routing.routes.empty() ? "Implicit role route" : "Absent route (zero)";
        auto found = std::find_if(l->routing.routes.begin(), l->routing.routes.end(),
                                  [&](auto &r) { return r.input == i && r.output == o; });
        if (found != l->routing.routes.end())
        {
            state.gain = found->gainDb;
            state.invert = found->invert;
            state.mute = found->mute;
            state.kind = "Explicit route";
        }
        else if (l->routing.routes.empty())
        {
            try
            {
                auto coefficient = routingMatrix(l->routing, bus, l->pan)[o][i];
                if (coefficient != 0)
                {
                    state.gain = juce::Decibels::gainToDecibels((double)std::abs(coefficient));
                    state.invert = coefficient < 0;
                    state.mute = false;
                }
                else
                    state.kind = "Absent implicit route (zero)";
            }
            catch (...)
            {
                state.kind = "Unresolved role route (zero)";
            }
        }
        state.signature = boundContext + ":" + l->routing.interpretation.id + ":" +
                          juce::String((int)l->routing.legacyAnalysisMean);
        for (auto physical : l->routing.inputs)
            state.signature += ":source" + juce::String(physical);
        state.signature += ":" + juce::String(i) + ":" + juce::String(o) + ":" +
                           juce::String((int)l->routing.spatial) + ":" + state.kind + ":" +
                           juce::String(state.gain, 17) + ":" + juce::String((int)state.invert) + ":" +
                           juce::String((int)state.mute);
        return state;
    }
    void bindRoute(bool force = false)
    {
        auto state = routeState();
        if (!state.valid || (!force && state.signature == editorSignature))
            return;
        // Timer refresh preserves a deliberate draft while its route/model context
        // is unchanged. Selection, Undo/Redo or an external route edit rebinds it.
        editorSignature = state.signature;
        loadedTrim = state.gain;
        loadedInvert = state.invert;
        loadedMute = state.mute;
        trim.setValue(loadedTrim, juce::dontSendNotification);
        displayedTrim = trim.getValue();
        invert.setToggleState(loadedInvert, juce::dontSendNotification);
        muted.setToggleState(loadedMute, juce::dontSendNotification);
        apply.setTooltip(state.kind + ". Set route applies deliberate edits to this input/output only.");
        trim.setTooltip("Current route " + juce::String(loadedTrim, 6) +
                        " dB. Unedited precision is preserved.");
    }
    void edit(const char *label, std::function<void(DesignLayer &)> f)
    {
        if (!sameContext())
        {
            refresh();
            return;
        }
        view.session.edit(label,
                          [&](auto &d)
                          {
                              for (auto &l : d.activeMix().layers)
                                  if (l.id == id)
                                      f(l);
                          });
        refresh();
    }
    void paint(juce::Graphics &g) override
    {
        g.fillAll(ui::surface);
    }
    void refresh()
    {
        updating = true;
        auto *wanted = desired();
        if (wanted && context(*wanted) != boundContext)
        {
            id = wanted->id;
            boundContext = context(*wanted);
            listSignature.clear();
            editorSignature.clear();
        }
        auto *l = display();
        if (!l)
        {
            setEnabled(false);
            updating = false;
            return;
        }
        setEnabled(true);
        auto bus = view.session.performanceMix().layout;
        int desiredHeight = std::min(540, 300 + std::max(1, l->routing.interpretation.size()) * 28);
        if (getHeight() != desiredHeight)
            setSize(620, desiredHeight);
        auto signature = boundContext + ":" + l->routing.interpretation.id + ":" + bus.id + ":" +
                         juce::String((int)l->routing.legacyAnalysisMean);
        for (auto c : l->routing.inputs)
            signature += ":" + juce::String(c);
        if (signature != listSignature)
        {
            editorSignature.clear();
            options.clear();
            interpretation.clear(juce::dontSendNotification);
            input.clear(juce::dontSendNotification);
            output.clear(juce::dontSendNotification);
            int n = l->routing.interpretation.size();
            subset.clear(juce::dontSendNotification);
            if (l->source.channels <= 16)
                subset.addItem("All " + juce::String(l->source.channels) + " source channels", 1);
            for (int c = 0; c < l->source.channels; c += 2)
                subset.addItem("Source " + juce::String(c + 1) + " / " +
                                   juce::String(std::min(c + 2, l->source.channels)),
                               c / 2 + 2);
            if (l->source.channels > 16)
                for (int first = 0; first < l->source.channels; first += 16)
                    subset.addItem("Source " + juce::String(first + 1) + " - " +
                                       juce::String(std::min(first + 16, l->source.channels)),
                                   100 + first / 16);
            subset.setTextWhenNothingSelected(l->routing.legacyAnalysisMean
                                                  ? "Analysis: all-channel mean"
                                                  : "Selected " + juce::String(n) + " of " +
                                                        juce::String(l->source.channels));
            for (auto p : namedLayouts())
                if (p.size() == n)
                    options.push_back(p);
            if (n > 0)
                options.push_back(discreteLayout(n));
            for (size_t i = 0; i < options.size(); ++i)
            {
                interpretation.addItem(layoutName(options[i]), (int)i + 1);
                if (options[i] == l->routing.interpretation)
                    interpretation.setSelectedId((int)i + 1, juce::dontSendNotification);
            }
            interpretation.setTextWhenNothingSelected(layoutName(l->routing.interpretation));
            for (int i = 0; i < n; ++i)
                input.addItem(l->routing.legacyAnalysisMean
                                  ? "Mono [all-channel mean]"
                                  : l->routing.interpretation.roles[i] + " [source " +
                                        juce::String(l->routing.inputs[(size_t)i] + 1) + "]",
                              i + 1);
            for (int i = 0; i < bus.size(); ++i)
                output.addItem(bus.roles[i], i + 1);
            input.setSelectedId(1, juce::dontSendNotification);
            output.setSelectedId(1, juce::dontSendNotification);
            listSignature = signature;
        }
        bindRoute();
        picture.routing = l->routing;
        picture.bus = bus;
        picture.pan = l->pan;
        picture.setSize(90 + bus.size() * 48, 30 + l->routing.interpretation.size() * 28);
        picture.repaint();
        title.setText(l->name + juce::String::fromUTF8(" · ") +
                          (l->routing.needsSelection ? juce::String("Choose source channels")
                                                     : layoutBadge(l->routing.interpretation)) +
                          juce::String::fromUTF8(" → ") + layoutName(bus),
                      juce::dontSendNotification);
        hint.setText("Matrix coefficients: output columns / interpreted inputs [source index]. Mute changes "
                     "exports. Monitor solo is separate.",
                     juce::dontSendNotification);
        spatial.setToggleState(l->routing.spatial, juce::dontSendNotification);
        angle.setValue(l->routing.azimuth, juce::dontSendNotification);
        width.setValue(l->routing.width, juce::dontSendNotification);
        divergence.setValue(l->routing.divergence, juce::dontSendNotification);
        lfe.setValue(l->routing.lfeDb, juce::dontSendNotification);
        cutoff.setValue(l->routing.lfeCutoff, juce::dontSendNotification);
        bool frozen = view.session.takeBusy() || view.session.document.activeTake.isNotEmpty();
        for (auto *c : std::initializer_list<juce::Component *>{
                 &interpretation, &subset, &spatial, &divergence, &lfe, &cutoff, &input, &output, &trim,
                 &invert, &muted, &apply, &defaults, &byIndex})
            c->setEnabled(!frozen);
        interpretation.setEnabled(!frozen && !l->routing.needsSelection && !l->routing.legacyAnalysisMean);
        apply.setEnabled(!frozen && !l->routing.needsSelection);
        spatial.setEnabled(!frozen && !l->routing.needsSelection && horizontalLayout(bus) &&
                           l->routing.interpretation.size() <= 2);
        bool moving =
            view.session.document.activeTake.isEmpty() &&
            (!view.session.takeBusy() || view.session.takeState == DesignSession::TakeState::recording);
        angle.setEnabled(moving && l->routing.spatial);
        width.setEnabled(moving && l->routing.spatial && l->routing.interpretation.size() == 2);
        lfe.setEnabled(!frozen && l->routing.spatial && bus.roles.contains("LFE"));
        cutoff.setEnabled(lfe.isEnabled());
        if (l->routing.spatial)
            for (auto *c :
                 std::initializer_list<juce::Component *>{&input, &output, &trim, &invert, &muted, &apply})
                c->setEnabled(false);
        hint.setText(l->routing.spatial ? juce::String::fromUTF8(
                                              "Spatial target coefficients · 10ms smoothing. Disable Spatial "
                                              "Panner for matrix edits. LFE sends use the declared lowpass.")
                     : l->routing.needsSelection
                         ? "Choose an explicit source subset; wider sources are never truncated."
                         : "Matrix coefficients: output columns / inputs [source index]. Mute changes "
                           "exports; Monitor solo is separate.",
                     juce::dontSendNotification);
        updating = false;
    }
    RoutingPanel(DesignerView &v, juce::String target) : view(v), id(std::move(target))
    {
        auto *l = display();
        if (!l)
            return;
        for (auto *c : std::initializer_list<juce::Component *>{
                 &title, &hint, &interpretation, &subset, &spatial, &angle, &width, &divergence, &lfe,
                 &cutoff, &input, &output, &trim, &invert, &muted, &apply, &defaults, &byIndex, &viewport})
            addAndMakeVisible(c);
        title.setFont(juce::FontOptions(15.f));
        hint.setFont(juce::FontOptions(11.f));
        hint.setColour(juce::Label::textColourId, ui::muted);
        interpretation.setTitle("Input interpretation override");
        subset.setTitle("Explicit source channel selection");
        input.setTitle("Route input");
        output.setTitle("Route output");
        int n = l->routing.interpretation.size();
        for (auto p : namedLayouts())
            if (p.size() == n)
                options.push_back(p);
        if (n > 0)
            options.push_back(discreteLayout(n));
        for (size_t i = 0; i < options.size(); ++i)
        {
            interpretation.addItem(layoutName(options[i]), (int)i + 1);
            if (options[i] == l->routing.interpretation)
                interpretation.setSelectedId((int)i + 1, juce::dontSendNotification);
        }
        interpretation.setTextWhenNothingSelected(layoutName(l->routing.interpretation));
        interpretation.onChange = [this]
        {
            if (!updating)
            {
                auto chosen = options[(size_t)interpretation.getSelectedId() - 1];
                edit("Input interpretation",
                     [&](auto &l)
                     {
                         l.routing.interpretation = chosen;
                         l.routing.routes.clear();
                     });
            }
        };
        subset.onChange = [this]
        {
            if (updating)
                return;
            int chosen = subset.getSelectedId();
            edit("Input channel selection",
                 [&](auto &l)
                 {
                     l.routing.inputs.clear();
                     l.routing.legacyAnalysisMean = false;
                     l.routing.needsSelection = false;
                     if (chosen == 1)
                     {
                         for (int i = 0; i < l.source.channels; ++i)
                             l.routing.inputs.push_back(i);
                         l.routing.interpretation = l.source.layoutOverride.id.isNotEmpty()
                                                        ? l.source.layoutOverride
                                                        : l.source.detectedChannels;
                     }
                     else if (chosen >= 100)
                     {
                         int first = (chosen - 100) * 16;
                         for (int c = first; c < std::min(first + 16, l.source.channels); ++c)
                             l.routing.inputs.push_back(c);
                         l.routing.interpretation = discreteLayout((int)l.routing.inputs.size());
                     }
                     else
                     {
                         int a = (chosen - 2) * 2;
                         l.routing.inputs.push_back(a);
                         if (a + 1 < l.source.channels)
                             l.routing.inputs.push_back(a + 1);
                         l.routing.interpretation =
                             channelLayout(l.routing.inputs.size() == 1 ? "mono-v1" : "stereo-v1");
                     }
                     l.routing.routes.clear();
                     l.routing.spatial = false;
                 });
        };
        auto configure = [&](juce::Slider &s, const char *name, double low, double high, double step)
        {
            s.setSliderStyle(juce::Slider::LinearHorizontal);
            s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 70, 24);
            s.setRange(low, high, step);
            s.setName(name);
            s.setTitle(name);
        };
        configure(angle, "Spatial azimuth (degrees)", -180, 180, .1);
        angle.setTextValueSuffix(juce::String::fromUTF8("°"));
        configure(width, "Stereo image width (degrees)", 0, 360, .1);
        width.setTextValueSuffix(juce::String::fromUTF8("°"));
        configure(divergence, "Spatial divergence", 0, 1, .01);
        configure(lfe, "LFE send (dB), -90 is off", -90, 0, .1);
        lfe.setTextValueSuffix(" dB");
        configure(cutoff, "LFE lowpass (Hz)", 20, 200, 1);
        cutoff.setTextValueSuffix(" Hz");
        configure(trim, "Route trim (dB)", -90, 24, .1);
        trim.setTextValueSuffix(" dB");
        angle.onDragStart = width.onDragStart = [this]
        {
            gestureActive = true;
            view.session.beginGesture();
        };
        angle.onDragEnd = width.onDragEnd = [this]
        {
            gestureActive = false;
            view.session.endGesture();
        };
        angle.onValueChange = [this]
        {
            if (!updating)
                edit("Spatial motion", [&](auto &l) { l.routing.azimuth = angle.getValue(); });
        };
        width.onValueChange = [this]
        {
            if (!updating)
                edit("Spatial motion", [&](auto &l) { l.routing.width = width.getValue(); });
        };
        spatial.onClick = [this]
        { edit("Spatial policy", [&](auto &l) { l.routing.spatial = spatial.getToggleState(); }); };
        divergence.onValueChange = [this]
        {
            if (!updating)
                edit("Spatial divergence", [&](auto &l) { l.routing.divergence = divergence.getValue(); });
        };
        lfe.onValueChange = [this]
        {
            if (!updating)
                edit("LFE send", [&](auto &l) { l.routing.lfeDb = lfe.getValue(); });
        };
        cutoff.onValueChange = [this]
        {
            if (!updating)
                edit("LFE filter", [&](auto &l) { l.routing.lfeCutoff = cutoff.getValue(); });
        };
        for (int i = 0; i < n; ++i)
            input.addItem(l->routing.legacyAnalysisMean
                              ? "Mono [all-channel mean]"
                              : l->routing.interpretation.roles[i] + " [source " +
                                    juce::String(l->routing.inputs[(size_t)i] + 1) + "]",
                          i + 1);
        for (int i = 0; i < view.session.document.activeMix().layout.size(); ++i)
            output.addItem(view.session.document.activeMix().layout.roles[i], i + 1);
        input.setSelectedId(1);
        output.setSelectedId(1);
        input.onChange = output.onChange = [this]
        {
            if (updating)
                return;
            if (!sameContext())
                refresh();
            else
                bindRoute(true);
        };
        apply.onClick = [this]
        {
            if (!sameContext() || routeState().signature != editorSignature || view.session.takeBusy() ||
                view.session.document.activeTake.isNotEmpty())
            {
                refresh();
                return;
            }
            bool trimEdited = std::abs(trim.getValue() - displayedTrim) > 1e-9;
            if (!trimEdited && invert.getToggleState() == loadedInvert &&
                muted.getToggleState() == loadedMute)
                return; // Inspection never materializes a default unity route.
            double gain = trimEdited ? trim.getValue() : loadedTrim;
            ChannelRoute selected{input.getSelectedId() - 1, output.getSelectedId() - 1, gain,
                                  invert.getToggleState(), muted.getToggleState()};
            edit("Channel route",
                 [&](auto &l)
                 {
                     if (l.routing.routes.empty())
                     {
                         try
                         {
                             auto m =
                                 routingMatrix(l.routing, view.session.document.activeMix().layout, l.pan);
                             for (int o = 0; o < view.session.document.activeMix().layout.size(); ++o)
                                 for (int i = 0; i < l.routing.interpretation.size(); ++i)
                                     if (m[o][i] != 0)
                                         l.routing.routes.push_back(
                                             {i, o, juce::Decibels::gainToDecibels((double)std::abs(m[o][i])),
                                              m[o][i] < 0, false});
                         }
                         catch (...)
                         {
                         }
                     }
                     int i = input.getSelectedId() - 1, o = output.getSelectedId() - 1;
                     auto it = std::find_if(l.routing.routes.begin(), l.routing.routes.end(),
                                            [&](auto &r) { return r.input == i && r.output == o; });
                     if (it == l.routing.routes.end())
                         l.routing.routes.push_back(selected);
                     else
                         *it = selected;
                 });
        };
        defaults.onClick = [this]
        {
            edit("Role routing",
                 [](auto &l)
                 {
                     l.routing.routes.clear();
                     l.routing.spatial = false;
                 });
        };
        byIndex.onClick = [this]
        {
            edit("Explicit index routing",
                 [&](auto &l)
                 {
                     l.routing.routes.clear();
                     for (int i = 0; i < std::min(l.routing.interpretation.size(),
                                                  view.session.document.activeMix().layout.size());
                          ++i)
                         l.routing.routes.push_back({i, i, 0, false, false});
                     l.routing.spatial = false;
                 });
        };
        viewport.setViewedComponent(&picture, false);
        int labelIndex = 0;
        for (auto name : {"Azimuth", "Stereo width", "Divergence", "LFE send (off at -90 dB)", "LFE lowpass"})
        {
            auto &label = labels[(size_t)labelIndex++];
            addAndMakeVisible(label);
            label.setText(name, juce::dontSendNotification);
            label.setFont(juce::FontOptions(10.f));
            label.setColour(juce::Label::textColourId, ui::muted);
        }
        setOpaque(true);
        setSize(620, std::min(540, 300 + std::max(1, n) * 28));
        refresh();
        startTimerHz(10);
    }
    ~RoutingPanel() override
    {
        if (gestureActive)
            view.session.endGesture();
    }
    bool keyPressed(const juce::KeyPress &k) override
    {
        if (k == juce::KeyPress::escapeKey && gestureActive)
        {
            gestureActive = false;
            view.session.cancelGesture();
            refresh();
            return true;
        }
        return false;
    }
    void resized() override
    {
        title.setBounds(12, 8, getWidth() - 24, 26);
        interpretation.setBounds(12, 40, 240, 28);
        subset.setBounds(264, 40, getWidth() - 276, 28);
        spatial.setBounds(12, 80, 160, 26);
        labels[0].setBounds(174, 69, 205, 14);
        labels[1].setBounds(390, 69, 218, 14);
        angle.setBounds(174, 81, 205, 26);
        width.setBounds(390, 81, 218, 26);
        labels[2].setBounds(12, 108, 180, 14);
        labels[3].setBounds(210, 108, 190, 14);
        labels[4].setBounds(410, 108, 198, 14);
        divergence.setBounds(12, 120, 180, 26);
        lfe.setBounds(210, 120, 190, 26);
        cutoff.setBounds(410, 120, 198, 26);
        input.setBounds(12, 146, 170, 28);
        output.setBounds(190, 146, 90, 28);
        trim.setBounds(290, 146, 190, 28);
        apply.setBounds(488, 146, 120, 28);
        invert.setBounds(12, 180, 150, 26);
        muted.setBounds(170, 180, 100, 26);
        defaults.setBounds(290, 180, 145, 26);
        byIndex.setBounds(446, 180, 162, 26);
        hint.setBounds(12, 214, getWidth() - 24, 35);
        viewport.setBounds(12, 254, getWidth() - 24, getHeight() - 266);
    }
};
} // namespace
void DesignerView::layoutPopup()
{
    juce::PopupMenu menu;
    auto choices = namedLayouts();
    for (int i = 1; i <= 16; ++i)
        choices.push_back(discreteLayout(i));
    auto current = designLayout(session.document);
    for (size_t i = 0; i < choices.size(); ++i)
        menu.addItem((int)i + 1, juce::String::fromUTF8("Design · ") + layoutName(choices[i]),
                     !session.takeBusy() && session.document.activeTake.isEmpty(), current == choices[i]);
    menu.addSeparator();
    menu.addItem(100, "Routing / Spatial Panner",
                 !session.performanceMix().layers.empty() && !session.performanceMix().layout.id.isEmpty());
    menu.addItem(101, "Per-channel waveforms", !session.performanceMix().layers.empty());
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&layoutControl),
                       [safe = juce::Component::SafePointer<DesignerView>(this), choices](int result)
                       {
                           if (!safe)
                               return;
                           if (result == 100)
                           {
                               safe->routingPopup();
                               return;
                           }
                           if (result == 101)
                           {
                               safe->channelPopup();
                               return;
                           }
                           if (result > 0 && result <= (int)choices.size())
                               safe->session.edit("Design layout (legacy live conversion)",
                                                  [&](auto &d)
                                                  {
                                                      setMixLayout(d.activeMix(),
                                                                   choices[(size_t)result - 1]);
                                                      d.renderSettings.layout = "same";
                                                  });
                       });
}
std::unique_ptr<juce::Component> DesignerView::makeRoutingPanel()
{
    auto &mix = session.performanceMix();
    if (mix.layers.empty() || mix.layout.id.isEmpty())
        return {};
    auto it = std::find_if(mix.layers.begin(), mix.layers.end(),
                           [&](auto &l) { return l.id == session.document.selectedLayer; });
    return std::make_unique<RoutingPanel>(*this, it == mix.layers.end() ? mix.layers.front().id : it->id);
}
void DesignerView::routingPopup()
{
    auto panel = makeRoutingPanel();
    if (!panel)
        return;
    juce::CallOutBox::launchAsynchronously(
        std::move(panel),
        routingButton.isShowing()
            ? getLocalArea(routingButton.getParentComponent(), routingButton.getBounds())
            : layoutControl.getBounds(),
        this);
}
void DesignerView::channelPopup()
{
    const DesignLayer *l = nullptr;
    for (auto &v : session.performanceMix().layers)
        if (v.id == session.document.selectedLayer)
            l = &v;
    if (!l && !session.performanceMix().layers.empty())
        l = &session.performanceMix().layers.front();
    if (!l)
        return;
    showChannelDetails(*this, getLocalArea(channelDetail.getParentComponent(), channelDetail.getBounds()),
                       peakPool, l->source,
                       session.performanceMix().layout.id.isEmpty() || l->routing.legacyAnalysisMean
                           ? ChannelLayout{}
                           : l->routing.interpretation,
                       session.performanceMix().layout.id.isEmpty() ? std::vector<int>{} : l->routing.inputs,
                       5, l->region.startSample, l->region.endSample, l->reverse);
}
void DesignerView::monitorPopup()
{
    juce::PopupMenu menu;
    menu.addItem(1, "Stereo monitor downmix / default pair", true);
    menu.addItem(2, "Clear monitor solo", true);
    auto layout = designLayout(session.document);
    for (int i = 0; i < layout.size(); ++i)
        menu.addItem(100 + i, juce::String::fromUTF8("Monitor solo · ") + layout.roles[i], true);
    auto status = audio.status();
    int available =
        status["deviceOutputChannels"].isArray() ? std::min(16, status["deviceOutputChannels"].size()) : 2;
    juce::PopupMenu count;
    for (int n = 1; n <= available; ++n)
        count.addItem(500 + n, "Enable " + juce::String(n) + " hardware outputs", !session.takeBusy());
    menu.addSubMenu("Hardware outputs", count);
    for (int device = 0; device < available; ++device)
    {
        juce::PopupMenu output;
        output.addItem(1000 + device * 32, "Mute device " + juce::String(device + 1));
        for (int i = 0; i < layout.size(); ++i)
            output.addItem(1001 + device * 32 + i, "Source " + layout.roles[i]);
        menu.addSubMenu("Assign device " + juce::String(device + 1), output);
    }
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&monitorControl),
                       [safe = juce::Component::SafePointer<DesignerView>(this)](int r)
                       {
                           if (!safe)
                               return;
                           if (r == 1)
                           {
                               for (int c = 0; c < 16; ++c)
                                   safe->audio.monitorChannel(c, -2);
                               safe->audio.monitorSolo(-1);
                           }
                           if (r == 2)
                               safe->audio.monitorSolo(-1);
                           if (r >= 100 && r < 116)
                               safe->audio.monitorSolo(r - 100);
                           if (r >= 1000 && r < 1512)
                           {
                               int x = r - 1000;
                               safe->audio.monitorChannel(x / 32, x % 32 - 1);
                           }
                           if (r > 500 && r <= 516)
                               safe->audio.deviceChannels(r - 500);
                       });
}
} // namespace sp
