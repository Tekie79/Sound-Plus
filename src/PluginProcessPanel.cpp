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
                                       std::shared_ptr<WaveformPool> waves, juce::File helper)
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
    setTitle("Process with Plug-in");
    for (auto *c : std::initializer_list<juce::Component *>{&heading,
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
        addAndMakeVisible(c);
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
    label(parameterSummary, "Generic parameters - values are normalized 0-1");
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
    roots = {juce::File("/Library/Audio/Plug-Ins/VST3"),
             juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                 .getChildFile("Library/Audio/Plug-Ins/VST3")};
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
    session.recallCatalog();
    startTimerHz(15);
}
PluginProcessPanel::~PluginProcessPanel()
{
    stopTimer();
    session.onChange = {};
    session.cancel();
    if (pool)
        pool->demand(8, {});
    if (audio.status()["id"].toString().startsWith("plugin-process:"))
        audio.stop();
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
    if (auto *entries = session.discovery["entries"].getArray())
        for (auto &entry : *entries)
        {
            if (entry["error"].toString().isNotEmpty())
                unavailable.add(juce::File(entry["path"].toString()).getFileName() + ": " +
                                entry["error"].toString());
            if (auto *list = entry["plugins"].getArray())
                for (auto &p : *list)
                {
                    auto name = p["name"].toString() + " / " + p["vendor"].toString();
                    if (!name.containsIgnoreCase(search.getText()))
                        continue;
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
    compare.setEnabled(session.ready());
    cancel.setEnabled(session.busy);
    prepare.setEnabled(!session.busy);
    plugins.setEnabled(session.input.isObject() && !session.busy);
    tail.setEnabled(session.settings.isObject() && !session.busy);
    normalize.setEnabled(session.settings.isObject() && !session.busy);
    split.setToggleState(session.deliverySplit, juce::dontSendNotification);
    bypass.setEnabled(session.settings.isObject() && !session.busy);
    auto published = session.receipt.isObject() && !session.busy;
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
    for (auto *s : parameterSliders)
        s->setEnabled(!session.busy);
    populatePlugins();
    auto waves = session.input["sha256"].toString() + session.result["sha256"].toString();
    if (waves != waveKey)
    {
        waveKey = waves;
        inputAsset = {};
        outputAsset = {};
        inputPeaks.reset();
        outputPeaks.reset();
        if (session.input.isObject())
        {
            inputAsset = artifact(session.input);
            selected = pluginInputFromJson(session.input["selection"]);
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

            inputSummary.setText(selected.scope + ": " + selected.source.name + " | " +
                                     juce::String(inputAsset.duration, 2) + " s | " +
                                     layoutBadge(inputAsset.detectedChannels),
                                 juce::dontSendNotification);
        }
        if (session.result.isObject())
        {
            outputAsset = artifact(session.result);
            outputSummary.setText("Processed | " + juce::String(outputAsset.duration, 2) + " s | " +
                                      layoutBadge(outputAsset.detectedChannels) + " | tail " +
                                      session.result["tailSeconds"].toString() + " s",
                                  juce::dontSendNotification);
        }
        else
            outputSummary.setText("No current processed waveform - render Preview",
                                  juce::dontSendNotification);
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
    updating = false;
    repaint();
}
void PluginProcessPanel::audition(bool processed)
{
    if (!session.ready() || audio.captureBusy())
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
    audio.playRegion("plugin-process:" + asset.id, juce::File(asset.path), region);
    processedPlaying = processed;
    compare.setButtonText(processed ? "Compare Original" : "Compare Processed");
}
void PluginProcessPanel::timerCallback()
{
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
    repaint();
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
    cancel.setBounds(w - 58, 498, 46, 30);
    status.setBounds(16, 538, w - 32, getHeight() - 548);
}
bool PluginProcessPanel::keyPressed(const juce::KeyPress &key)
{
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
