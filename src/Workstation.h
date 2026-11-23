#pragma once
#include "LibraryView.h"
#include "DesignerView.h"
#include "PluginProcessPanel.h"
#include "WorkspaceSizing.h"
#include "DeliveryWorkspace.h"
#include "WorkspaceCommands.h"
class Workstation final : public juce::Component,public juce::DragAndDropContainer
{
  public:
    juce::PropertiesFile preferences;
    sp::LibraryView library;
    sp::DesignerView designer;
    Workstation(const juce::File &db,bool enableAI,const juce::File &modelDirectory,bool benchmark)
        : preferences(db.getParentDirectory().getChildFile("interface.settings"),sp::ui::preferenceOptions()),
          library(db,juce::File::getSpecialLocation(juce::File::currentExecutableFile),enableAI,modelDirectory),
          designer(db,library.audioClient(),preferences,library.waveformProvider()),delivery(designer),divider(*this)
    {
        library.onProcess=[this,db](const sp::Asset &a,int64_t first,int64_t last) { sp::PluginInput input; input.source=a;input.first=first;input.last=last;input.layout=a.layoutOverride.id.isNotEmpty()?a.layoutOverride:a.detectedChannels; if(a.channels<=16)for(int c=0;c<a.channels;++c)input.inputs.push_back(c); showProcessing(input,db); };
        designer.onProcessOutput=[this,db](const juce::File &file) { try { sp::PluginInput input;input.source=sp::inspectDesignSource(file);input.last=input.source.samples;input.layout=input.source.detectedChannels;input.scope="Finalized design/take output";for(int c=0;c<input.source.channels;++c)input.inputs.push_back(c);showProcessing(input,db); } catch(const std::exception &e) { designer.session.message=e.what(); } };
        library.bindPreferences(preferences);
        browserOpen=preferences.getBoolValue("browserOpen",true);
        addAndMakeVisible(library);
        addChildComponent(designer);
        addChildComponent(delivery);
        addChildComponent(divider);
        addAndMakeVisible(shell);
        shell.setLookAndFeel(&shellLook);
        setLookAndFeel(&shellLook);
        designer.useWorkspaceShell();
        designer.onWorkspaceChanged = [this]
        {
            if (workspace == Workspace::edit || workspace == Workspace::perform)
            {
                workspace = designer.inPerform() ? Workspace::perform : Workspace::edit;
                syncWorkspace();
            }
        };
        setupShell();
        applyAppearance(preferences.getBoolValue("lightAppearance"));
        library.onShowDesigner=[this]{showDesigner(true);};library.onBeforeAudition=[this]{designer.session.stop();};
        library.onAddToDesigner=[this](const sp::Asset &a,int64_t first,int64_t last,bool external,int pair){designer.addSource(a,first,last,external,pair);showDesigner(true);};
        designer.onLibrary=[this]{showDesigner(false);};
        designer.onToggleBrowser=[this]{browserOpen=!browserOpen;preferences.setValue("browserOpen",browserOpen);resized();};
        designer.onSearch=[this]{browserOpen=true;resized();library.focusSearch();};
        designer.session.onBeforePlay=[this]{library.parkPlayback();};
        library.audioClient().gain((float)preferences.getDoubleValue("monitorVolume",.7));
        const auto saved = preferences.getIntValue("workspace", preferences.getBoolValue("designerActive")
                                                                     ? (designer.inPerform() ? 2 : 1) : 0);
        chooseWorkspace(benchmark ? Workspace::library : (Workspace)std::clamp(saved, 0, 3));
    }
    ~Workstation() override
    {
        preferences.setValue("monitorVolume", library.audioClient().monitorGain());
        preferences.saveIfNeeded();
        shell.setLookAndFeel(nullptr);
        setLookAndFeel(nullptr);
    }
    void paint(juce::Graphics &g) override { g.fillAll(sp::ui::background); }
    void resized() override
    {
        constexpr int shelf = 44;
        shell.setBounds(0, 0, getWidth(), shelf);
        auto content = getLocalBounds().withTrimmedTop(shelf);
        const bool editing = workspace == Workspace::edit || workspace == Workspace::perform;
        const bool both = editing && browserOpen;
        library.setCompact(editing);
        designer.setBrowserVisible(both);
        library.setVisible(workspace == Workspace::library || both);
        designer.setVisible(editing);
        delivery.setVisible(workspace == Workspace::deliver);
        divider.setVisible(both);
        const int width = both ? sp::ui::workspace::sidebarWidth : 0;
        library.setBounds(both ? content.withWidth(width) : content);
        divider.setBounds(width, shelf, 6, content.getHeight());
        designer.setBounds(getLocalBounds().withTrimmedLeft(both ? width + 6 : 0));
        delivery.setBounds(content);
        shell.toFront(false);
    }
    void showDesigner(bool yes)
    {
        chooseWorkspace(yes ? (designer.inPerform() ? Workspace::perform : Workspace::edit) : Workspace::library);
    }
    bool keyPressed(const juce::KeyPress &key) override
    {
        if (key.getModifiers().isCommandDown())
        {
            auto command = juce::CharacterFunctions::toUpperCase((juce::juce_wchar)key.getKeyCode());
            if (command == 'K')
            {
                showCommands();
                return true;
            }
            if (command == 'S')
            {
                designer.saveFromShell();
                return true;
            }
            if (workspace != Workspace::library && !library.hasKeyboardFocus(true) &&
                (command == 'R' || command == 'Z' || command == 'D' || command == 'O' || command == 'N'))
                return designer.commandKey(key);
        }
        if (workspace == Workspace::deliver && key == juce::KeyPress::spaceKey &&
            !dynamic_cast<juce::TextEditor *>(getCurrentlyFocusedComponent()))
            return designer.commandKey(key);
        return false;
    }
    bool shouldDropFilesWhenDraggedExternally(const juce::DragAndDropTarget::SourceDetails &details,juce::StringArray &files,bool &canMove) override
    {
        try{auto d=sp::designFromJson(juce::JSON::parse(details.description.toString()));if(d.original.layers.size()!=1)return false;auto &l=d.original.layers.front();if(l.region.startSample!=0 || l.region.endSample!=l.source.samples || l.source.error.isNotEmpty() || !juce::File(l.source.path).existsAsFile())return false;files.add(l.source.path);canMove=false;return true;}catch(...){return false;}
    }
  private:
    struct ProcessWindow final : juce::DocumentWindow
    {
        std::function<void()> onClose;
        ProcessWindow():DocumentWindow("Process with Plug-in",sp::ui::background,juce::DocumentWindow::closeButton){setUsingNativeTitleBar(true);}
        void closeButtonPressed() override {if(onClose)onClose();}
    };
    std::unique_ptr<ProcessWindow> processWindow;
    void showProcessing(sp::PluginInput input,const juce::File &db)
    {
        if(designer.session.takeBusy())return;
        processWindow.reset();auto panel=std::make_unique<sp::PluginProcessPanel>(input,db.getParentDirectory().getChildFile("plugin-processing/"+juce::Uuid().toString()),library.audioClient(),library.waveformProvider());
        auto id=designer.session.document.id,variation=designer.session.document.activeVariation,take=designer.session.document.activeTake;auto revision=designer.session.document.revision;
        juce::Component::SafePointer<Workstation> safe(this);
        panel->session.contextValid=[safe,id,variation,take,revision] {return safe&&safe->designer.session.document.id==id&&safe->designer.session.document.activeVariation==variation&&safe->designer.session.document.activeTake==take&&safe->designer.session.document.revision==revision&&!safe->designer.session.takeBusy()&&!safe->designer.session.transitionPending();};
        panel->beforeAudition=[safe]{if(safe){safe->designer.session.stop();safe->library.parkPlayback();}};
        panel->onPrint=[safe](auto &file){if(safe&&!safe->designer.session.takeBusy()){if(safe->designer.session.document.activeTake.isNotEmpty())safe->designer.session.selectTake({});safe->designer.session.addExternal(file);safe->showDesigner(true);}};
        panel->onIndex=[safe](auto &folder){if(safe)safe->library.addFolder(folder);};
        processWindow=std::make_unique<ProcessWindow>();processWindow->onClose=[safe]{juce::MessageManager::callAsync([safe]{if(safe)safe->processWindow.reset();});};
        processWindow->setContentOwned(panel.release(),true);processWindow->centreWithSize(700,650);processWindow->setVisible(true);processWindow->toFront(true);
    }
    enum class Workspace { library, edit, perform, deliver };
    Workspace workspace = Workspace::library;
    bool browserOpen = true;
    sp::DeliveryWorkspace delivery;
    sp::ui::LookAndFeel shellLook;
    struct Shell final : juce::Component
    {
        sp::ui::TextActionButton library{"Library", true}, edit{"Edit", true}, perform{"Perform", true},
            deliver{"Deliver", true};
        sp::IconButton search{"Search commands", sp::ActionIcon::similar};
        sp::IconButton settings{"Settings", sp::ActionIcon::settings};
        sp::IconButton save{"Save design", sp::ActionIcon::save};
        std::array<sp::ui::TextActionButton *, 4> tabs{&library, &edit, &perform, &deliver};

        Shell()
        {
            setTitle("Workspaces");
            for (auto *tab : tabs)
                addAndMakeVisible(*tab);
            for (auto *button : {&save, &search, &settings})
                addAndMakeVisible(*button);
        }
        void resized() override
        {
            auto row = getLocalBounds().reduced(12, 6);
            auto tools = row.removeFromRight(106);
            save.setBounds(tools.removeFromLeft(34));
            search.setBounds(tools.removeFromLeft(34));
            settings.setBounds(tools.removeFromLeft(34));
            auto nav = row.withSizeKeepingCentre(296, row.getHeight());
            for (auto *tab : tabs)
            {
                tab->setBounds(nav.removeFromLeft(70));
                nav.removeFromLeft(4);
            }
        }
        void paint(juce::Graphics &g) override
        {
            g.fillAll(sp::ui::surface);
            g.setColour(sp::ui::line);
            g.drawHorizontalLine(getHeight() - 1, 0, (float)getWidth());
            g.setColour(sp::ui::text);
            g.setFont(juce::FontOptions(15.f, juce::Font::bold));
            g.drawText("SoundPlus", 16, 0, 140, getHeight(), juce::Justification::centredLeft);
        }
    } shell;

    void setupShell()
    {
        for (int i = 0; i < (int)shell.tabs.size(); ++i)
            shell.tabs[(size_t)i]->onClick = [this, i] { chooseWorkspace((Workspace)i); };
        shell.settings.onClick = [this] { showSettings(); };
        shell.search.onClick = [this] { showCommands(); };
        shell.save.onClick = [this] { designer.saveFromShell(); };
    }
    void chooseWorkspace(Workspace target)
    {
        if (workspace == Workspace::deliver && target != workspace)
            delivery.leave();
        const bool entry = target == Workspace::deliver && target != workspace;
        workspace = target;
        if (target == Workspace::edit || target == Workspace::perform)
            designer.showPerform(target == Workspace::perform);
        if (entry)
            delivery.enter();
        syncWorkspace();
        if (isShowing())
        {
            if (target == Workspace::library) library.grabKeyboardFocus();
            else if (target != Workspace::deliver) designer.grabKeyboardFocus();
        }
    }
    void syncWorkspace()
    {
        const auto index = (int)workspace;
        preferences.setValue("workspace", index);
        preferences.setValue("designerActive", workspace != Workspace::library);
        for (int i = 0; i < (int)shell.tabs.size(); ++i)
            shell.tabs[(size_t)i]->setToggleState(i == index, juce::dontSendNotification);
        resized();
    }
    void applyAppearance(bool light)
    {
        using namespace sp::ui;
        if (lightAppearance == light)
            return;
        auto &before = lightAppearance ? lightPalette : darkPalette;
        auto &after = light ? lightPalette : darkPalette;
        setAppearance(light);
        sp::icons::preload();
        std::set<LookAndFeel *> looks;
        auto visit = [&](auto &self, juce::Component &component) -> void
        {
            if (auto *look = dynamic_cast<LookAndFeel *>(&component.getLookAndFeel()))
                looks.insert(look);
            for (int id : {int(juce::Label::textColourId), int(juce::TextEditor::textColourId),
                           int(juce::Label::backgroundColourId), int(juce::TextEditor::backgroundColourId),
                           int(juce::TextEditor::outlineColourId),
                           int(juce::Slider::textBoxTextColourId), int(juce::Slider::trackColourId),
                           int(juce::ListBox::backgroundColourId), int(juce::ComboBox::textColourId)})
            {
                if (!component.isColourSpecified(id))
                    continue;
                auto colour = component.findColour(id);
                auto rgb = colour.withAlpha(1.f).getARGB();
                auto match = std::find(before.begin(), before.end(), rgb);
                if (match != before.end())
                {
                    auto themed = juce::Colour(after[(size_t)(match - before.begin())])
                                      .withAlpha(colour.getFloatAlpha());
                    if (auto *editor = dynamic_cast<juce::TextEditor *>(&component);
                        editor && id == juce::TextEditor::textColourId)
                        editor->applyColourToAllText(themed);
                    else
                        component.setColour(id, themed);
                }
            }
            for (auto *child : component.getChildren())
                self(self, *child);
        };
        visit(visit, *this);
        for (auto *look : looks)
            look->refreshPalette();
        sendLookAndFeelChange();
        preferences.setValue("lightAppearance", light);
    }
    void showCommands()
    {
        using Command = sp::ui::WorkspaceCommands::Command;
        juce::Component::SafePointer<Workstation> safe(this);
        std::vector<Command> commands;
        for (int i = 0; i < (int)shell.tabs.size(); ++i)
            commands.push_back({shell.tabs[(size_t)i]->getButtonText(),
                                [safe, i] { if (safe) safe->chooseWorkspace((Workspace)i); }});
        commands.push_back({"Find sounds", [safe]
            { if (safe) { safe->chooseWorkspace(Workspace::library); safe->library.focusSearch(); } }});
        commands.push_back({"Save design", [safe] { if (safe) safe->designer.saveFromShell(); }});
        commands.push_back({"Settings", [safe] { if (safe) safe->showSettings(); }});
        auto panel = std::make_unique<sp::ui::WorkspaceCommands>(std::move(commands), getCurrentlyFocusedComponent());
        auto *focus = panel.get();
        juce::CallOutBox::launchAsynchronously(std::move(panel), shell.getLocalArea(&shell.search, shell.search.getLocalBounds()), this);
        focus->focusSearch();
    }
    void showSettings()
    {
        struct Settings final : juce::Component
        {
            sp::ui::LookAndFeel look;
            juce::Label appearance, devices, shortcuts, version;
            sp::ui::TextActionButton dark{"Dark", true}, light{"Light", true};
            juce::ComboBox device;
            sp::ui::TextActionButton monitor{"Output mapping"};
            Settings()
            {
                setLookAndFeel(&look);
                setTitle("SoundPlus settings");
                for (auto *c : std::initializer_list<juce::Component *>{&appearance, &dark, &light, &devices,
                                                                       &device, &monitor, &shortcuts, &version})
                    addAndMakeVisible(c);
                appearance.setText("Appearance", juce::dontSendNotification);
                devices.setText("Audio output", juce::dontSendNotification);
                shortcuts.setText(juce::String::fromUTF8("⌘K Commands    ⌘S Save    Space Play / Stop"),
                                  juce::dontSendNotification);
                version.setText(juce::String::fromUTF8("SoundPlus · ") + juce::JUCEApplication::getInstance()->getApplicationVersion(),
                                juce::dontSendNotification);
                device.setTitle("Audio output");
                setSize(346, 206);
            }
            ~Settings() override { setLookAndFeel(nullptr); }
            void paint(juce::Graphics &g) override { g.fillAll(sp::ui::surface); }
            void resized() override
            {
                auto area = getLocalBounds().reduced(12);
                appearance.setBounds(area.removeFromTop(22));
                auto appearanceChoices = area.removeFromTop(30);
                dark.setBounds(appearanceChoices.removeFromLeft(82));
                light.setBounds(appearanceChoices.removeFromLeft(82));
                devices.setBounds(area.removeFromTop(25));
                auto output = area.removeFromTop(32);
                monitor.setBounds(output.removeFromRight(112));
                device.setBounds(output.withTrimmedRight(6));
                shortcuts.setBounds(area.removeFromTop(37));
                version.setBounds(area.removeFromTop(26));
            }
        };
        auto panel = std::make_unique<Settings>();
        auto *options = panel.get();
        auto selectAppearance = [this, options](bool light)
        {
            applyAppearance(light);
            options->dark.setToggleState(!light, juce::dontSendNotification);
            options->light.setToggleState(light, juce::dontSendNotification);
            options->look.refreshPalette();
            options->sendLookAndFeelChange();
        };
        selectAppearance(sp::ui::lightAppearance);
        options->dark.onClick = [selectAppearance] { selectAppearance(false); };
        options->light.onClick = [selectAppearance] { selectAppearance(true); };
        auto state = library.audioClient().status();
        if (auto *outputs = state["outputs"].getArray())
            for (const auto &value : *outputs)
                options->device.addItem(value.toString(), options->device.getNumItems() + 1);
        options->device.setTextWhenNothingSelected("Audio service starting");
        options->device.setText(state["device"].toString(), juce::dontSendNotification);
        options->device.onChange = [this, options] { library.audioClient().device(options->device.getText()); };
        options->monitor.onClick = [this, options]
        {
            if (auto *box = options->findParentComponentOfClass<juce::CallOutBox>())
                box->dismiss();
            chooseWorkspace(Workspace::edit);
            designer.monitorFromShell();
        };
        juce::CallOutBox::launchAsynchronously(std::move(panel), shell.settings.getBounds(), this);
    }
    struct Divider : juce::Component
    {
        explicit Divider(Workstation &){setTitle("Library sidebar divider");}
        void paint(juce::Graphics &g) override{g.fillAll(sp::ui::background);g.setColour(sp::ui::line);g.drawVerticalLine(2,12,(float)getHeight()-12);}
    } divider;
    juce::TooltipWindow tooltips{this,600};
};
