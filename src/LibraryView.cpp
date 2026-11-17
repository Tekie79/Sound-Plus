#include "LibraryView.h"
namespace sp
{
namespace
{
const juce::Colour bg{0xff101419}, panel{0xff171d25}, line{0xff29323e}, text{0xffe6ecf2}, muted{0xff9baabc},
    accent{0xff77bfff}, amber{0xffffc384};
juce::String duration(double seconds)
{
    if (seconds <= 0)
        return juce::String::fromUTF8("—");
    int whole = (int)seconds;
    return juce::String(whole / 60) + ":" + juce::String(whole % 60).paddedLeft('0', 2) + "." +
           juce::String((int)(seconds * 10) % 10);
}
} // namespace
LibraryView::LibraryView(const juce::File &db, const juce::File &executable) : library(db), audio(executable)
{
    look.setColour(juce::ResizableWindow::backgroundColourId, bg);
    look.setColour(juce::TextButton::buttonColourId, line);
    look.setColour(juce::TextButton::textColourOffId, text);
    look.setColour(juce::TextEditor::backgroundColourId, panel);
    look.setColour(juce::TextEditor::textColourId, text);
    look.setColour(juce::TextEditor::outlineColourId, line);
    look.setColour(juce::TextEditor::focusedOutlineColourId, accent);
    look.setColour(juce::ComboBox::backgroundColourId, panel);
    look.setColour(juce::ComboBox::textColourId, text);
    look.setColour(juce::ListBox::backgroundColourId, panel);
    look.setColour(juce::ListBox::textColourId, text);
    look.setColour(juce::TableHeaderComponent::backgroundColourId, panel);
    look.setColour(juce::TableHeaderComponent::textColourId, muted);
    look.setColour(juce::TableHeaderComponent::outlineColourId, line);
    look.setColour(juce::Slider::thumbColourId, accent);
    look.setColour(juce::Slider::trackColourId, accent);
    setLookAndFeel(&look);
    for (auto *c : std::initializer_list<juce::Component *>{&table, &sources, &search, &add, &rescan, &play,
                                                            &restart, &output, &gain, &summary, &detail,
                                                            &assetId, &transport, &status, &volumeLabel})
        addAndMakeVisible(c);
    for (auto *l : {&summary, &detail, &assetId, &transport, &status, &volumeLabel})
    {
        l->setColour(juce::Label::textColourId, muted);
        l->setFont(juce::FontOptions(13.0f));
    }
    detail.setColour(juce::Label::textColourId, text);
    detail.setFont(juce::FontOptions(16.0f));
    transport.setColour(juce::Label::textColourId, amber);
    search.setTextToShowWhenEmpty(juce::String::fromUTF8("Search filenames and folders…"), muted);
    search.setTitle("Search library");
    search.setFont(juce::FontOptions(17.0f));
    search.setIndents(12, 10);
    search.onTextChange = [this] { requestSearch(); };
    search.addKeyListener(this);
    table.addKeyListener(this);
    sources.addKeyListener(this);
    addKeyListener(this);
    add.setTitle("Add existing sound folders");
    add.onClick = [this] { chooseFolder(); };
    rescan.onClick = [this] { library.rescan(); };
    rescan.setTooltip("Reconcile all sources. Unchanged audio headers are reused.");
    play.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff334c61));
    play.onClick = [this] { toggle(); };
    restart.onClick = [this] { audio.restart(); };
    output.setTitle("Audio output device");
    output.setTextWhenNothingSelected(juce::String::fromUTF8("Starting audio…"));
    output.onChange = [this]
    {
        if (!updating)
            audio.device(output.getText());
    };
    gain.setSliderStyle(juce::Slider::LinearHorizontal);
    gain.setRange(0, 1, 0.01);
    gain.setValue(0.7);
    gain.setTextBoxStyle(juce::Slider::TextBoxRight, false, 48, 24);
    gain.setTitle("Audition volume");
    gain.setTextValueSuffix("");
    gain.textFromValueFunction = [](double v) { return juce::String((int)(v * 100)) + "%"; };
    gain.updateText();
    gain.onValueChange = [this] { audio.gain((float)gain.getValue()); };
    volumeLabel.setText("Listen", juce::dontSendNotification);
    auto &h = table.getHeader();
    h.addColumn("RECORDING / LOCATION", 1, 440, 200, 2000, juce::TableHeaderComponent::visible);
    h.addColumn("DURATION", 2, 90, 70, 140, juce::TableHeaderComponent::visible);
    h.addColumn("CHANNELS", 3, 85, 70, 140, juce::TableHeaderComponent::visible);
    h.addColumn("RATE", 4, 90, 70, 140, juce::TableHeaderComponent::visible);
    h.addColumn("STATUS", 5, 120, 90, 180, juce::TableHeaderComponent::visible);
    table.setRowHeight(58);
    table.setHeaderHeight(34);
    table.setMultipleSelectionEnabled(false);
    table.setOutlineThickness(0);
    sources.setRowHeight(70);
    sources.setOutlineThickness(0);
    sources.selectRow(0);
    status.setText(juce::String::fromUTF8("Opening local catalog…"), juce::dontSendNotification);
    detail.setText("Choose a recording to listen", juce::dontSendNotification);
    summary.setText("Your sounds, in place. No project needed.", juce::dontSendNotification);
    setSize(1200, 780);
    startTimerHz(10);
}
LibraryView::~LibraryView()
{
    stopTimer();
    table.setModel(nullptr);
    sources.setModel(nullptr);
    setLookAndFeel(nullptr);
}
void LibraryView::paint(juce::Graphics &g)
{
    g.fillAll(bg);
    g.setColour(panel);
    g.fillRect(0, 84, 238, getHeight() - 84);
    g.setColour(line);
    g.drawHorizontalLine(83, 0, (float)getWidth());
    g.drawVerticalLine(238, 84, (float)getHeight());
    g.drawHorizontalLine(getHeight() - 176, 238, (float)getWidth());
    g.setColour(text);
    g.setFont(juce::FontOptions(23.0f, juce::Font::bold));
    g.drawText("SOUND+", 24, 19, 175, 30, juce::Justification::centredLeft);
    g.setColour(muted);
    g.setFont(juce::FontOptions(12.0f));
    g.drawText("SOUND LIBRARY", 25, 49, 180, 18, juce::Justification::centredLeft);
    g.setColour(text);
    g.setFont(juce::FontOptions(21.0f));
    g.drawText("Library", 266, 19, 110, 30, juce::Justification::centredLeft);
    g.setColour(muted);
    g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
    g.drawText("SOURCES", 22, 106, 170, 20, juce::Justification::centredLeft);
    g.setFont(juce::FontOptions(12.0f));
    g.drawText("Original files stay where they are.", 22, getHeight() - 102, 200, 20,
               juce::Justification::centredLeft);
    g.drawText(juce::String::fromUTF8("⌘F  Search     ↓  Results"), 22, getHeight() - 76, 205, 20,
               juce::Justification::centredLeft);
    g.drawText("Space  Play / stop", 22, getHeight() - 53, 200, 20, juce::Justification::centredLeft);
}
void LibraryView::paintOverChildren(juce::Graphics &g)
{
    if (snapshot.results.empty())
    {
        g.setColour(text);
        g.setFont(juce::FontOptions(22.0f));
        g.drawText(search.isEmpty() ? "A home for your sound library" : "No matching recordings", 270, 260,
                   getWidth() - 310, 36, juce::Justification::centred);
        g.setColour(muted);
        g.setFont(juce::FontOptions(15.0f));
        g.drawText(search.isEmpty() ? "Add a folder to discover and audition its recordings."
                                    : "Try a shorter filename or choose All recordings.",
                   270, 305, getWidth() - 310, 30, juce::Justification::centred);
    }
}
void LibraryView::resized()
{
    add.setBounds(getWidth() - 162, 24, 136, 35);
    rescan.setBounds(142, 101, 76, 28);
    summary.setBounds(264, 50, getWidth() - 450, 24);
    search.setBounds(264, 103, getWidth() - 292, 44);
    sources.setBounds(12, 143, 216, getHeight() - 265);
    table.setBounds(253, 165, getWidth() - 277, getHeight() - 353);
    int fixed = 90 + 85 + 90 + 120;
    table.getHeader().setColumnWidth(1, std::max(200, table.getWidth() - fixed - 18));
    int y = getHeight() - 162;
    detail.setBounds(266, y, getWidth() - 460, 26);
    play.setBounds(getWidth() - 168, y, 140, 37);
    assetId.setBounds(266, y + 28, getWidth() - 300, 25);
    transport.setBounds(266, y + 57, getWidth() - 300, 24);
    volumeLabel.setBounds(266, y + 91, 48, 25);
    gain.setBounds(312, y + 91, 175, 25);
    output.setBounds(510, y + 91, 230, 28);
    restart.setBounds(755, y + 91, 150, 28);
    status.setBounds(264, getHeight() - 30, getWidth() - 290, 25);
}
const Asset *LibraryView::selected() const
{
    auto n = table.getSelectedRow();
    return juce::isPositiveAndBelow(n, (int)snapshot.results.size()) ? &snapshot.results[(size_t)n] : nullptr;
}
void LibraryView::paintRowBackground(juce::Graphics &g, int row, int w, int h, bool isSelected)
{
    g.fillAll(isSelected ? juce::Colour(0xff253d52) : (row % 2 ? juce::Colour(0xff151b22) : bg));
    g.setColour(line.withAlpha(0.45f));
    g.drawHorizontalLine(h - 1, 0, (float)w);
    if (isSelected)
    {
        g.setColour(accent);
        g.fillRect(0, 0, 3, h);
    }
}
void LibraryView::paintCell(juce::Graphics &g, int row, int column, int w, int h, bool)
{
    if (!juce::isPositiveAndBelow(row, (int)snapshot.results.size()))
        return;
    const auto &a = snapshot.results[(size_t)row];
    g.setColour(a.online ? text : muted);
    g.setFont(juce::FontOptions(14.0f));
    juce::String value;
    if (column == 1)
    {
        g.drawText(a.name, 12, 6, w - 24, 23, juce::Justification::centredLeft, true);
        g.setFont(juce::FontOptions(11.5f));
        g.setColour(muted);
        g.drawText(a.path, 12, 29, w - 24, 20, juce::Justification::centredLeft, true);
        return;
    }
    if (column == 2)
        value = duration(a.duration);
    if (column == 3)
        value =
            a.channels > 0
                ? (a.channels == 1 ? "Mono" : (a.channels == 2 ? "Stereo" : juce::String(a.channels) + " ch"))
                : juce::String::fromUTF8("—");
    if (column == 4)
        value =
            a.sampleRate > 0 ? juce::String(a.sampleRate / 1000.0, 1) + " kHz" : juce::String::fromUTF8("—");
    if (column == 5)
    {
        value = !a.online             ? "Offline"
                : a.state == "ready"  ? "Ready"
                : a.state == "failed" ? "Unreadable"
                                      : "Reading header";
        g.setColour(a.online ? (a.state == "failed" ? amber : accent) : amber);
    }
    g.drawText(value, 8, 0, w - 12, h, juce::Justification::centredLeft, true);
}
void LibraryView::selectedRowsChanged(int)
{
    if (updating)
        return;
    if (auto *a = selected())
    {
        selectedId = a->id;
        detail.setText(a->name, juce::dontSendNotification);
        assetId.setText("Asset " + a->id + juce::String::fromUTF8("  •  Revision ") +
                            juce::String(a->revision) +
                            (a->error.isEmpty() ? "" : juce::String::fromUTF8("  •  ") + a->error),
                        juce::dontSendNotification);
        detail.setTooltip(a->path);
        assetId.setTooltip(a->path + "\n" + a->error);
    }
}
void LibraryView::cellDoubleClicked(int, int, const juce::MouseEvent &)
{
    toggle();
}
void LibraryView::paintListBoxItem(int n, juce::Graphics &g, int w, int h, bool chosen)
{
    if (chosen)
    {
        g.setColour(juce::Colour(0xff253d52));
        g.fillRoundedRectangle(0, 4, (float)w, (float)h - 8, 6);
    }
    g.setColour(text);
    g.setFont(juce::FontOptions(14.0f));
    if (n == 0)
    {
        g.drawText("All recordings", 12, 12, w - 24, 22, juce::Justification::centredLeft);
        g.setColour(muted);
        g.setFont(juce::FontOptions(12.0f));
        g.drawText("One library across every folder", 12, 35, w - 24, 20, juce::Justification::centredLeft);
        return;
    }
    if (!juce::isPositiveAndBelow(n - 1, (int)snapshot.roots.size()))
        return;
    const auto &r = snapshot.roots[(size_t)n - 1];
    g.drawText(juce::File(r.path).getFileName(), 12, 10, w - 24, 23, juce::Justification::centredLeft, true);
    g.setColour(!r.online ? amber : muted);
    g.setFont(juce::FontOptions(12.0f));
    auto label = !r.online              ? "Offline"
                 : r.scanning           ? juce::String::fromUTF8("Scanning…")
                 : r.error.isNotEmpty() ? "Check folder access"
                                        : "Online";
    g.drawText(juce::String(label) + juce::String::fromUTF8("  ·  ") + juce::String(r.discovered) + " files",
               12, 35, w - 24, 22, juce::Justification::centredLeft, true);
}
void LibraryView::listBoxItemClicked(int n, const juce::MouseEvent &)
{
    rootFilter = n > 0 ? snapshot.roots[(size_t)n - 1].id : juce::String();
    requestSearch();
}
void LibraryView::requestSearch()
{
    requested = library.search(search.getText(), rootFilter);
}
bool LibraryView::keyPressed(const juce::KeyPress &key, juce::Component *)
{
    if (key.getModifiers().isCommandDown() && (key.isKeyCode('F') || key.isKeyCode('f')))
    {
        search.grabKeyboardFocus();
        search.selectAll();
        return true;
    }
    if (search.hasKeyboardFocus(true) &&
        (key.isKeyCode(juce::KeyPress::downKey) || key.isKeyCode(juce::KeyPress::escapeKey)))
    {
        table.grabKeyboardFocus();
        if (table.getSelectedRow() < 0 && !snapshot.results.empty())
            table.selectRow(0);
        return true;
    }
    if (key.isKeyCode(juce::KeyPress::spaceKey) && !search.hasKeyboardFocus(true))
    {
        toggle();
        return true;
    }
    return false;
}
void LibraryView::chooseFolder()
{
    chooser = std::make_unique<juce::FileChooser>(
        "Add sound folders", juce::File::getSpecialLocation(juce::File::userHomeDirectory), "", true);
    juce::Component::SafePointer<LibraryView> safe(this);
    chooser->launchAsync(juce::FileBrowserComponent::openMode |
                             juce::FileBrowserComponent::canSelectDirectories |
                             juce::FileBrowserComponent::canSelectMultipleItems,
                         [safe](const juce::FileChooser &fc)
                         {
                             if (safe)
                                 for (const auto &f : fc.getResults())
                                     safe->library.addFolder(f);
                         });
}
void LibraryView::toggle()
{
    auto state = audio.status();
    const bool isPlaying = (bool)state["playing"];
    const auto *a = selected();
    if (isPlaying && (!a || state["id"].toString() == a->id))
    {
        audio.stop();
        return;
    }
    if (!a)
        return;
    if (!a->online)
    {
        transport.setText(
            juce::String::fromUTF8("Offline recording — reconnect its source folder to audition."),
            juce::dontSendNotification);
        return;
    }
    playingName = a->name;
    playingId = a->id;
    audio.play(a->id, juce::File(a->path));
}
void LibraryView::timerCallback()
{
    auto next = library.snapshot();
    if (next.request == requested && (next.version != appliedVersion || next.request != appliedRequest))
    {
        auto scroll = table.getViewport()->getViewPosition();
        snapshot = std::move(next);
        appliedVersion = snapshot.version;
        appliedRequest = snapshot.request;
        updating = true;
        table.updateContent();
        int row = -1;
        for (size_t i = 0; i < snapshot.results.size(); ++i)
            if (snapshot.results[i].id == selectedId)
                row = (int)i;
        if (row >= 0)
            table.selectRow(row, true, false);
        else
            table.deselectAllRows();
        table.getViewport()->setViewPosition(scroll);
        sources.updateContent();
        int sourceRow = 0;
        for (size_t i = 0; i < snapshot.roots.size(); ++i)
            if (snapshot.roots[i].id == rootFilter)
                sourceRow = (int)i + 1;
        sources.selectRow(sourceRow, true, false);
        updating = false;
        selectedRowsChanged(row);
        table.repaint();
        sources.repaint();
        repaint();
        int total = 0, ready = 0, failed = 0, offline = 0;
        bool scanning = false;
        juce::String sourceError;
        for (auto &r : snapshot.roots)
        {
            total += r.discovered;
            ready += r.ready;
            failed += r.failed;
            if (!r.online)
                ++offline;
            scanning |= r.scanning;
            if (r.error.isNotEmpty())
                sourceError = r.error;
        }
        summary.setText(juce::String((int)snapshot.results.size()) +
                            (snapshot.results.size() == 1000 ? "+ results" : " results") +
                            juce::String::fromUTF8("  ·  ") + juce::String(total) +
                            juce::String::fromUTF8(" indexed  ·  ") + juce::String(ready) + " ready" +
                            (scanning ? juce::String::fromUTF8("  ·  Scanning…") : ""),
                        juce::dontSendNotification);
        status.setText(snapshot.error.isNotEmpty() ? snapshot.error
                       : sourceError.isNotEmpty()
                           ? sourceError
                           : juce::String(failed) + juce::String::fromUTF8(" unreadable  ·  ") +
                                 juce::String(offline) + juce::String::fromUTF8(" offline folders  ·  ") +
                                 juce::String(snapshot.headersRead) + " headers read this session",
                       juce::dontSendNotification);
    }
    else if (next.error.isNotEmpty())
        status.setText(next.error, juce::dontSendNotification);
    auto state = audio.status();
    bool isPlaying = (bool)state["playing"];
    play.setButtonText(isPlaying ? "Stop  [Space]" : "Play  [Space]");
    auto err = state["error"].toString();
    if (err.isNotEmpty())
        transport.setText(err, juce::dontSendNotification);
    else if (isPlaying)
        transport.setText("Playing  " + playingName + "   " + duration((double)state["seconds"]) +
                              juce::String::fromUTF8("   •   ") + state["device"].toString(),
                          juce::dontSendNotification);
    else if (playingId.isNotEmpty())
        transport.setText(juce::String::fromUTF8("Stopped  •  ") + playingName, juce::dontSendNotification);
    updating = true;
    if (auto *options = state["outputs"].getArray())
    {
        juce::StringArray list;
        for (auto &v : *options)
            list.add(v.toString());
        juce::StringArray existing;
        for (int i = 0; i < output.getNumItems(); ++i)
            existing.add(output.getItemText(i));
        if (list != existing)
        {
            output.clear(juce::dontSendNotification);
            output.addItemList(list, 1);
        }
        output.setText(state["device"].toString(), juce::dontSendNotification);
    }
    updating = false;
}
} // namespace sp
