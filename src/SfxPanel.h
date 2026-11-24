#pragma once
#include "SfxGeneration.h"
#include "DesignerView.h"
#include "LibraryView.h"

namespace sp::sfx
{
class Panel final : public juce::Component, private juce::Timer
{
  public:
    Panel(Generation &, LibraryView &, DesignerView &, std::function<void(int)> selectWorkspace);
    ~Panel() override;
    void setInput(TransformInput);
    void setPreparationStatus(juce::String);
    void resized() override;
    void paint(juce::Graphics &) override;

  private:
    Generation &jobs;
    LibraryView &library;
    DesignerView &designer;
    std::function<void(int)> navigate;
    juce::Label heading, model, historyHeading, status, detail, durationLabel, countLabel, influenceLabel;
    juce::Label descriptionHeading, settingsHeading, outputLabel, destinationLabel;
    ui::TextActionButton manageKey{"Manage key", true};
    std::array<juce::Rectangle<int>, 4> sectionBounds{};
    void layoutGeneration();
    void refreshGenerationLayout();
    juce::TextEditor prompt, key;
    juce::ComboBox duration, count, format, history, collection;
    juce::Slider influence;
    juce::ToggleButton loop{"Loop"};
    ui::TextActionButton saveKey{"Save key"}, test{"Test key"}, erase{"Delete key"}, generate{"Generate"},
        cancel{"Cancel"}, preview{"Preview"}, save{"Save to Library"}, edit{"Insert in Edit"},
        perform{"Assign to Pad"}, reveal{"Show in Library"};
    std::unique_ptr<juce::FileChooser> chooser;
    Snapshot state;
    uint64_t seen = UINT64_MAX, previewGeneration = 0;
    juce::String selected, previewId;
    std::vector<std::pair<juce::String, juce::String>> collections;
    void timerCallback() override;
    void updateControls();
    const Candidate *current() const;
    bool transforming = false;
    std::optional<TransformInput> input;
    juce::TextEditor maskIn, maskOut, crossfade;
    juce::ComboBox editMode;
    juce::Label inputSummary, inLabel, outLabel, fadeLabel;
    ui::TextActionButton original{"Original"}, resume{"Resume result"}, alternative{"Add alternative"};
    void previewOriginal();
    void approve();
    void chooseFolder();
    void use(int action, int slot = -1, juce::File folder = {});
    void padMenu();
};
} // namespace sp::sfx
