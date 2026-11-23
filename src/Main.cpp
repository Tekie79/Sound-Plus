#include "LibraryView.h"
#include "MacSources.h"
#include "Benchmark.h"
#include "Performance.h"
#include "Workstation.h"
class SoundPlusApplication final : public juce::JUCEApplication
{
  public:
    const juce::String getApplicationName() override
    {
        return "SoundPlus";
    }
    const juce::String getApplicationVersion() override
    {
        return "0.1.0";
    }
    bool moreThanOneInstanceAllowed() override
    {
        return true;
    }
    void initialise(const juce::String &) override
    {
        auto args = getCommandLineParameterArray();
        auto planAt = args.indexOf("--benchmark-plan");
        if (planAt >= 0 && planAt + 1 < args.size())
        {
            auto p = juce::JSON::parse(juce::File(args[planAt + 1]));
            sp::performance.delayedRoot = p["delayedRoot"].toString();
            sp::performance.delayMs = (int)p["delayMs"];
            sp::performance.expectedFiles = (juce::int64)p["expectedFiles"];
        }
        auto dir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                       .getChildFile("Application Support")
                       .getChildFile("SoundPlus");
        auto at = args.indexOf("--data-dir");
        if (at >= 0 && at + 1 < args.size())
            dir = juce::File(args[at + 1]);
        instanceLock = std::make_unique<juce::InterProcessLock>(
            "SoundPlus-" + juce::String(dir.getFullPathName().hashCode64()));
        if (!instanceLock->enter(0))
        {
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon, "SoundPlus is already open",
                "This catalog is already in use by another SoundPlus window.");
            quit();
            return;
        }
        bool enableAI = !args.contains("--disable-ai");
        if (planAt >= 0 && planAt + 1 < args.size())
            enableAI =
                juce::JSON::parse(juce::File(args[planAt + 1]))["mode"].toString().startsWith("semantic");
        juce::File modelDirectory;
        auto modelAt = args.indexOf("--model-dir");
        if (modelAt >= 0 && modelAt + 1 < args.size())
            modelDirectory = juce::File(args[modelAt + 1]);
        window = std::make_unique<Window>(dir.getChildFile("library.sqlite"), enableAI, modelDirectory,
                                          planAt >= 0);
        sp::activateDesktopApplication();
        window->toFront(true);
        for (int i = 0; i + 1 < args.size(); ++i)
            if (args[i] == "--add-folder")
                window->view->addFolder(juce::File(args[++i]));
        auto outputAt = args.indexOf("--benchmark-output");
        if (planAt >= 0 && outputAt >= 0)
        {
            auto plan = juce::JSON::parse(juce::File(args[planAt + 1]));
            if (plan["mode"].toString().startsWith("semantic-designer") ||
                (plan["mode"].toString() == "native-effects" ||
                 plan["mode"].toString() == "advanced-effects"))
            {
                window->workstation->showDesigner(true);
                window->workstation->designer.session.open(juce::File(plan["designFile"].toString()));
            }
            benchmark = std::make_unique<sp::Benchmark>(*window->view, juce::File(args[planAt + 1]),
                                                        juce::File(args[outputAt + 1]),
                                                        &window->workstation->designer);
        }
    }
    void shutdown() override
    {
        benchmark.reset();
        window.reset();
        if (instanceLock)
            instanceLock->exit();
        instanceLock.reset();
    }
    void systemRequestedQuit() override
    {
        if (window)
        {
            window->workstation->designer.commitNames();
            window->workstation->designer.session.closeWhenSafe([this] { quit(); });
        }
        else
            quit();
    }
    void anotherInstanceStarted(const juce::String &) override {}

  private:
    struct Window : juce::DocumentWindow
    {
        sp::LibraryView *view = nullptr;
        Workstation *workstation = nullptr;
        explicit Window(const juce::File &db, bool enableAI, const juce::File &modelDirectory, bool benchmark)
            : DocumentWindow("SoundPlus", juce::Colour(0xff101419), DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar(true);
            workstation = new Workstation(db, enableAI, modelDirectory, benchmark);
            view = &workstation->library;
            setContentOwned(workstation, true);
            setResizable(true, true);
            setResizeLimits(sp::ui::workspace::minimumWindowWidth, sp::ui::workspace::minimumWindowHeight,
                            3200, 2200);
            centreWithSize(1360, 860);
            if (!benchmark)
                restoreWindowStateFromString(workstation->preferences.getValue("window"));
            setVisible(true);
            toFront(true);
        }
        ~Window() override
        {
            if (workstation)
                workstation->preferences.setValue("window", getWindowStateAsString());
        }
        void closeButtonPressed() override
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }
    };
    std::unique_ptr<Window> window;
    std::unique_ptr<sp::Benchmark> benchmark;
    std::unique_ptr<juce::InterProcessLock> instanceLock;
};
START_JUCE_APPLICATION(SoundPlusApplication)
