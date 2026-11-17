#include "LibraryView.h"
#include "MacSources.h"
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
        window = std::make_unique<Window>(dir.getChildFile("library.sqlite"));
        sp::activateDesktopApplication();
        window->toFront(true);
        for (int i = 0; i + 1 < args.size(); ++i)
            if (args[i] == "--add-folder")
                window->view->addFolder(juce::File(args[++i]));
    }
    void shutdown() override
    {
        window.reset();
        if (instanceLock)
            instanceLock->exit();
        instanceLock.reset();
    }
    void systemRequestedQuit() override
    {
        quit();
    }
    void anotherInstanceStarted(const juce::String &) override {}

  private:
    struct Window : juce::DocumentWindow
    {
        sp::LibraryView *view = nullptr;
        explicit Window(const juce::File &db)
            : DocumentWindow("SoundPlus", juce::Colour(0xff101419), DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar(true);
            view = new sp::LibraryView(db, juce::File::getSpecialLocation(juce::File::currentExecutableFile));
            setContentOwned(view, true);
            setResizable(true, true);
            setResizeLimits(1080, 650, 2600, 1800);
            centreWithSize(1200, 780);
            setVisible(true);
            toFront(true);
        }
        void closeButtonPressed() override
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }
    };
    std::unique_ptr<Window> window;
    std::unique_ptr<juce::InterProcessLock> instanceLock;
};
START_JUCE_APPLICATION(SoundPlusApplication)
