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
    HostedRackView(DesignSession &s, bool master, juce::File registry, bool embeddedView = false)
        : session(s), isMaster(master), storage(std::move(registry)), embedded(embeddedView)
    {
        for (auto *c : std::initializer_list<juce::Component *>{
                 &title, &slots, &add, &remove, &up, &down, &bypass, &rackBypass, &transfer, &savePreset,
                 &openPreset, &editor, &parameters, &more, &feedback})
            addAndMakeVisible(c);
        title.setText("External / after stock", juce::dontSendNotification);
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
        return slots.getItemText(index).fromFirstOccurrenceOf(" | ", false, false);
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
        auto *rack = find(owner);
        probeAvailability(rack);
        bool editable = rack && !session.takeBusy() && session.document.activeTake.isEmpty();
        slots.clear(juce::dontSendNotification);
        int index = 0;
        if (rack)
            for (size_t i = 0; i < rack->slots.size(); ++i)
            {
                auto &slot = rack->slots[i];
                auto settings = parsedSettings(slot);
                auto p = settings["plugin"];
                slots.addItem(juce::String((int)i + 1) + " | " + p["name"].toString() + " / " +
                                  pluginFormatLabel(p) +
                                  (availability[slot.id].isNotEmpty() ? " (unavailable)" : "") +
                                  (slot.bypass ? " (bypassed)" : ""),
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
        auto params = settings["parameters"];
        auto key = owner.document + owner.variation + owner.layer +
                   juce::String((juce::int64)owner.generation) + selected +
                   settings["plugin"]["id"].toString() + settings["plugin"]["fingerprint"].toString() + ":" +
                   juce::String(params.size());
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
                    v->onValueChange = [this, v, target, id, parameter]
                    {
                        if (updating)
                            return;
                        double value = v->getValue();
                        change(target, "Adjust external insert",
                               [&](auto &rack)
                               {
                                   for (auto &slot : rack.slots)
                                       if (slot.id == id)
                                       {
                                           auto s = juce::JSON::parse(slot.settings);
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
        title.setText(isMaster ? "External / summed bus"
                      : session.document.activeMix().layout.id.isEmpty()
                          ? "External / legacy stereo"
                          : "External / " + layoutName(bus(owner)) + " source",
                      juce::dontSendNotification);
        feedback.setTooltip(
            "Persistent external chain follows stock inserts; master processing remains separate. "
            "Hosted take recording is not yet qualified.");
        resized();
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
    SerialWorker worker;
    std::shared_ptr<std::atomic<uint64_t>> epoch = std::make_shared<std::atomic<uint64_t>>(0);
    juce::Label title, feedback;
    void showFeedback(const Owner &target, const juce::String &message)
    {
        if (target.document == owner.document && target.variation == owner.variation &&
            target.layer == owner.layer && target.generation == owner.generation)
        {
            feedback.setText(message, juce::dontSendNotification);
            feedback.setTooltip(message);
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
        editor{"Editor (apply state when closed)", ActionIcon::settings},
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
                    for (auto key : {"state", "stateHash", "parameters", "program", "programName"})
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
                        safe->change(target, "Add external insert",
                                     [&](auto &rack)
                                     {
                                         if (rack.slots.size() < 8)
                                             rack.slots.push_back({juce::Uuid().toString(),
                                                                   juce::JSON::toString(settings, true),
                                                                   false});
                                     });
                        safe->showFeedback(target, "Ready / persistent chain");
                    });
            });
    }
    struct Chooser final : juce::Component
    {
        juce::TextEditor search;
        juce::ComboBox available;
        ui::TextActionButton scan{"Rescan"}, retry{"Retry failed"};
        IconButton location{"Add VST3 folder", ActionIcon::folder};
        juce::Label status;
        std::vector<juce::var> choices;
        juce::var snapshot;
        std::function<void(juce::var)> choose;
        ui::LookAndFeel look;
        ~Chooser() override
        {
            setLookAndFeel(nullptr);
        }
        Chooser()
        {
            setLookAndFeel(&look);
            setSize(480, 160);
            for (auto *c : std::initializer_list<juce::Component *>{&search, &available, &scan, &retry,
                                                                    &location, &status})
                addAndMakeVisible(c);
            search.setTextToShowWhenEmpty("Search effect, vendor or category", ui::muted);
            search.onTextChange = [this] { populate(); };
            available.onChange = [this]
            {
                int i = available.getSelectedId() - 1;
                if (juce::isPositiveAndBelow(i, (int)choices.size()) && choose)
                    choose(choices[(size_t)i]);
            };
        }
        void resized() override
        {
            search.setBounds(10, 10, 460, 28);
            available.setBounds(10, 46, 460, 30);
            scan.setBounds(10, 84, 120, 28);
            retry.setBounds(138, 84, 120, 28);
            location.setBounds(266, 84, 30, 28);
            status.setBounds(10, 118, 460, 32);
        }
        void populate()
        {
            choices.clear();
            available.clear(juce::dontSendNotification);
            if (auto *entries = snapshot["entries"].getArray())
                for (auto &entry : *entries)
                    if (auto *plugins = entry["plugins"].getArray())
                        for (auto &p : *plugins)
                            if (p["reason"].toString().isEmpty() &&
                                (p["name"].toString() + p["vendor"].toString() + p["category"].toString() +
                                 pluginFormatLabel(p))
                                    .containsIgnoreCase(search.getText()))
                                choices.push_back(p);
            std::sort(choices.begin(), choices.end(),
                      [](auto &a, auto &b)
                      {
                          return (pluginEffectGroup(a) + a["name"].toString())
                                     .compareIgnoreCase(pluginEffectGroup(b) + b["name"].toString()) < 0;
                      });
            juce::String group;
            for (size_t i = 0; i < choices.size(); ++i)
            {
                auto &p = choices[i];
                auto g = pluginEffectGroup(p);
                if (g != group)
                {
                    available.addSectionHeading(g);
                    group = g;
                }
                available.addItem(p["name"].toString() + " / " + pluginFormatLabel(p), (int)i + 1);
            }
        }
    };
    juce::Component::SafePointer<Chooser> chooser;
    void showChooser(const Owner &target, juce::Rectangle<int> anchor = {})
    {
        auto content = std::make_unique<Chooser>();
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
        content->scan.onClick = [safe]
        {
            if (safe)
                safe->scanChooser(false);
        };
        content->retry.onClick = [safe]
        {
            if (safe)
                safe->scanChooser(true);
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
        juce::CallOutBox::launchAsynchronously(std::move(content),
                                               anchor.isEmpty() ? add.getScreenBounds() : anchor, nullptr);
    }
    void scanChooser(bool retryQuarantine, std::vector<juce::File> extraRoots = {})
    {
        auto *panel = chooser.getComponent();
        if (!panel)
            return;
        auto registry = storage;
        auto exe = helper();
        auto folder = storage.getParentDirectory().getChildFile("insert-work");
        auto serial = ++*epoch;
        auto gate = epoch;
        juce::Component::SafePointer<HostedRackView> safe(this);
        juce::Component::SafePointer<Chooser> view(panel);
        panel->status.setText(retryQuarantine ? "Retrying failed effects..."
                                              : "Checking installed effects...",
                              juce::dontSendNotification);
        worker.submit(
            [safe, view, registry, exe, folder, retryQuarantine, extraRoots, serial, gate]
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
                        auto roots = defaultVst3Locations();
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
                            worker, roots, retryQuarantine, [=] { return gate->load() != serial; }, {}, true);
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
                        view->status.setText(
                            failure.isEmpty()
                                ? "Choose an effect; actual source bus is negotiated before insertion"
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
                safe->refresh();
            });
    }
    void showActions()
    {
        auto target = owner;
        auto id = selected;
        juce::PopupMenu menu;
        menu.addItem(1, "Save preset…", savePreset.isEnabled());
        menu.addItem(2, "Load preset…", openPreset.isEnabled());
        menu.addItem(3, "Copy / move…", transfer.isEnabled());
        menu.addSeparator();
        menu.addItem(4, "Move earlier", up.isEnabled());
        menu.addItem(5, "Move later", down.isEnabled());
        if (auto *rack = find(target); rack && rack->slots.size() > 1)
            menu.addItem(6, "Bypass plug-in effects", rackBypass.isEnabled(), rackBypass.getToggleState());
        menu.addSeparator();
        menu.addItem(7, "Remove effect", remove.isEnabled());
        juce::Component::SafePointer<HostedRackView> safe(this);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(more),
                           [safe, target, id](int choice)
                           {
                               if (!safe || safe->owner.document != target.document ||
                                   safe->owner.variation != target.variation ||
                                   safe->owner.layer != target.layer ||
                                   safe->owner.generation != target.generation || safe->selected != id)
                                   return;
                               switch (choice)
                               {
                               case 1:
                                   safe->preset(true);
                                   break;
                               case 2:
                                   safe->preset(false);
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
        auto gate = epoch;
        auto serial = ++*gate;
        juce::Component::SafePointer<HostedRackView> safe(this);
        feedback.setText("Editor open / apply on close", juce::dontSendNotification);
        worker.submit(
            [safe, target, id, original, settings, exe, folder, gate, serial]
            {
                juce::var edited;
                juce::String failure;
                try
                {
                    edited = PluginWorker(exe).call(settings, folder, 20 * 60 * 1000,
                                                    [=] { return gate->load() != serial; });
                }
                catch (const std::exception &e)
                {
                    failure = e.what();
                }
                juce::MessageManager::callAsync(
                    [safe, target, id, original, edited, failure, gate, serial]
                    {
                        if (!safe || gate->load() != serial || !safe->find(target))
                            return;
                        if (failure.isNotEmpty())
                        {
                            safe->showFeedback(target, failure);
                            return;
                        }
                        juce::Process::makeForegroundProcess();
                        if (auto *window = safe->getTopLevelComponent())
                            window->toFront(true);
                        bool applied = false;
                        safe->change(
                            target, "Apply isolated plugin editor",
                            [&](auto &rack)
                            {
                                for (auto &slot : rack.slots)
                                    if (slot.id == id && slot.settings == original)
                                    {
                                        auto settings = juce::JSON::parse(original);
                                        for (auto key :
                                             {"state", "stateHash", "parameters", "program", "programName"})
                                            settings.getDynamicObject()->setProperty(key, edited[key]);
                                        slot.settings = juce::JSON::toString(settings, true);
                                        applied = true;
                                    }
                            });
                        safe->showFeedback(target,
                                           applied ? "Editor state applied"
                                                   : "Insert changed; editor result retained in insert-work");
                    });
            });
    }
    void preset(bool writing)
    {
        auto target = owner;
        auto id = selected;
        auto *rack = find(target);
        if (!rack)
            return;
        juce::String settings;
        for (auto &s : rack->slots)
            if (s.id == id)
                settings = s.settings;
        fileChooser =
            std::make_unique<juce::FileChooser>(writing ? "Save insert preset" : "Open insert preset",
                                                storage.getParentDirectory(), "*.soundplus-insert");
        juce::Component::SafePointer<HostedRackView> safe(this);
        fileChooser->launchAsync(
            juce::FileBrowserComponent::canSelectFiles |
                (writing ? juce::FileBrowserComponent::saveMode : juce::FileBrowserComponent::openMode),
            [safe, target, id, settings, writing](const auto &fc)
            {
                if (!safe || !safe->find(target) || fc.getResult() == juce::File())
                    return;
                auto file = writing ? fc.getResult().withFileExtension("soundplus-insert") : fc.getResult();
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
            });
    }
};
} // namespace sp
