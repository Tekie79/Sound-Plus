#pragma once
#include "DesignSession.h"
#include "PluginProcessing.h"
#include "UIComponents.h"
namespace sp
{
class HostedRackView final : public juce::Component
{
  public:
    struct Owner
    {
        juce::String document, variation, layer;
        uint64_t generation = 0;
    };
    friend struct LiveCorrectionTestAccess;
    using PresetChooser = std::function<void(bool, std::function<void(juce::File)>)>;
    using EditorInvoker = std::function<juce::var(juce::var, const juce::File &, const juce::File &,
                                                  const std::function<bool()> &)>;
    HostedRackView(DesignSession &s, bool master, juce::File registry, bool embeddedView = false,
                   EditorInvoker invokeEditor = {}, std::vector<juce::File> discoveryRoots = {},
                   bool discoverAudioUnits = true)
        : session(s), isMaster(master), storage(std::move(registry)), embedded(embeddedView),
          editorInvoker(std::move(invokeEditor)), scanRoots(std::move(discoveryRoots)),
          scanAudioUnits(discoverAudioUnits)
    {
        for (auto *c : std::initializer_list<juce::Component *>{
                 &title, &slots, &add, &remove, &up, &down, &bypass, &rackBypass, &transfer, &savePreset,
                 &openPreset, &editor, &parameters, &more, &feedback})
            addAndMakeVisible(c);
        title.setText("Inserts", juce::dontSendNotification);
        title.setFont(juce::FontOptions(12.f));
        title.setColour(juce::Label::textColourId, ui::muted);
        feedback.setFont(juce::FontOptions(11.f));
        feedback.setColour(juce::Label::textColourId, ui::muted);
        slots.setTitle(master ? "Master external insert slots" : "Track external insert slots");
        slots.setTextWhenNothingSelected("No external inserts");
        slots.onChange = [this]
        {
            selected = slotId();
            refresh();
        };
        add.onClick = [this] { showChooser(owner); };
        more.onClick = [this] { showActions(); };
        remove.onClick = [this]
        {
            ++*epoch;
            editSlot("Remove external insert",
                     [](auto &rack, size_t at) { rack.slots.erase(rack.slots.begin() + (int)at); });
        };
        up.onClick = [this] { move(-1); };
        down.onClick = [this] { move(1); };
        bypass.setToggleable(true);
        rackBypass.setToggleable(true);
        bypass.onClick = [this]
        {
            editSlot("Bypass external insert",
                     [](auto &rack, size_t at) { rack.slots[at].bypass = !rack.slots[at].bypass; });
        };
        rackBypass.onClick = [this]
        { change(owner, "Bypass external rack", [](auto &rack) { rack.bypass = !rack.bypass; }); };
        transfer.onClick = [this] { showTransfer(); };
        editor.onClick = [this] { editNative(); };
        parameters.setToggleable(true);
        parameters.onClick = [this]
        {
            expanded = !expanded;
            refresh();
            if (onHeightChange)
                onHeightChange();
        };
        savePreset.onClick = [this] { preset(true); };
        openPreset.onClick = [this] { preset(false); };
        parameterViewport.setViewedComponent(&parameterContent, false);
        parameterViewport.setScrollBarsShown(true, false);
        addAndMakeVisible(parameterViewport);
    }
    ~HostedRackView() override
    {
        for (auto &edit : editors)
            *edit->cancel = true;
        editors.clear();
        ++*epoch;
        ++*availabilityEpoch;
        worker.finish({});
        parameterViewport.setViewedComponent(nullptr, false);
    }
    HostedRack *currentRack()
    {
        return find(owner);
    }
    juce::String slotDescription(int index) const
    {
        return slots.getItemText(index);
    }
    juce::String effectTooltip()
    {
        return slots.getTooltip();
    }
    void selectSlot(const juce::String &id)
    {
        if (selected != id)
        {
            selected = id;
            refresh();
        }
    }
    void chooseEffectFor(const Owner &target, juce::Rectangle<int> anchor)
    {
        showChooser(target, anchor);
    }
    std::function<void()> onHeightChange;
    void bind(const juce::String &layer)
    {
        Owner next{session.document.id, session.document.activeVariation, isMaster ? juce::String() : layer,
                   session.documentGeneration()};
        if (next.document != owner.document || next.variation != owner.variation ||
            next.generation != owner.generation)
            ++*epoch;
        if (next.document != owner.document || next.variation != owner.variation ||
            next.layer != owner.layer || next.generation != owner.generation)
        {
            selected.clear();
            parameterKey.clear();
            parsed.clear();
            feedback.setText({}, juce::dontSendNotification);
        }
        owner = next;
        refresh();
    }
    void refresh()
    {
        reconcileEditors();
        auto *rack = find(owner);
        probeAvailability(rack);
        bool editable = rack && !session.takeBusy() && session.document.activeTake.isEmpty();
        slots.clear(juce::dontSendNotification);
        int index = 0;
        juce::Array<juce::var> identities;
        if (rack)
            for (auto &slot : rack->slots)
                identities.add(parsedSettings(slot)["plugin"]);
        auto names = pluginDisplayNames(identities);
        if (rack)
            for (size_t i = 0; i < rack->slots.size(); ++i)
            {
                auto &slot = rack->slots[i];
                auto settings = parsedSettings(slot);
                auto p = settings["plugin"];
                slots.addItem(names[(int)i] + (slot.bypass ? " · bypassed" : "") +
                                  (availability[slot.id].isNotEmpty() ? " · unavailable" : ""),
                              (int)i + 1);
                if (slot.id == selected)
                    index = (int)i + 1;
            }
        if (!index && rack && !rack->slots.empty())
            index = 1;
        slots.setSelectedId(index, juce::dontSendNotification);
        selected = slotId();
        std::set<juce::String> retained;
        if (rack)
            for (auto &slot : rack->slots)
                retained.insert(slot.id);
        for (auto i = parsed.begin(); i != parsed.end();)
            if (!retained.contains(i->first))
                i = parsed.erase(i);
            else
                ++i;
        bool has = index > 0;
        add.setEnabled(editable && rack->slots.size() < 8);
        remove.setEnabled(editable && has);
        up.setEnabled(editable && index > 1);
        down.setEnabled(editable && has && index < (int)rack->slots.size());
        for (auto *b : {&bypass, &transfer, &savePreset, &openPreset, &editor, &parameters})
            b->setEnabled(editable && has);
        rackBypass.setEnabled(editable && rack && !rack->slots.empty());
        rackBypass.setToggleState(rack && rack->bypass, juce::dontSendNotification);
        bypass.setToggleState(has && !rack->slots[(size_t)index - 1].bypass, juce::dontSendNotification);
        bypass.setTooltip(has && rack->slots[(size_t)index - 1].bypass ? "Enable effect" : "Bypass effect");
        if (has && availability[selected].isNotEmpty())
        {
            editor.setEnabled(false);
            feedback.setText(availability[selected], juce::dontSendNotification);
        }
        else if (feedback.getText().startsWith("Missing plug-in") ||
                 feedback.getText().startsWith("Unsupported architecture"))
            feedback.setText({}, juce::dontSendNotification);
        auto settings = has ? parsedSettings(rack->slots[(size_t)index - 1]) : juce::var();
        auto p = settings["plugin"];
        slots.setTooltip(p["vendor"].toString() + " · " + pluginFormatLabel(p) + "\n" + p["path"].toString() +
                         (has && availability[selected].isNotEmpty() ? "\n" + availability[selected] : "") +
                         (has && rack->slots[(size_t)index - 1].bypass ? "\nBypassed" : ""));
        auto params = settings["parameters"];
        auto key = owner.document + owner.variation + owner.layer +
                   juce::String((juce::int64)owner.generation) + selected +
                   settings["plugin"]["id"].toString() + settings["plugin"]["fingerprint"].toString() + ":" +
                   pluginParameterSchemaSignature(params);
        if (key != parameterKey)
        {
            sliders.clear();
            labels.clear();
            parameterKey = key;
            if (auto *list = params.getArray())
                for (auto &p : *list)
                {
                    if (sliders.size() >= 512)
                        break;
                    auto target = owner;
                    auto id = selected;
                    int parameter = (int)p["index"];
                    auto *label = labels.add(new juce::Label());
                    label->setText(p["name"].toString(), juce::dontSendNotification);
                    label->setColour(juce::Label::textColourId, ui::muted);
                    label->setFont(juce::FontOptions(11.f));
                    parameterContent.addAndMakeVisible(label);
                    auto *v = sliders.add(new juce::Slider());
                    v->setSliderStyle(juce::Slider::LinearHorizontal);
                    v->setTextBoxStyle(juce::Slider::TextBoxRight, false, 65, 22);
                    v->setRange(0, 1, .0001);
                    v->setScrollWheelEnabled(false);
                    v->setTitle("External insert / " + p["name"].toString() + " (normalized)");
                    parameterContent.addAndMakeVisible(v);
                    v->onDragStart = [this] { session.beginGesture(); };
                    v->onDragEnd = [this] { session.endGesture(); };
                    v->onValueChange = [this, slider = juce::Component::SafePointer<juce::Slider>(v), target,
                                        id, parameter, schema = pluginParameterSchemaSignature(params)]
                    {
                        if (updating || !slider)
                            return;
                        double value = slider->getValue();
                        change(target, "Adjust external insert",
                               [&](auto &rack)
                               {
                                   for (auto &slot : rack.slots)
                                       if (slot.id == id)
                                       {
                                           auto s = juce::JSON::parse(slot.settings);
                                           if (pluginParameterSchemaSignature(s["parameters"]) != schema)
                                               return;
                                           if (auto *a = s["parameters"].getArray())
                                               for (auto &p : *a)
                                                   if ((int)p["index"] == parameter)
                                                       p.getDynamicObject()->setProperty("value", value);
                                           slot.settings = juce::JSON::toString(s, true);
                                       }
                               });
                    };
                }
        }
        updating = true;
        for (int i = 0; i < sliders.size(); ++i)
        {
            auto p = params[i];
            auto *v = sliders[i];
            if (!v->isMouseButtonDown())
                v->setValue((double)p["value"], juce::dontSendNotification);
            v->setEnabled(editable);
            v->setTooltip("Normalized host value. Plugin-reported text at preparation: " +
                          p["text"].toString());
        }
        title.setVisible(!embedded);
        slots.setVisible(!embedded && has);
        add.setVisible(!embedded);
        for (auto *b : {&remove, &up, &down, &rackBypass, &transfer, &savePreset, &openPreset})
            b->setVisible(false);
        for (auto *b : {&bypass, &editor, &parameters, &more})
            b->setVisible(has);
        more.setEnabled(has && editable);
        parameters.setEnabled(has);
        parameters.setToggleState(expanded, juce::dontSendNotification);
        parameters.setButtonText(expanded ? "Hide external insert parameters"
                                          : "Show external insert parameters");
        parameterViewport.setVisible(expanded && !sliders.isEmpty());
        updating = false;
        title.setText("Inserts", juce::dontSendNotification);
        feedback.setTooltip(
            "Persistent external chain follows stock inserts; master processing remains separate. "
            "Hosted take recording is not yet qualified.");
        resized();
        reportHeight();
    }
    int preferredHeight() const
    {
        return (embedded ? 0 : 50) + (selected.isNotEmpty() ? 32 : 0) +
               (feedback.getText().isNotEmpty() ? 24 : 0) +
               (selected.isNotEmpty() && expanded && !sliders.isEmpty() ? 132 : 0);
    }
    void resized() override
    {
        auto r = getLocalBounds();
        if (!embedded)
        {
            title.setBounds(r.removeFromTop(22));
            auto row = r.removeFromTop(28);
            slots.setBounds(row.removeFromLeft(std::max(70, row.getWidth() - 30)));
            add.setBounds(row);
        }
        if (selected.isNotEmpty())
        {
            auto row = r.removeFromTop(32);
            for (auto *b : {&bypass, &editor, &parameters, &more})
                b->setBounds(row.removeFromLeft(32).reduced(2));
        }
        feedback.setBounds(r.removeFromTop(feedback.getText().isNotEmpty() ? 24 : 0));
        parameterViewport.setBounds(r);
        int width = std::max(80, r.getWidth() - 14);
        parameterContent.setSize(width, sliders.size() * 46);
        for (int i = 0; i < sliders.size(); ++i)
        {
            labels[i]->setBounds(0, i * 46, width, 17);
            sliders[i]->setBounds(0, i * 46 + 17, width, 28);
        }
    }

  private:
    DesignSession &session;
    bool isMaster, embedded;
    juce::File storage;
    Owner owner;
    int reportedHeight = -1;
    void reportHeight()
    {
        auto height = preferredHeight();
        if (height != reportedHeight)
        {
            reportedHeight = height;
            if (onHeightChange)
                onHeightChange();
        }
    }

    EditorInvoker editorInvoker;
    struct EditorTask
    {
        Owner owner;
        juce::String slot;
        std::shared_ptr<std::atomic<bool>> cancel = std::make_shared<std::atomic<bool>>(false);
        std::shared_ptr<std::atomic<bool>> done = std::make_shared<std::atomic<bool>>(false);
        SerialWorker worker;
    };
    std::vector<std::unique_ptr<EditorTask>> editors;
    void reconcileEditors()
    {
        for (auto &edit : editors)
        {
            auto *rack = find(edit->owner);
            if (!rack || std::none_of(rack->slots.begin(), rack->slots.end(),
                                      [&](auto &slot) { return slot.id == edit->slot; }))
                *edit->cancel = true;
        }
        std::erase_if(editors, [](auto &edit) { return edit->done->load(); });
    }
    SerialWorker worker;
    std::vector<juce::File> scanRoots;
    bool scanAudioUnits = true;
    std::shared_ptr<std::atomic<uint64_t>> epoch = std::make_shared<std::atomic<uint64_t>>(0);
    juce::Label title, feedback;
    void showFeedback(const Owner &target, const juce::String &message)
    {
        if (target.document == owner.document && target.variation == owner.variation &&
            target.layer == owner.layer && target.generation == owner.generation)
        {
            feedback.setText(message, juce::dontSendNotification);
            feedback.setTooltip(message);
            resized();
            reportHeight();
        }
    }
    juce::ComboBox slots;
    IconButton add{"Add external insert", ActionIcon::addEffect},
        remove{"Remove external insert", ActionIcon::trash}, up{"Move external insert up", ActionIcon::up},
        down{"Move external insert down", ActionIcon::down},
        bypass{"Effect enabled", ActionIcon::effectEnabled},
        rackBypass{"Bypass external rack", ActionIcon::processing},
        transfer{"Copy or move external insert", ActionIcon::duplicate},
        savePreset{"Save insert preset", ActionIcon::save},
        openPreset{"Open insert preset", ActionIcon::open},
        editor{"Open or focus live editor", ActionIcon::settings},
        parameters{"Show external insert parameters", ActionIcon::chevron},
        more{"Effect actions", ActionIcon::more};
    juce::Viewport parameterViewport;
    juce::Component parameterContent;
    juce::OwnedArray<juce::Slider> sliders;
    juce::OwnedArray<juce::Label> labels;
    std::map<juce::String, std::pair<juce::String, juce::var>> parsed;
    juce::var parsedSettings(const HostedSlot &slot)
    {
        auto &cache = parsed[slot.id];
        if (cache.first != slot.settings)
        {
            cache.first = slot.settings;
            cache.second = juce::JSON::parse(slot.settings);
        }
        return cache.second;
    }
    std::map<juce::String, juce::String> availability;
    std::shared_ptr<std::atomic<uint64_t>> availabilityEpoch = std::make_shared<std::atomic<uint64_t>>(0);
    juce::String availabilityKey;
    double availabilityChecked = 0;
    bool availabilityPending = false;
    void probeAvailability(HostedRack *rack)
    {
        struct Probe
        {
            juce::String id, path, architecture;
        };
        std::vector<Probe> probes;
        juce::String key =
            owner.document + owner.variation + owner.layer + juce::String((juce::int64)owner.generation);
        if (rack)
            for (auto &slot : rack->slots)
            {
                auto plugin = parsedSettings(slot)["plugin"];
                auto path = plugin["format"].toString() == "AudioUnit" ? plugin["bundle"].toString()
                                                                       : plugin["path"].toString();
                probes.push_back({slot.id, path, plugin["architecture"].toString()});
                key += slot.id + path + plugin["fingerprint"].toString();
            }
        auto now = juce::Time::getMillisecondCounterHiRes();
        if (key != availabilityKey)
        {
            availabilityKey = key;
            availability.clear();
            availabilityPending = false;
            ++*availabilityEpoch;
            availabilityChecked = 0;
        }
        if (availabilityPending || now - availabilityChecked < 2000 || probes.empty())
            return;
        availabilityChecked = now;
        availabilityPending = true;
        auto gate = availabilityEpoch;
        auto serial = ++*gate;
        juce::Component::SafePointer<HostedRackView> safe(this);
        if (!worker.submit(
                [safe, probes, gate, serial]
                {
                    std::map<juce::String, juce::String> result;
                    for (auto &p : probes)
                    {
                        if (gate->load() != serial)
                            return;
                        if (p.path.isEmpty() || !juce::File(p.path).exists())
                            result[p.id] = "Missing plug-in / saved state retained";
                        else if (p.architecture.isNotEmpty() && !p.architecture.contains("arm64"))
                            result[p.id] = "Unsupported architecture / no bridge";
                    }
                    juce::MessageManager::callAsync(
                        [safe, result, gate, serial]
                        {
                            if (!safe || gate->load() != serial)
                                return;
                            safe->availabilityPending = false;
                            safe->availability = result;
                            safe->refresh();
                        });
                }))
            availabilityPending = false;
    }
    juce::String selected, parameterKey;
    bool updating = false, expanded = false;
    std::unique_ptr<juce::FileChooser> fileChooser;
    HostedRack *find(const Owner &o)
    {
        if (o.generation != session.documentGeneration() || o.document != session.document.id ||
            o.variation != session.document.activeVariation)
            return nullptr;
        if (o.layer.isEmpty())
            return isMaster ? &session.document.activeMix().hosted : nullptr;
        for (auto &l : session.document.activeMix().layers)
            if (l.id == o.layer)
                return &l.hosted;
        return nullptr;
    }
    ChannelLayout bus(const Owner &o)
    {
        auto &mix = session.document.activeMix();
        if (mix.layout.id.isEmpty())
            return channelLayout("stereo-v1");
        if (o.layer.isEmpty())
            return mix.layout;
        for (auto &l : mix.layers)
            if (l.id == o.layer)
                return l.routing.interpretation;
        return {};
    }
    juce::String slotId()
    {
        auto *r = find(owner);
        int i = slots.getSelectedId() - 1;
        return r && juce::isPositiveAndBelow(i, (int)r->slots.size()) ? r->slots[(size_t)i].id
                                                                      : juce::String();
    }
    void change(const Owner &o, const juce::String &label, std::function<void(HostedRack &)> fn)
    {
        if (!find(o) || session.takeBusy() || session.document.activeTake.isNotEmpty())
            return;
        session.edit(label,
                     [&](auto &)
                     {
                         if (auto *rack = find(o))
                             fn(*rack);
                     });
        refresh();
    }
    void editSlot(const juce::String &name, std::function<void(HostedRack &, size_t)> fn)
    {
        auto id = selected;
        change(owner, name,
               [&](auto &r)
               {
                   for (size_t i = 0; i < r.slots.size(); ++i)
                       if (r.slots[i].id == id)
                       {
                           fn(r, i);
                           break;
                       }
               });
    }
    void move(int step)
    {
        editSlot("Reorder external insert",
                 [=](auto &r, size_t i)
                 {
                     auto j = std::clamp((int)i + step, 0, (int)r.slots.size() - 1);
                     std::swap(r.slots[i], r.slots[(size_t)j]);
                 });
    }
    juce::File helper()
    {
        auto override = juce::SystemStats::getEnvironmentVariable("SOUNDPLUS_PLUGIN_HELPER", {});
        if (override.isNotEmpty())
            return juce::File(override);
        auto exe = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
        auto file = exe.getSiblingFile("SoundPlusPlugin");
        if (!file.existsAsFile())
            file = exe.getParentDirectory().getParentDirectory().getChildFile("Helpers/SoundPlusPlugin");
        return file;
    }
    void load(Owner target, juce::var plugin)
    {
        if (!find(target))
            return;
        auto layout = bus(target);
        auto rate = session.document.sampleRate;
        auto exe = helper();
        auto folder = storage.getParentDirectory().getChildFile("insert-work");
        auto serial = ++*epoch;
        auto gate = epoch;
        juce::Component::SafePointer<HostedRackView> safe(this);
        feedback.setText("Loading effect...", juce::dontSendNotification);
        worker.submit(
            [safe, target, plugin, layout, rate, exe, folder, serial, gate]
            {
                juce::var settings;
                juce::String failure;
                try
                {
                    auto *o = new juce::DynamicObject();
                    o->setProperty("mode", "describe");
                    o->setProperty("plugin", plugin);
                    o->setProperty("rate", rate);
                    o->setProperty("block", 512);
                    o->setProperty("layout", layoutJson(layout));
                    settings = juce::var(o);
                    auto result = PluginWorker(exe).call(settings, folder, 20000,
                                                         [=] { return gate->load() != serial; });
                    for (auto key :
                         {"state", "stateHash", "parameters", "parameterBaseline", "program", "programName"})
                        o->setProperty(key, result[key]);
                }
                catch (const std::exception &e)
                {
                    failure = e.what();
                }
                juce::MessageManager::callAsync(
                    [safe, target, settings, failure, gate, serial]
                    {
                        if (!safe || gate->load() != serial || !safe->find(target))
                            return;
                        if (failure.isNotEmpty())
                        {
                            safe->showFeedback(target, failure);
                            return;
                        }
                        const auto newSlot = juce::Uuid().toString();
                        safe->change(target, "Add external insert",
                                     [&](auto &rack)
                                     {
                                         if (rack.slots.size() < 8)
                                             rack.slots.push_back(
                                                 {newSlot, juce::JSON::toString(settings, true), false});
                                     });
                        safe->session.openLiveEditor(newSlot);
                        safe->showFeedback(target, "Live");
                    });
            });
    }

  public:
    struct Chooser final : juce::Component, juce::ListBoxModel
    {
        juce::TextEditor search, details;
        juce::ListBox available{"Installed effects", this};
        ui::TextActionButton scan{"Rescan"}, retry{"Retry selected"}, insert{"Add effect"},
            attention{"Needs attention"};
        IconButton location{"Add VST3 folder", ActionIcon::folder};
        juce::Label status, requested;
        juce::Array<juce::var> choices;
        juce::var snapshot;
        juce::String wantedSelection, wantedFormat, wantedCanonical;
        bool refreshingRows = false, wantedBundleOnly = false;
        ChannelLayout expected;
        std::function<void(juce::var)> choose;
        ui::LookAndFeel look;
        ~Chooser() override
        {
            available.setModel(nullptr);
            setLookAndFeel(nullptr);
        }
        Chooser(ChannelLayout layout, const juce::String &ownerName) : expected(layout)
        {
            setLookAndFeel(&look);
            setSize(560, 420);
            for (auto *c : std::initializer_list<juce::Component *>{&search, &available, &scan, &retry,
                                                                    &insert, &attention, &location, &status,
                                                                    &requested, &details})
                addAndMakeVisible(c);
            search.setTextToShowWhenEmpty("Search name, vendor, format or reason", ui::muted);
            search.setTitle("Search installed effects");
            search.onTextChange = [this] { populate(); };
            requested.setText(ownerName + " · " + layoutName(expected) + " main bus",
                              juce::dontSendNotification);
            requested.setColour(juce::Label::textColourId, ui::muted);
            requested.setFont(juce::FontOptions(12.f));
            attention.setClickingTogglesState(true);
            attention.onClick = [this] { populate(); };
            details.setMultiLine(true);
            details.setReadOnly(true);
            details.setTitle("Selected effect diagnostics");
            available.setRowHeight(44);
            available.setColour(juce::ListBox::backgroundColourId, ui::background);
            insert.onClick = [this] { loadSelected(); };
            insert.setEnabled(false);
            retry.setEnabled(false);
        }
        int getNumRows() override
        {
            return choices.size();
        }
        juce::String getNameForRow(int row) override
        {
            if (!juce::isPositiveAndBelow(row, choices.size()))
                return {};
            auto p = choices[row];
            return p["displayName"].toString() +
                   (p["status"].toString() == "Available"
                        ? juce::String()
                        : juce::String::fromUTF8(" · ") + p["status"].toString());
        }
        void paintListBoxItem(int row, juce::Graphics &g, int width, int height, bool selected) override
        {
            if (!juce::isPositiveAndBelow(row, choices.size()))
                return;
            auto p = choices[row];
            if (selected)
            {
                g.setColour(ui::accent.withAlpha(.15f));
                g.fillRect(0, 0, width, height);
            }
            g.setColour(ui::text);
            g.setFont(juce::FontOptions(12.f, juce::Font::bold));
            bool needsAttention = p["status"].toString() != "Available";
            g.drawText(p["displayName"].toString(), 10, needsAttention ? 2 : 12, width - 20, 20,
                       juce::Justification::centredLeft, true);
            if (needsAttention)
            {
                g.setColour(ui::muted);
                g.setFont(juce::FontOptions(11.f));
                g.drawText(p["status"].toString(), 10, 22, width - 20, 18, juce::Justification::centredLeft,
                           true);
            }
        }
        void selectedRowsChanged(int row) override
        {
            if (!juce::isPositiveAndBelow(row, choices.size()))
            {
                insert.setEnabled(false);
                retry.setEnabled(false);
                details.clear();
                return;
            }
            auto p = choices[row];
            if (!refreshingRows)
            {
                wantedSelection = pluginCandidateSelectionKey(p);
                wantedFormat = p["plugin"]["format"].toString();
                wantedCanonical = p["canonicalIdentity"].toString();
                wantedBundleOnly = p["plugin"]["id"].toString().isEmpty();
            }
            insert.setEnabled((bool)p["canInsert"]);
            retry.setEnabled(!(bool)p["canInsert"]);
            juce::StringArray layouts;
            if (auto *a = p["layouts"].getArray())
                for (auto &l : *a)
                    layouts.add(l.toString());
            auto text = p["vendor"].toString() + " · " + p["format"].toString() + " · " +
                        p["architecture"].toString() + "\n" + p["status"].toString() +
                        " / stage: " + p["stage"].toString() + "\n" + p["reason"].toString() +
                        "\nRequested: " + layoutName(expected) + " · Supported: " +
                        (layouts.isEmpty() ? "not established" : layouts.joinIntoString(", ")) +
                        "\nLast attempt: " + p["lastAttempt"].toString() + "\n" + p["path"].toString();
            if (p["diagnostics"]["stderr"].toString().isNotEmpty())
                text += "\nLog: " + p["diagnostics"]["stderr"].toString();
            details.setText(text, false);
        }
        void listBoxItemDoubleClicked(int, const juce::MouseEvent &) override
        {
            loadSelected();
        }
        void returnKeyPressed(int) override
        {
            loadSelected();
        }
        void loadSelected()
        {
            int row = available.getSelectedRow();
            if (juce::isPositiveAndBelow(row, choices.size()) && (bool)choices[row]["canInsert"] &&
                pluginCandidateSelectionKey(choices[row]) == wantedSelection && choose)
                choose(choices[row]["plugin"]);
        }
        juce::String selectedPath() const
        {
            int row = available.getSelectedRow();
            return juce::isPositiveAndBelow(row, choices.size()) ? choices[row]["path"].toString()
                                                                 : juce::String();
        }
        void resized() override
        {
            search.setBounds(12, 10, 536, 28);
            requested.setBounds(12, 40, 340, 22);
            attention.setBounds(388, 40, 160, 24);
            available.setBounds(12, 70, 536, 144);
            details.setBounds(12, 220, 536, 104);
            scan.setBounds(12, 332, 100, 28);
            retry.setBounds(118, 332, 130, 28);
            location.setBounds(254, 332, 30, 28);
            insert.setBounds(416, 332, 132, 28);
            status.setBounds(12, 366, 536, 42);
            status.setFont(juce::FontOptions(11.f));
            status.setColour(juce::Label::textColourId, ui::muted);
        }
        void populate()
        {
            refreshingRows = true;
            choices = pluginCandidateRows(snapshot, expected, search.getText(), attention.getToggleState());
            if (!attention.getToggleState())
                for (int i = choices.size(); --i >= 0;)
                    if (!(bool)choices[i]["canInsert"])
                        choices.remove(i);
            available.updateContent();
            int selected = -1;
            for (int i = 0; i < choices.size(); ++i)
                if (pluginCandidateSelectionKey(choices[i]) == wantedSelection)
                    selected = i;
            if (selected < 0 && wantedBundleOnly)
                for (int i = 0; i < choices.size(); ++i)
                    if (choices[i]["canonicalIdentity"].toString() == wantedCanonical &&
                        choices[i]["plugin"]["format"].toString() == wantedFormat)
                    {
                        selected = i;
                        break;
                    }
            if (wantedSelection.isEmpty() && !choices.isEmpty())
                selected = 0;
            if (selected >= 0)
                available.selectRow(selected);
            else
                available.deselectAllRows();
            refreshingRows = false;
            selectedRowsChanged(selected);
            available.repaint();
        }
    };
    Chooser *openChooser() const
    {
        return chooser.getComponent();
    }

  private:
    juce::Component::SafePointer<Chooser> chooser;

  public:
    std::unique_ptr<Chooser> createChooserFor(const Owner &target)
    {
        juce::String ownerName = isMaster ? "Master" : "Track";
        if (!isMaster)
            for (auto &l : session.document.activeMix().layers)
                if (l.id == target.layer)
                    ownerName = l.name;
        auto content = std::make_unique<Chooser>(bus(target), ownerName);
        chooser = content.get();
        juce::Component::SafePointer<HostedRackView> safe(this);
        auto view = chooser;
        content->choose = [safe, target, view](auto p)
        {
            if (safe)
                safe->load(target, p);
            if (view)
                if (auto *box = view->template findParentComponentOfClass<juce::CallOutBox>())
                    box->dismiss();
        };
        content->scan.onClick = [safe, view]
        {
            if (safe && view)
                safe->scanChooser(false);
        };
        content->retry.onClick = [safe, view]
        {
            if (safe && view)
                safe->scanChooser(true, {}, view->selectedPath());
        };
        content->location.onClick = [safe, view]
        {
            if (!safe || !view)
                return;
            safe->fileChooser = std::make_unique<juce::FileChooser>(
                "Add VST3 location", juce::File::getSpecialLocation(juce::File::userHomeDirectory), "*");
            safe->fileChooser->launchAsync(juce::FileBrowserComponent::openMode |
                                               juce::FileBrowserComponent::canSelectDirectories,
                                           [safe, view](const auto &fc)
                                           {
                                               if (safe && view && fc.getResult() != juce::File())
                                                   safe->scanChooser(false, {fc.getResult()});
                                           });
        };
        scanChooser(false);
        return content;
    }

  private:
    void showChooser(const Owner &target, juce::Rectangle<int> anchor = {})
    {
        juce::CallOutBox::launchAsynchronously(createChooserFor(target),
                                               anchor.isEmpty() ? add.getScreenBounds() : anchor, nullptr);
    }
    void scanChooser(bool retryQuarantine, std::vector<juce::File> extraRoots = {},
                     const juce::String &targetPath = {})
    {
        auto *panel = chooser.getComponent();
        if (!panel)
            return;
        auto registry = storage;
        auto exe = helper();
        auto folder = storage.getParentDirectory().getChildFile("insert-work");
        auto serial = ++*epoch;
        auto gate = epoch;
        auto configuredRoots = scanRoots;
        bool includeAU = scanAudioUnits;
        juce::Component::SafePointer<HostedRackView> safe(this);
        juce::Component::SafePointer<Chooser> view(panel);
        panel->status.setText(retryQuarantine ? "Retrying failed effects..."
                                              : "Checking installed effects...",
                              juce::dontSendNotification);
        worker.submit(
            [safe, view, registry, exe, folder, retryQuarantine, extraRoots, targetPath, serial, gate,
             configuredRoots, includeAU]
            {
                juce::var snapshot;
                juce::String failure;
                try
                {
                    PluginCatalog catalog(registry);
                    snapshot = catalog.snapshot();
                    if (snapshot["entries"].isArray())
                        juce::MessageManager::callAsync(
                            [view, snapshot, gate, serial]
                            {
                                if (view && gate->load() == serial)
                                {
                                    view->snapshot = snapshot;
                                    view->populate();
                                    view->status.setText(
                                        "Cached effects / checking installed registrations...",
                                        juce::dontSendNotification);
                                }
                            });
                    {
                        auto roots = configuredRoots.empty() ? defaultVst3Locations() : configuredRoots;
                        if (auto *saved = snapshot["roots"].getArray())
                            for (auto &v : *saved)
                            {
                                juce::File f(v.toString());
                                if (std::find(roots.begin(), roots.end(), f) == roots.end())
                                    roots.push_back(f);
                            }
                        for (auto &root : extraRoots)
                            if (std::find(roots.begin(), roots.end(), root) == roots.end())
                                roots.push_back(root);
                        PluginWorker worker(exe);
                        snapshot = catalog.scan(
                            worker, roots, retryQuarantine, [=] { return gate->load() != serial; }, {},
                            includeAU, targetPath);
                    }
                }
                catch (const std::exception &e)
                {
                    failure = e.what();
                }
                juce::MessageManager::callAsync(
                    [safe, view, snapshot, failure, gate, serial]
                    {
                        if (!safe || !view || gate->load() != serial)
                            return;
                        view->snapshot = snapshot;
                        view->populate();
                        view->status.setText(failure.isEmpty() ? "Scan complete. Select a candidate for its "
                                                                 "exact layout and failure details."
                                                               : failure,
                                             juce::dontSendNotification);
                    });
            });
    }
    void showTransfer()
    {
        auto source = owner;
        auto id = selected;
        juce::PopupMenu menu;
        std::vector<juce::String> ids{juce::String()};
        juce::StringArray names{"Master"};
        for (auto &l : session.document.activeMix().layers)
        {
            ids.push_back(l.id);
            names.add(l.name);
        }
        for (int i = 0; i < names.size(); ++i)
        {
            menu.addItem(i * 2 + 1, "Copy to " + names[i]);
            menu.addItem(i * 2 + 2, "Move to " + names[i]);
        }
        juce::Component::SafePointer<HostedRackView> safe(this);
        menu.showMenuAsync(
            juce::PopupMenu::Options().withTargetComponent(more),
            [safe, source, id, ids](int choice)
            {
                if (!safe || !safe->find(source) || choice <= 0 || (size_t)((choice - 1) / 2) >= ids.size())
                    return;
                auto layer = ids[(size_t)((choice - 1) / 2)];
                bool move = choice % 2 == 0;
                safe->session.withLiveState(
                    [safe, source, id, layer, move]
                    {
                        if (!safe || !safe->find(source))
                            return;
                        safe->session.edit(move ? "Move external insert" : "Copy external insert",
                                           [&](auto &d)
                                           {
                                               auto *from = safe->find(source);
                                               auto *to = &d.activeMix().hosted;
                                               if (layer.isNotEmpty())
                                               {
                                                   to = nullptr;
                                                   for (auto &l : d.activeMix().layers)
                                                       if (l.id == layer)
                                                           to = &l.hosted;
                                               }
                                               if (!from || !to || from == to || to->slots.size() >= 8)
                                                   return;
                                               for (size_t i = 0; i < from->slots.size(); ++i)
                                                   if (from->slots[i].id == id)
                                                   {
                                                       auto slot = from->slots[i];
                                                       if (!move)
                                                           slot.id = juce::Uuid().toString();
                                                       to->slots.push_back(slot);
                                                       if (move)
                                                           from->slots.erase(from->slots.begin() + (int)i);
                                                       break;
                                                   }
                                           });
                    });
                safe->refresh();
            });
    }
    PresetChooser presetChooser;
    juce::PopupMenu actionMenu()
    {
        auto target = owner;
        auto id = selected;
        juce::Component::SafePointer<HostedRackView> safe(this);
        juce::PopupMenu menu;
        menu.addItem(juce::PopupMenu::Item("Save preset...")
                         .setID(1)
                         .setEnabled(savePreset.isEnabled())
                         .setAction(
                             [safe, target, id]
                             {
                                 if (safe)
                                     safe->preset(true, target, id);
                             }));
        menu.addItem(juce::PopupMenu::Item("Load preset...")
                         .setID(2)
                         .setEnabled(openPreset.isEnabled())
                         .setAction(
                             [safe, target, id]
                             {
                                 if (safe)
                                     safe->preset(false, target, id);
                             }));
        menu.addItem(3, "Copy / move…", transfer.isEnabled());
        menu.addSeparator();
        menu.addItem(4, "Move earlier", up.isEnabled());
        menu.addItem(5, "Move later", down.isEnabled());
        if (auto *rack = find(target); rack && rack->slots.size() > 1)
            menu.addItem(6, "Bypass plug-in effects", rackBypass.isEnabled(), rackBypass.getToggleState());
        menu.addSeparator();
        menu.addItem(7, "Remove effect", remove.isEnabled());
        return menu;
    }
    void showActions()
    {
        const auto target = owner;
        const auto id = selected;
        auto menu = actionMenu();
        juce::Component::SafePointer<HostedRackView> safe(this);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(more),
                           [safe, target, id](int choice)
                           {
                               if (choice == 1 || choice == 2)
                                   return;
                               if (!safe || safe->owner.document != target.document ||
                                   safe->owner.variation != target.variation ||
                                   safe->owner.layer != target.layer ||
                                   safe->owner.generation != target.generation || safe->selected != id)
                                   return;
                               switch (choice)
                               {
                               case 1:
                                   safe->preset(true, target, id);
                                   break;
                               case 2:
                                   safe->preset(false, target, id);
                                   break;
                               case 3:
                                   safe->showTransfer();
                                   break;
                               case 4:
                                   safe->move(-1);
                                   break;
                               case 5:
                                   safe->move(1);
                                   break;
                               case 6:
                                   safe->rackBypass.onClick();
                                   break;
                               case 7:
                                   safe->remove.onClick();
                                   break;
                               }
                           });
    }
    void editNative()
    {
        if (!editorInvoker)
        {
            if (session.openLiveEditor(selected))
                showFeedback(owner, "Live");
            else
                showFeedback(owner, "Live editor unavailable");
            return;
        }
        auto target = owner;
        auto id = selected;
        auto *r = find(target);
        if (!r)
            return;
        juce::String original;
        for (auto &slot : r->slots)
            if (slot.id == id)
                original = slot.settings;
        if (original.isEmpty())
            return;
        auto settings = juce::JSON::parse(original);
        auto layout = bus(target);
        settings.getDynamicObject()->setProperty("mode", "editor");
        settings.getDynamicObject()->setProperty("layout", layoutJson(layout));
        settings.getDynamicObject()->setProperty("rate", session.document.sampleRate);
        settings.getDynamicObject()->setProperty("block", 512);
        auto exe = helper().getSiblingFile("SoundPlusPluginEditor.app/Contents/MacOS/SoundPlusPluginEditor");
        auto folder = storage.getParentDirectory().getChildFile("insert-work");
        reconcileEditors();
        if (editors.size() >= 8)
        {
            showFeedback(target, "Close an editor first (maximum eight)");
            return;
        }
        for (auto &edit : editors)
            if (edit->owner.document == target.document && edit->owner.variation == target.variation &&
                edit->owner.layer == target.layer && edit->owner.generation == target.generation &&
                edit->slot == id)
            {
                showFeedback(target, "This effect's editor is already open");
                return;
            }
        auto task = std::make_unique<EditorTask>();
        task->owner = target;
        task->slot = id;
        auto cancel = task->cancel, done = task->done;
        auto invoke = editorInvoker;
        juce::Component::SafePointer<HostedRackView> safe(this);
        showFeedback(target, "Editor open / apply on close");
        task->worker.submit(
            [safe, target, id, original, settings, exe, folder, cancel, done, invoke]
            {
                juce::var edited;
                juce::String failure, recovery;
                PluginWorker child(exe);
                try
                {
                    auto cancelled = [cancel] { return cancel->load(); };
                    edited = invoke ? invoke(settings, exe, folder, cancelled)
                                    : child.call(settings, folder, 0, cancelled);
                    recovery = edited["editorCheckpoint"].toString();
                }
                catch (const std::exception &e)
                {
                    failure = e.what();
                    recovery = child.lastDiagnostics["editorCheckpoint"].toString();
                }
                *done = true;
                juce::MessageManager::callAsync(
                    [safe, target, id, original, edited, failure, recovery, cancel]
                    {
                        if (!safe)
                            return;
                        auto report = [&](const juce::String &text)
                        {
                            auto message = text + (recovery.isNotEmpty() ? " / recovery preset: " + recovery
                                                                         : juce::String());
                            safe->session.message = message;
                            safe->showFeedback(target, message);
                            if (safe->session.onChange)
                                safe->session.onChange();
                        };
                        if (cancel->load() || !safe->find(target))
                        {
                            report("Editor owner was removed or changed; completion not applied");
                            return;
                        }
                        if (failure.isNotEmpty())
                        {
                            report(failure);
                            return;
                        }
                        juce::Process::makeForegroundProcess();
                        if (auto *window = safe->getTopLevelComponent())
                            window->toFront(true);
                        bool applied = false;
                        safe->change(target, "Apply isolated plugin editor",
                                     [&](auto &rack)
                                     {
                                         for (auto &slot : rack.slots)
                                             if (slot.id == id && slot.settings == original)
                                             {
                                                 auto s = juce::JSON::parse(original);
                                                 for (auto key :
                                                      {"state", "stateHash", "parameters",
                                                       "parameterBaseline", "program", "programName"})
                                                     s.getDynamicObject()->setProperty(key, edited[key]);
                                                 slot.settings = juce::JSON::toString(s, true);
                                                 applied = true;
                                             }
                                     });
                        if (applied)
                            safe->showFeedback(target, "Editor state applied");
                        else
                            report("Insert state changed; editor state retained in insert-work");
                    });
            });
        editors.push_back(std::move(task));
    }
    void preset(bool writing)
    {
        preset(writing, owner, selected);
    }
    void preset(bool writing, Owner target, juce::String id)
    {
        auto *rack = find(target);
        if (!rack)
            return;
        juce::Component::SafePointer<HostedRackView> safe(this);
        auto chosen = [safe, target, id, writing](const juce::File &result)
        {
            if (!safe || result == juce::File())
                return;
            auto file = writing ? result.withFileExtension("soundplus-insert") : result;
            auto persist = [safe, target, id, writing, file]
            {
                if (!safe)
                    return;
                if (!safe->find(target))
                {
                    safe->session.message = "Preset cancelled: owner changed or removed";
                    safe->refresh();
                    return;
                }
                juce::String settings;
                for (auto &slot : safe->find(target)->slots)
                    if (slot.id == id)
                        settings = slot.settings;
                if (settings.isEmpty())
                {
                    safe->showFeedback(target, "Preset cancelled: insert removed");
                    return;
                }
                auto gate = safe->epoch;
                auto serial = ++*gate;
                safe->worker.submit(
                    [safe, target, id, settings, writing, file, gate, serial]
                    {
                        juce::String value, error;
                        try
                        {
                            if (writing)
                            {
                                auto v = juce::var(new juce::DynamicObject());
                                v.getDynamicObject()->setProperty("version", 1);
                                v.getDynamicObject()->setProperty("settings", juce::JSON::parse(settings));
                                saveProcessingJson(file, v);
                            }
                            else
                            {
                                auto v = loadProcessingJson(file);
                                if ((int)v["version"] != 1 || !v["settings"].isObject())
                                    throw std::runtime_error("Invalid insert preset");
                                auto old = juce::JSON::parse(settings);
                                if (v["settings"]["plugin"]["id"] != old["plugin"]["id"] ||
                                    v["settings"]["plugin"]["format"] != old["plugin"]["format"])
                                    throw std::runtime_error("Preset belongs to another processor/format");
                                value = juce::JSON::toString(v["settings"], true);
                            }
                        }
                        catch (const std::exception &e)
                        {
                            error = e.what();
                        }
                        juce::MessageManager::callAsync(
                            [safe, target, id, value, error, writing, gate, serial]
                            {
                                if (!safe || gate->load() != serial || !safe->find(target))
                                    return;
                                if (error.isNotEmpty())
                                {
                                    safe->showFeedback(target, error);
                                    return;
                                }
                                if (!writing)
                                    safe->change(target, "Load insert preset",
                                                 [&](auto &r)
                                                 {
                                                     for (auto &s : r.slots)
                                                         if (s.id == id)
                                                             s.settings = value;
                                                 });
                                safe->showFeedback(target, writing ? "Preset saved" : "Preset restored");
                            });
                    });
            };
            if (writing)
                safe->session.withLiveState(std::move(persist));
            else
                persist();
        };
        if (presetChooser)
            presetChooser(writing, std::move(chosen));
        else
        {
            fileChooser =
                std::make_unique<juce::FileChooser>(writing ? "Save insert preset" : "Open insert preset",
                                                    storage.getParentDirectory(), "*.soundplus-insert");
            fileChooser->launchAsync(
                juce::FileBrowserComponent::canSelectFiles |
                    (writing ? juce::FileBrowserComponent::saveMode : juce::FileBrowserComponent::openMode),
                [chosen = std::move(chosen)](const auto &fc) { chosen(fc.getResult()); });
        }
    }
};
} // namespace sp
