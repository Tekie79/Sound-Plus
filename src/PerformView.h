#pragma once
#include "DesignSession.h"
#include "PerformanceState.h"
#include "DesignControls.h"
namespace sp
{
class PerformView final : public juce::Component, public juce::DragAndDropTarget
{
  public:
    explicit PerformView(DesignSession &);
    void refresh();
    void resized() override;
    void paint(juce::Graphics &) override;
    bool keyPressed(const juce::KeyPress &) override;
    bool isInterestedInDragSource(const SourceDetails &) override;
    void itemDropped(const SourceDetails &) override;
    void benchmarkControls(int phase, double value);

  private:
    friend struct PerformanceViewTestAccess;
    struct Pad : juce::Component
    {
        PerformView &owner;
        explicit Pad(PerformView &);
        void paint(juce::Graphics &) override;
        void mouseDown(const juce::MouseEvent &) override;
        void mouseDrag(const juce::MouseEvent &) override;
        void mouseUp(const juce::MouseEvent &) override;
        bool keyPressed(const juce::KeyPress &) override;
    } pad;
    struct LayerDrag : IconButton
    {
        PerformView &owner;
        explicit LayerDrag(PerformView &);
        void mouseDrag(const juce::MouseEvent &) override;
    } layerDrag;
    DesignSession &session;
    bool updating = false, mappingOpen = false, controlGesture = false, staleControl = false;
    int selectedMacro = 0, targetSerial = 100;
    juce::String identity, gestureIdentity, assignmentSignature;
    DesignSession::DialogToken gestureToken;
    std::array<DesignSession::DialogToken, 4> macroNameTokens, assignmentNameTokens;
    std::array<juce::String, 4> macroNameContexts, assignmentNameContexts;
    std::vector<std::pair<int, PerformanceTargetInfo>> targetItems;
    std::vector<std::pair<int, juce::String>> layerItems;
    PerformanceTarget editingTarget;
    std::array<juce::ComboBox, 4> assignments;
    std::array<juce::Label, 4> assignmentNames, macroNames, macroFeedback;
    std::array<juce::Slider, 4> macros;
    std::array<std::unique_ptr<IconButton>, 4> macroEnable, macroReset, macroMap;
    juce::Slider xValue, yValue;
    IconButton blendEnable{"Enable blend surface", ActionIcon::check},
        blendReset{"Centre blend surface", ActionIcon::history};
    juce::ComboBox layerSource;
    juce::Label blendState, mappingTitle, mappingFeedback, policy, minimumLabel, maximumLabel;
    IconButton back{"Back to macros", ActionIcon::previous},
        removeMapping{"Unassign target", ActionIcon::trash};
    juce::ComboBox mappingChoice, targetChoice, curve;
    juce::TextEditor minimum, maximum;
    juce::ToggleButton inverted{"Invert"};
    ui::TextActionButton addMapping{"Add mapping"};
    std::array<juce::Rectangle<int>, 4> slotBounds, macroBounds;
    juce::Rectangle<int> blendBounds, macroArea;
    void setPad(double, double);
    void beginControl();
    void endControl(bool cancel = false);
    bool controlValid() const;
    void openMapping(int);
    void updateMappingFields();
    void commitMappingFields();
    int slotAt(juce::Point<int>) const;
};
} // namespace sp
