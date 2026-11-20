#include "PluginProcessPanel.h"
#include <set>
namespace sp
{
namespace
{
void label(juce::Label &l, const juce::String &name)
{
    l.setText(name, juce::dontSendNotification);
    l.setColour(juce::Label::textColourId, ui::muted);
    l.setFont(juce::FontOptions(12.f));
}
Asset artifact(const juce::var &v)
{
    Asset a;
    a.path = v["path"].toString();
    a.name = juce::File(a.path).getFileName();
    a.id = v["sha256"].toString();
    a.samples = (int64_t)v["frames"];
    a.sampleRate = (double)v["rate"];
    a.detectedChannels = layoutFromJson(v["layout"]);
    a.channels = a.detectedChannels.size();
    a.duration = a.samples / a.sampleRate;
    a.online = true;
    a.state = "ready";
    FileStamp s;
    if (fileStamp(juce::File(a.path), s))
    {
        a.size = s.size;
        a.modified = s.modified;
        a.inode = s.inode;
    }
    return a;
}
} // namespace
PluginProcessPanel::PluginProcessPanel(PluginInput input, juce::File workspace, AudioClient &client,
                                       std::shared_ptr<WaveformPool> waves, juce::File helper,
                                       std::vector<juce::File> automaticRoots)
    : session(helper == juce::File() ? juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                                           .getParentDirectory()
                                           .getParentDirectory()
                                           .getChildFile("Helpers/SoundPlusPlugin")
                                     : helper,
              workspace),
      selected(std::move(input)), audio(client), pool(std::move(waves))
{
    setLookAndFeel(&look);
    setWantsKeyboardFocus(true);
    addKeyListener(this);
    setTitle("Process with Plug-in");
    for (auto *c : std::initializer_list<juce::Component *>{&heading,
                                                            &transport,
                                                            &auditionFeedback,
                                                            &inputSummary,
                                                            &outputSummary,
                                                            &status,
                                                            &physicalLabel,
                                                            &roleLabel,
                                                            &tailLabel,
                                                            &parameterSummary,
                                                            &interpretation,
                                                            &plugins,
                                                            &subset,
                                                            &search,
                                                            &mean,
                                                            &bypass,
                                                            &normalize,
                                                            &split,
                                                            &prepare,
                                                            &scan,
                                                            &retry,
                                                            &location,
                                                            &preview,
                                                            &compare,
                                                            &copy,
                                                            &print,
                                                            &index,
                                                            &cancel,
                                                            &undo,
                                                            &redo,
                                                            &reset,
                                                            &save,
                                                            &open,
                                                            &tail,
                                                            &parameterViewport,
                                                            &drag})
    {
        addAndMakeVisible(c);
        if (!dynamic_cast<juce::TextEditor *>(c))
            c->addKeyListener(this);
    }
    label(heading, "Process with Plug-in");
    heading.setColour(juce::Label::textColourId, ui::text);
    heading.setFont(juce::FontOptions(18.f, juce::Font::bold));
    label(inputSummary, selected.scope + ": " + selected.source.name);
    label(outputSummary, "Processed waveform appears after Preview");
    label(status, session.status);
    status.setMinimumHorizontalScale(1);
    status.setJustificationType(juce::Justification::topLeft);
    label(physicalLabel, "Physical inputs (one-based)");
    label(roleLabel, "Ordered input roles");
    label(tailLabel, "Tail cap (seconds)");
    label(parameterSummary, "Generic controls | normalized 0-1; plugin text in tooltips");
    normalize.setTooltip("Requested normalization. The waveform summary identifies the actual "
                         "audible/deliverable representation, including a raw fallback after failure.");
    subset.setTitle("Physical input subset, comma separated");
    subset.setColour(juce::TextEditor::outlineColourId, ui::line);
    juce::StringArray values;
    for (auto n : selected.inputs)
        values.add(juce::String(n + 1));
    subset.setText(values.joinIntoString(", "), false);
    int n = 1;
    for (auto &l : namedLayouts())
        interpretation.addItem(layoutName(l), n++);
    for (int count = 1; count <= 16; ++count)
        interpretation.addItem(layoutName(discreteLayout(count)), n++);
    interpretation.setTitle("Explicit ordered input layout");
    interpretation.setTextWhenNothingSelected("Choose ordered roles");
    auto layouts = namedLayouts();
    for (int count = 1; count <= 16; ++count)
        layouts.push_back(discreteLayout(count));
    for (size_t j = 0; j < layouts.size(); ++j)
        if (layouts[j] == selected.layout)
            interpretation.setSelectedId((int)j + 1, juce::dontSendNotification);
    mean.setToggleState(selected.mean, juce::dontSendNotification);
    mean.setTooltip("Explicit arithmetic mean of all physical channels into Mono, up to 64 inputs");
    subset.onTextChange = [this]
    {
        if (!updating)
            session.clearInput();
    };
    interpretation.onChange = [this]
    {
        if (!updating)
            session.clearInput();
    };
    mean.onClick = [this]
    {
        if (!updating)
        {
            interpretation.setEnabled(!mean.getToggleState());
            subset.setEnabled(!mean.getToggleState());
            if (mean.getToggleState())
                interpretation.setSelectedId(1, juce::dontSendNotification);
            session.clearInput();
        }
    };
    search.setTitle("Search plugin name or vendor");
    search.setTextToShowWhenEmpty("Search plugin / vendor", ui::muted);
    search.onTextChange = [this] { populatePlugins(); };
    plugins.setTitle("Compatible installed VST3 effect");
    plugins.setTextWhenNothingSelected("Scan and choose an effect");
    plugins.onChange = [this]
    {
        if (!updating && juce::isPositiveAndBelow(plugins.getSelectedId() - 1, (int)choices.size()))
            session.choose(choices[(size_t)plugins.getSelectedId() - 1]);
    };
    roots = std::move(automaticRoots);
    prepare.onClick = [this] { prepareInput(); };
    scan.onClick = [this] { session.scan(roots); };
    retry.onClick = [this] { session.scan(roots, true); };
    location.onClick = [this] { chooseLocation(); };
    preview.onClick = [this] { session.preview(); };
    cancel.onClick = [this] { session.cancel(); };
    tail.setRange(0, 30, .1);
    tail.setSliderStyle(juce::Slider::LinearHorizontal);
    tail.setTextBoxStyle(juce::Slider::TextBoxRight, false, 52, 22);
    tail.setTitle("Explicit finite tail cap in seconds");
    tail.setValue(2, juce::dontSendNotification);
    tail.onDragStart = [this] { session.beginGesture(); };
    tail.onDragEnd = [this] { session.endGesture(); };
    tail.onValueChange = [this]
    {
        if (!updating && session.settings.isObject())
        {
            auto s = session.settings.clone();
            s.getDynamicObject()->setProperty("tailSeconds", tail.getValue());
            session.change(s);
        }
    };
    bypass.onClick = [this]
    {
        if (session.settings.isObject())
        {
            auto s = session.settings.clone();
            s.getDynamicObject()->setProperty("bypass", bypass.getToggleState());
            session.change(s);
        }
    };
    normalize.onClick = [this]
    {
        if (!updating)
            session.setNormalization(normalize.getToggleState());
    };
    split.onClick = [this] { session.deliverySplit = split.getToggleState(); };
    undo.onClick = [this] { session.undo(); };
    redo.onClick = [this] { session.redo(); };
    reset.onClick = [this] { session.reset(); };
    transport.onClick = [this]
    {
        auto state = audio.status();
        bool active = auditionGeneration && state["intentId"].toString() == auditionId &&
                      (uint64_t)(juce::int64)state["intentGeneration"] == auditionGeneration &&
                      ((bool)state["playing"] || (bool)state["pendingPlayback"] || (bool)state["preparing"]);
        if (active)
            stopAudition();
        else
            audition(processedPlaying && session.ready());
    };
    transport.setPrimary();
    label(auditionFeedback, "Audition idle");
    compare.onClick = [this] { audition(!processedPlaying); };
    copy.onClick = [this]
    {
        session.createCopy(juce::File::getSpecialLocation(juce::File::userMusicDirectory)
                               .getChildFile("SoundPlus/Processed"),
                           false, split.getToggleState());
    };
    print.onClick = [this]
    {
        if (session.receipt.isObject() && onPrint)
            onPrint(juce::File(session.receipt["path"].toString()));
    };
    index.onClick = [this]
    {
        if (session.receipt.isObject() && onIndex)
            onIndex(juce::File(session.receipt["path"].toString()).getParentDirectory());
    };
    save.onClick = [this] { jobFile(true); };
    open.onClick = [this] { jobFile(false); };
    parameterViewport.setViewedComponent(&parameterContent, false);
    parameterViewport.setScrollBarsShown(true, false);
    session.onChange = [this] { refresh(); };
    setSize(700, 620);
    refresh();
    session.recallCatalog(roots);
    startTimerHz(15);
}
PluginProcessPanel::~PluginProcessPanel()
{
    stopTimer();
    session.onChange = {};
    session.cancel();
    if (pool)
        pool->demand(8, {});
    stopAudition();
    setLookAndFeel(nullptr);
}
void PluginProcessPanel::prepareInput()
{
    try
    {
        auto layouts = namedLayouts();
        for (int count = 1; count <= 16; ++count)
            layouts.push_back(discreteLayout(count));
        int id = interpretation.getSelectedId() - 1;
        selected.mean = mean.getToggleState();
        if (selected.mean)
            selected.layout = channelLayout("mono-v1");
        else
        {
            if (!juce::isPositiveAndBelow(id, (int)layouts.size()))
                throw std::runtime_error(
                    "Choose an explicit ordered layout; channel count does not imply surround");
            selected.layout = layouts[(size_t)id];
        }
        selected.inputs.clear();
        std::set<int> seen;
        for (auto &t : juce::StringArray::fromTokens(subset.getText(), ", ", ""))
            if (t.isNotEmpty())
            {
                int c = t.getIntValue() - 1;
                if (!t.containsOnly("0123456789") || c < 0 || c >= selected.source.channels ||
                    !seen.insert(c).second)
                    throw std::runtime_error("Enter distinct existing physical channel numbers");
                selected.inputs.push_back(c);
            }
        session.prepare(selected);
    }
    catch (const std::exception &e)
    {
        session.error = e.what();
        session.status = session.error;
        refresh();
    }
}
void PluginProcessPanel::populatePlugins()
{
    updating = true;
    auto remember = [this](const juce::String &path)
    {
        juce::File f(path);
        if (f != juce::File() && std::find(roots.begin(), roots.end(), f) == roots.end())
            roots.push_back(f);
    };
    if (auto *saved = session.discovery["roots"].getArray())
        for (auto &path : *saved)
            remember(path.toString());
    else if (auto *entries = session.discovery["entries"].getArray())
        for (auto &e : *entries)
            remember(juce::File(e["path"].toString()).getParentDirectory().getFullPathName());
    auto selectedId = session.settings["plugin"]["id"].toString();
    plugins.clear(juce::dontSendNotification);
    choices.clear();
    int active = 0;
    juce::StringArray unavailable;
    std::vector<juce::var> grouped;
    if (auto *entries = session.discovery["entries"].getArray())
        for (auto &entry : *entries)
        {
            if (entry["error"].toString().isNotEmpty())
                unavailable.add(juce::File(entry["path"].toString()).getFileName() + ": " +
                                entry["error"].toString());
            if (auto *list = entry["plugins"].getArray())
                for (auto &p : *list)
                    if ((p["name"].toString() + " / " + p["vendor"].toString() + " / " +
                         p["category"].toString())
                            .containsIgnoreCase(search.getText()))
                        grouped.push_back(p);
        }
    std::sort(grouped.begin(), grouped.end(),
              [](const auto &a, const auto &b)
              {
                  auto left =
                      pluginEffectGroup(a) + ":" + a["name"].toString() + ":" + a["vendor"].toString();
                  auto right =
                      pluginEffectGroup(b) + ":" + b["name"].toString() + ":" + b["vendor"].toString();
                  return left.compareIgnoreCase(right) < 0;
              });
    juce::String previousGroup;
    for (auto &p : grouped)
    {
        auto group = pluginEffectGroup(p);
        if (group != previousGroup)
        {
            plugins.addSectionHeading(group);
            previousGroup = group;
        }
        auto name = p["name"].toString() + " / " + p["vendor"].toString();
        choices.push_back(p);
        int id = (int)choices.size();
        plugins.addItem(name, id);
        bool supported = p["reason"].toString().isEmpty();
        auto layoutId = session.input["layout"]["id"].toString();
        bool layoutOK = false;
        if (auto *a = p["layouts"].getArray())
            for (auto &l : *a)
                layoutOK |= l.toString() == layoutId;
        if (layoutId.isEmpty())
            plugins.changeItemText(id, name + " (prepare input first)");
        else if (!supported || !layoutOK)
            plugins.changeItemText(id, name + " (unavailable for input)");
        plugins.setItemEnabled(id, supported && layoutOK);
        if (!supported || (!layoutOK && layoutId.isNotEmpty()))
            unavailable.add(name + ": " +
                            (supported ? "does not accept " + layoutId : p["reason"].toString()));
        if (p["id"].toString() == selectedId)
            active = id;
    }
    if (!session.busy && !unavailable.isEmpty())
        status.setText(session.status + "\n" + unavailable[0], juce::dontSendNotification);
    plugins.setSelectedId(active, juce::dontSendNotification);
    plugins.setTooltip(unavailable.isEmpty() ? "Choose one compatible native VST3 effect"
                                             : unavailable.joinIntoString("\n"));
    retry.setTooltip(unavailable.joinIntoString("\n"));
    if (choices.empty() && session.settings["plugin"].isObject())
    {
        plugins.addItem("Saved: " + session.settings["plugin"]["name"].toString(), 9000);
        plugins.setSelectedId(9000, juce::dontSendNotification);
        plugins.setItemEnabled(9000, false);
    }
    updating = false;
}
void PluginProcessPanel::rebuildParameters()
{
    parameterSliders.clear();
    parameterNames.clear();
    auto params = session.settings["parameters"];
    lastParameters = params.clone();
    int y = 0;
    if (auto *a = params.getArray())
        for (int i = 0; i < a->size(); ++i)
        {
            auto p = (*a)[i];
            auto *name = parameterNames.add(new juce::Label());
            label(*name, p["name"].toString());
            name->setBounds(0, y, 180, 28);
            parameterContent.addAndMakeVisible(name);
            auto *s = parameterSliders.add(new juce::Slider());
            s->addKeyListener(this);
            s->setTitle(p["name"].toString() + " normalized value");
            s->setRange(0, 1, .0001);
            s->setSliderStyle(juce::Slider::LinearHorizontal);
            s->setTextBoxStyle(juce::Slider::TextBoxRight, false, 66, 24);
            s->setValue((double)p["value"], juce::dontSendNotification);
            s->setBounds(182, y, std::max(100, parameterViewport.getWidth() - 200), 28);
            parameterContent.addAndMakeVisible(s);
            s->onDragStart = [this] { session.beginGesture(); };
            s->onDragEnd = [this] { session.endGesture(); };
            s->onValueChange = [this, s, i]
            {
                if (!updating)
                {
                    auto v = session.settings.clone();
                    v["parameters"].getArray()->getReference(i).getDynamicObject()->setProperty(
                        "value", s->getValue());
                    session.change(v);
                }
            };
            y += 32;
        }
    parameterContent.setSize(std::max(280, parameterViewport.getWidth() - 14), std::max(32, y));
}
void PluginProcessPanel::refresh()
{
    updating = true;
    status.setText(session.status, juce::dontSendNotification);
    status.setTooltip(session.error);
    undo.setEnabled(session.canUndo() && !session.busy);
    redo.setEnabled(session.canRedo() && !session.busy);
    reset.setEnabled(session.settings.isObject() && !session.busy);
    save.setEnabled(session.input.isObject() && !session.busy);
    preview.setEnabled(session.input.isObject() && session.settings.isObject() && !session.busy &&
                       session.contextValid());
    copy.setEnabled(session.ready());
    compare.setEnabled(session.ready() && !audio.captureBusy());
    cancel.setEnabled(session.canCancel());
    prepare.setEnabled(!session.busy);
    plugins.setEnabled(session.input.isObject() && !session.busy);
    tail.setEnabled(session.settings.isObject() && !session.busy);
    normalize.setEnabled(session.settings.isObject() && !session.busy);
    split.setToggleState(session.deliverySplit, juce::dontSendNotification);
    bypass.setEnabled(session.settings.isObject() && !session.busy);
    auto published = session.receipt.isObject() && session.ready();
    print.setEnabled(published);
    index.setEnabled(published);
    drag.setEnabled(published);
    drag.file = published ? juce::File(session.receipt["path"].toString()) : juce::File();
    if (session.settings.isObject())
    {
        tail.setValue((double)session.settings["tailSeconds"], juce::dontSendNotification);
        bypass.setToggleState((bool)session.settings["bypass"], juce::dontSendNotification);
        normalize.setToggleState(session.deliveryNormalize, juce::dontSendNotification);
    }
    auto key = session.settings["plugin"]["id"].toString() + session.settings["stateHash"].toString();
    if (key != parameterKey)
    {
        parameterKey = key;
        rebuildParameters();
        updating = true;
    }
    else if (auto *a = session.settings["parameters"].getArray())
        for (int i = 0; i < std::min(a->size(), parameterSliders.size()); ++i)
            parameterSliders[i]->setValue((double)(*a)[i]["value"], juce::dontSendNotification);
    for (int i = 0; i < parameterSliders.size(); ++i)
    {
        auto current = session.settings["parameters"][i];
        auto *renderedParameters = session.result["parameters"].getArray();
        auto reported = renderedParameters && juce::isPositiveAndBelow(i, renderedParameters->size())
                            ? (*renderedParameters)[i]
                            : juce::var();
        if (!reported.isObject() || std::abs((double)reported["value"] - (double)current["value"]) > 1e-7)
            reported = lastParameters[i];
        bool matches =
            reported.isObject() && std::abs((double)reported["value"] - (double)current["value"]) <= 1e-7;
        parameterSliders[i]->setTooltip(current["name"].toString() + " | normalized 0-1" +
                                        (matches ? " | plugin reports: " + reported["text"].toString()
                                                 : " | Preview to read the plugin's formatted value"));
    }
    for (auto *s : parameterSliders)
        s->setEnabled(!session.busy);
    populatePlugins();
    auto identity =
        session.input.isObject() ? juce::JSON::toString(session.input["selection"], true) : juce::String();
    if (identity != bindingKey)
    {
        bindingKey = identity;
        if (session.input.isObject())
        {
            selected = pluginInputFromJson(session.input["selection"]);
            inputSummary.setTooltip(selected.source.path + " | original frames [" +
                                    juce::String(selected.first) + ", " + juce::String(selected.last) +
                                    ") | " + selected.scope);
            juce::StringArray physical;
            for (auto c : selected.inputs)
                physical.add(juce::String(c + 1));
            subset.setText(physical.joinIntoString(", "), false);
            mean.setToggleState(selected.mean, juce::dontSendNotification);
            interpretation.setEnabled(!selected.mean);
            subset.setEnabled(!selected.mean);
            auto layouts = namedLayouts();
            for (int n = 1; n <= 16; ++n)
                layouts.push_back(discreteLayout(n));
            for (size_t n = 0; n < layouts.size(); ++n)
                if (layouts[n] == selected.layout)
                    interpretation.setSelectedId((int)n + 1, juce::dontSendNotification);

            inputSummary.setText(
                selected.scope + ": " + selected.source.name + " | " +
                    juce::String(((double)session.input["frames"] / (double)session.input["rate"]), 2) +
                    " s | " + layoutBadge(layoutFromJson(session.input["layout"])),
                juce::dontSendNotification);
        }
        else
            inputSummary.setText(selected.scope + ": " + selected.source.name + " | Prepare input",
                                 juce::dontSendNotification);
    }
    if (auditionGeneration &&
        (auditionBinding != bindingKey ||
         auditionHash != (processedPlaying ? session.result : session.input)["sha256"].toString() ||
         !session.contextValid()))
        stopAudition();
    auto waves = session.input["sha256"].toString() + session.result["sha256"].toString();
    if (waves != waveKey || waveformBindingNeedsRefresh())
    {
        waveKey = waves;
        inputAsset = {};
        outputAsset = {};
        inputPeaks.reset();
        outputPeaks.reset();
        if (session.input.isObject())
        {
            inputAsset = artifact(session.input);
        }
        if (session.result.isObject())
        {
            outputAsset = artifact(session.result);
        }
        if (pool)
        {
            std::vector<Asset> demand;
            if (inputAsset.samples)
                demand.push_back(inputAsset);
            if (outputAsset.samples)
                demand.push_back(outputAsset);
            pool->demand(8, demand);
        }
    }
    if (session.result.isObject())
    {
        const bool normalized = (bool)session.result["presentationNormalized"];
        auto representation = normalized ? juce::String("Normalized") : juce::String("Raw processed");
        if (normalized != session.deliveryNormalize)
            representation += " (requested normalization differs)";
        outputSummary.setText(
            representation + " | " +
                juce::String((double)session.result["frames"] / (double)session.result["rate"], 2) + " s | " +
                layoutBadge(layoutFromJson(session.result["layout"])) + " | tail " +
                session.result["tailSeconds"].toString() + " s",
            juce::dontSendNotification);
    }
    else
        outputSummary.setText("No current processed waveform - render Preview", juce::dontSendNotification);
    updating = false;
    updateAudition();
    repaint();
}
void PluginProcessPanel::audition(bool processed)
{
    if ((processed ? !session.ready()
                   : !session.input.isObject() || session.busy || !session.contextValid()) ||
        audio.captureBusy())
        return;
    if (beforeAudition)
        beforeAudition();
    auto v = processed ? session.result : session.input;
    auto asset = artifact(v);
    PlaybackRegion region;
    region.endSample = asset.samples;
    region.size = asset.size;
    region.modified = asset.modified;
    region.inode = asset.inode;
    region.revision = asset.revision;
    region.auditionPair = -2;
    region.sourceLayoutOverride = asset.detectedChannels.id;
    auditionId = "plugin-process:" + juce::Uuid().toString();
    auditionHash = v["sha256"].toString();
    auditionBinding = bindingKey;
    auditionGeneration = audio.playRegion(auditionId, juce::File(asset.path), region);
    auditionState = "Preparing";
    auditionWasActive = true;
    processedPlaying = processed;
    updateAudition();
}
void PluginProcessPanel::stopAudition()
{
    if (audio.stopIfOwned(auditionId, auditionGeneration))
        auditionState = "Stopped";
    auditionGeneration = 0;
    auditionWasActive = false;
    updateAudition();
}
void PluginProcessPanel::updateAudition()
{
    auto state = audio.status();
    bool owned = auditionGeneration && state["intentId"].toString() == auditionId &&
                 (uint64_t)(juce::int64)state["intentGeneration"] == auditionGeneration;
    bool active =
        owned && ((bool)state["playing"] || (bool)state["pendingPlayback"] || (bool)state["preparing"]);
    if (owned)
    {
        if ((bool)state["pendingPlayback"] || (bool)state["preparing"])
            auditionState = "Preparing";
        else if (state["error"].toString().isNotEmpty())
            auditionState = "Error: " + state["error"].toString();
        else if ((bool)state["playing"])
            auditionState = "Playing";
        else if ((bool)state["completed"])
            auditionState = "Completed";
        else if (auditionWasActive)
            auditionState = "Stopped";
    }
    else if (auditionGeneration)
    {
        auditionState = "Other playback owns transport";
        auditionGeneration = 0;
    }
    auditionWasActive = active;
    transport.setIcon(active ? ActionIcon::stop : ActionIcon::play);
    transport.setButtonText(active
                                ? "Stop audition (Space)"
                                : (processedPlaying && session.ready() ? "Play processed audition (Space)"
                                                                       : "Play original audition (Space)"));
    transport.setEnabled(!audio.captureBusy() && (active || (!session.busy && session.contextValid() &&
                                                             (session.ready() || session.input.isObject()))));
    compare.setButtonText(processedPlaying ? "Compare Original" : "Compare Processed");
    compare.setEnabled(session.ready() && !audio.captureBusy());
    auditionFeedback.setText(
        ((processedPlaying && session.result.isObject()) ? "Processed | " : "Original | ") + auditionState,
        juce::dontSendNotification);
    auditionFeedback.setTooltip(auditionFeedback.getText());
}
void PluginProcessPanel::timerCallback()
{
    updateAudition();
    if (!session.contextValid() && !session.error.contains("context"))
    {
        session.cancel();
        session.error = "Document context changed";
        session.status = "Document context changed - reopen Process for the new input";
        refresh();
    }
    auto stateFor = [this](const Asset &a) -> juce::String
    {
        if (!a.samples)
            return "Not prepared";
        auto data = pool ? pool->get(a) : nullptr;
        if (!data)
            return "Loading waveform";
        if (data->status == WaveformData::Status::offline)
            return "Offline";
        if (data->status == WaveformData::Status::changed)
            return "Source changed";
        if (data->status == WaveformData::Status::decodeError)
            return "Waveform error: " + data->error;
        bool silent = std::all_of(data->peaks.begin(), data->peaks.end(),
                                  [](auto &p) { return p.first == 0 && p.second == 0; });
        return silent ? "Silence" : "Ready";
    };
    inputWaveState = stateFor(inputAsset);
    outputWaveState = stateFor(outputAsset);
    if (pool)
    {
        pool->tick();
        if (inputAsset.samples)
            inputPeaks = pool->columns(inputAsset, 0, inputAsset.samples, false, getWidth() - 40);
        if (outputAsset.samples)
            outputPeaks = pool->columns(outputAsset, 0, outputAsset.samples, false, getWidth() - 40);
    }
    if (waveformBindingNeedsRefresh())
        refresh();
    repaint();
}
bool PluginProcessPanel::waveformBindingNeedsRefresh() const
{
    if (!pool)
        return false;
    auto unavailableAlias = [this](const Asset &cached, const juce::var &current)
    {
        if (!current.isObject() || cached.path == current["path"].toString())
            return false;
        auto data = pool->get(cached);
        return data && data->status != WaveformData::Status::ready;
    };
    return unavailableAlias(inputAsset, session.input) || unavailableAlias(outputAsset, session.result);
}
void PluginProcessPanel::paint(juce::Graphics &g)
{
    g.fillAll(ui::background);
    for (int y : {60, 450})
    {
        g.setColour(ui::surface);
        g.fillRoundedRectangle(16.f, (float)y, (float)getWidth() - 32, 40, 5);
    }
    if (!inputPeaks || inputWaveState == "Silence")
    {
        g.setColour(ui::muted);
        g.drawText(inputWaveState, juce::Rectangle<int>(20, 64, getWidth() - 40, 32),
                   juce::Justification::centred);
    }
    if (!outputPeaks || outputWaveState == "Silence")
    {
        g.setColour(ui::muted);
        g.drawText(outputWaveState, juce::Rectangle<int>(20, 454, getWidth() - 40, 32),
                   juce::Justification::centred);
    }
    if (inputPeaks)
        paintCompactWaveform(g, {20, 64, (float)getWidth() - 40, 32}, *inputPeaks, ui::accent);
    if (outputPeaks)
        paintCompactWaveform(g, {20, 454, (float)getWidth() - 40, 32}, *outputPeaks, ui::accent);
}
void PluginProcessPanel::resized()
{
    int w = getWidth();
    heading.setBounds(16, 6, w - 200, 26);
    save.setBounds(w - 176, 6, 76, 26);
    open.setBounds(w - 96, 6, 76, 26);
    inputSummary.setBounds(16, 34, w - 32, 24);
    physicalLabel.setBounds(16, 104, 170, 20);
    subset.setBounds(16, 126, 160, 26);
    roleLabel.setBounds(188, 104, 170, 20);
    interpretation.setBounds(188, 126, 160, 26);
    mean.setBounds(358, 126, 180, 26);
    prepare.setBounds(w - 138, 126, 122, 26);
    search.setBounds(16, 160, 210, 26);
    scan.setBounds(230, 160, 56, 26);
    retry.setBounds(290, 160, 130, 26);
    location.setBounds(426, 160, 170, 26);
    plugins.setBounds(16, 194, w - 32, 28);
    parameterSummary.setBounds(16, 228, w - 200, 22);
    undo.setBounds(w - 188, 226, 52, 26);
    redo.setBounds(w - 134, 226, 52, 26);
    reset.setBounds(w - 80, 226, 64, 26);
    parameterViewport.setBounds(16, 254, w - 32, 136);
    for (int i = 0; i < parameterSliders.size(); ++i)
        parameterSliders[i]->setBounds(182, i * 32, std::max(100, w - 246), 28);
    parameterContent.setSize(w - 46, std::max(32, parameterSliders.size() * 32));
    tailLabel.setBounds(16, 394, 120, 24);
    tail.setBounds(140, 394, 200, 26);
    bypass.setBounds(350, 394, 92, 26);
    normalize.setBounds(446, 394, 150, 26);
    split.setBounds(600, 394, 94, 26);
    outputSummary.setBounds(16, 424, w - 32, 24);
    preview.setBounds(16, 498, 90, 30);
    compare.setBounds(110, 498, 150, 30);
    copy.setBounds(264, 498, 102, 30);
    print.setBounds(370, 498, 110, 30);
    index.setBounds(484, 498, 112, 30);
    drag.setBounds(w - 92, 498, 32, 30);
    transport.setBounds(16, 534, 32, 30);
    auditionFeedback.setBounds(56, 536, w - 232, 26);
    cancel.setBounds(w - 166, 534, 150, 30);
    status.setBounds(16, 574, w - 32, getHeight() - 584);
}
bool PluginProcessPanel::keyPressed(const juce::KeyPress &key)
{
    for (auto *focus = juce::Component::getCurrentlyFocusedComponent(); focus;
         focus = focus->getParentComponent())
        if (dynamic_cast<juce::TextEditor *>(focus))
            return false;
    if (key.getKeyCode() == juce::KeyPress::spaceKey && !key.getModifiers().isAnyModifierKeyDown())
    {
        if (!transport.isEnabled())
            return false;
        transport.onClick();
        return true;
    }
    if (key.getModifiers().isCommandDown() &&
        juce::CharacterFunctions::toUpperCase((juce::juce_wchar)key.getKeyCode()) == 'Z')
    {
        if (key.getModifiers().isShiftDown())
            session.redo();
        else
            session.undo();
        return true;
    }
    return false;
}
bool PluginProcessPanel::keyPressed(const juce::KeyPress &key, juce::Component *)
{
    if (key.getKeyCode() != juce::KeyPress::spaceKey)
        return keyPressed(key);
    for (auto *focus = juce::Component::getCurrentlyFocusedComponent(); focus;
         focus = focus->getParentComponent())
        if (dynamic_cast<juce::TextEditor *>(focus))
            return false;
    if (key.getModifiers().isAnyModifierKeyDown())
        return false;
    const bool physicallyHeld = juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::spaceKey);
    if (physicallyHeld && spaceApplied)
        return true;
    const bool used = keyPressed(key);
    // Separate synthetic/accessibility key presses need not supply a key-up.
    // Suppress only an actual held physical key's repeat, never a later press.
    spaceApplied = physicallyHeld && used;
    return used;
}
bool PluginProcessPanel::keyStateChanged(bool, juce::Component *)
{
    for (auto *focus = juce::Component::getCurrentlyFocusedComponent(); focus;
         focus = focus->getParentComponent())
        if (dynamic_cast<juce::TextEditor *>(focus))
            return false;
    if (juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::spaceKey) &&
        !juce::ModifierKeys::currentModifiers.isAnyModifierKeyDown())
    {
        spaceHeld = true;
        return true;
    }
    if (spaceHeld || spaceApplied)
    {
        spaceHeld = spaceApplied = false;
        return true;
    }
    return false;
}
void PluginProcessPanel::chooseLocation()
{
    chooser = std::make_unique<juce::FileChooser>("Choose a VST3 plugin folder or bundle",
                                                  juce::File("/Library/Audio/Plug-Ins"), "*", true);
    juce::Component::SafePointer<PluginProcessPanel> safe(this);
    chooser->launchAsync(juce::FileBrowserComponent::openMode |
                             juce::FileBrowserComponent::canSelectDirectories,
                         [safe](auto &c)
                         {
                             if (safe && c.getResult().isDirectory())
                             {
                                 safe->roots.push_back(c.getResult());
                                 safe->session.scan(safe->roots);
                             }
                         });
}
void PluginProcessPanel::jobFile(bool writing)
{
    chooser = std::make_unique<juce::FileChooser>(
        writing ? "Save plugin processing job" : "Open plugin processing job",
        session.directory.getChildFile("Processing.soundplus-process"), "*.soundplus-process", true);
    juce::Component::SafePointer<PluginProcessPanel> safe(this);
    chooser->launchAsync(
        (writing ? juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting
                 : juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles),
        [safe, writing](auto &c)
        {
            if (safe && c.getResult() != juce::File())
                try
                {
                    if (writing)
                        safe->session.save(c.getResult());
                    else
                        safe->session.open(c.getResult());
                }
                catch (const std::exception &e)
                {
                    safe->session.error = e.what();
                    safe->session.status = safe->session.error;
                    safe->refresh();
                }
        });
}
} // namespace sp
