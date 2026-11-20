#pragma once
#include "PluginProcessing.h"
#include <juce_gui_basics/juce_gui_basics.h>
namespace sp
{
class PluginSession final : public juce::Component
{
  public:
    PluginSession(juce::File helper, juce::File directory);
    ~PluginSession() override;
    juce::var input, settings, result, receipt, discovery;
    juce::String status = "Choose input interpretation, then Prepare input", error;
    bool busy = false, deliverySplit = false, deliveryNormalize = false;
    uint64_t generation = 0;
    std::function<void()> onChange;
    std::function<bool()> contextValid = [] { return true; };
    // Optional worker-thread progress observer; also permits controlled cancellation QA.
    std::function<void()> deliveryStaged;
    void clearInput();
    void prepare(PluginInput);
    void recallCatalog(std::vector<juce::File> standardRoots = defaultVst3Locations());
    void scan(std::vector<juce::File>, bool retry = false);
    void choose(juce::var plugin);
    void change(const juce::var &);
    void beginGesture();
    void endGesture();
    void undo();
    void redo();
    void reset();
    void cancel();
    void setNormalization(bool);
    void preview();
    void createCopy(juce::File, bool normalize = false, bool split = false);
    void save(const juce::File &);
    void open(const juce::File &);
    juce::File directory;
    bool canUndo() const
    {
        return !past.empty();
    }
    bool canRedo() const
    {
        return !future.empty();
    }
    bool ready() const
    {
        return result.isObject() && !busy && contextValid();
    }
    bool canCancel() const
    {
        return busy && !verifyingCancellation;
    }

  private:
    SerialWorker worker;
    PluginWorker child;
    std::shared_ptr<std::atomic<uint64_t>> epoch = std::make_shared<std::atomic<uint64_t>>(0);
    std::vector<juce::var> past, future;
    juce::var gesture;
    bool registeredDiscovery = false;
    bool retainingPass = false;
    bool verifyingCancellation = false;
    void invalidate();
    void notify();
    void launch(const juce::String &, std::function<juce::var(const std::function<bool()> &)>,
                std::function<void(juce::var)>, bool retainPass = false);
};
} // namespace sp
