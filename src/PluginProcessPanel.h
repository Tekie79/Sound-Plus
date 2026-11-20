#pragma once
#include "PluginSession.h"
#include "DesignControls.h"
#include "WaveformPreview.h"
#include "AudioIPC.h"
namespace sp
{
class PluginProcessPanel final : public juce::Component, private juce::Timer, private juce::KeyListener
{
  public:
    PluginProcessPanel(PluginInput, juce::File workspace, AudioClient &, std::shared_ptr<WaveformPool>,
                       juce::File helper = {},
                       std::vector<juce::File> automaticRoots = defaultVst3Locations());
    ~PluginProcessPanel() override;
    PluginSession session;
    std::function<void(const juce::File &)> onPrint, onIndex;
    std::function<void()> beforeAudition;
    void paint(juce::Graphics &) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress &) override;
    // The same production commands are exercised by component/session tests.
    void prepareInput();
    void refresh();
    void audition(bool processed);
    void stopAudition();
    void updateAudition();
    IconButton transport{"Play processed audition (Space)", ActionIcon::play};
    juce::ComboBox interpretation, plugins;
    juce::TextEditor subset, search;
    juce::ToggleButton mean{"All-channel Mono mean"}, bypass{"Bypass"}, normalize{"Normalize to -1 dB"},
        split{"Split mono"};
    ui::TextActionButton prepare{"Prepare input"}, scan{"Scan"}, retry{"Retry quarantined"},
        location{"Choose VST3 folder"}, preview{"Preview"}, compare{"Audition Processed"},
        copy{"Create Copy"}, print{"Print to Design"}, index{"Add to Library"}, cancel{"Cancel Processing"},
        undo{"Undo"}, redo{"Redo"}, reset{"Reset"}, save{"Save job"}, open{"Open job"};
    juce::Slider tail;
    juce::OwnedArray<juce::Slider> parameterSliders;

  private:
    PluginInput selected;
    AudioClient &audio;
    std::shared_ptr<WaveformPool> pool;
    ui::LookAndFeel look;
    juce::Label heading, inputSummary, outputSummary, status, physicalLabel, roleLabel, tailLabel,
        parameterSummary;
    juce::Viewport parameterViewport;
    juce::Component parameterContent;
    juce::OwnedArray<juce::Label> parameterNames;
    ui::FileDragButton drag{"Drag processed WAV"};
    std::unique_ptr<juce::FileChooser> chooser;
    std::vector<juce::var> choices;
    std::vector<juce::File> roots;
    juce::var lastParameters;
    juce::String waveKey, bindingKey, parameterKey, inputWaveState = "Prepare input",
                                                    outputWaveState = "Render Preview";
    juce::String auditionId, auditionHash, auditionBinding, auditionState = "Audition idle";
    uint64_t auditionGeneration = 0;
    juce::Label auditionFeedback;
    Asset inputAsset, outputAsset;
    std::shared_ptr<const PeakColumns> inputPeaks, outputPeaks;
    bool updating = false, processedPlaying = true, auditionWasActive = false;
    void timerCallback() override;
    void populatePlugins();
    void rebuildParameters();
    void chooseLocation();
    void jobFile(bool writing);
    bool waveformBindingNeedsRefresh() const;
    bool keyPressed(const juce::KeyPress &, juce::Component *) override;
    bool keyStateChanged(bool, juce::Component *) override;
    bool spaceHeld = false, spaceApplied = false;
};
} // namespace sp
