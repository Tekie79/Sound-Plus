#pragma once
#include "DesignSession.h"
#include "TrackModel.h"
#include "CanvasEditing.h"
#include "DesignControls.h"
namespace sp
{
// One component per expanded owner, never one component per curve.
class AutomationEditor final : public juce::Component
{
  public:
    AutomationEditor(DesignSession &, juce::String owner, TimelineViewport &, SnapSettings &);
    ~AutomationEditor() override;
    void refresh();
    void resized() override;
    void paint(juce::Graphics &) override;
    void mouseDown(const juce::MouseEvent &) override;
    void mouseDrag(const juce::MouseEvent &) override;
    void mouseUp(const juce::MouseEvent &) override;
    void mouseDoubleClick(const juce::MouseEvent &) override;
    bool keyPressed(const juce::KeyPress &) override;
    void focusLost(FocusChangeType) override;
    void chooseTarget();
    bool repairHostedTarget(const AutomationTarget &from, const AutomationTarget &to, bool confirmed,
                            std::optional<HostedAutomationBinding> expected = {});
    void insertPoint(int64_t, double);
    void numericPoint(int64_t, double);
    void deletePoints();
    void copyPoints();
    void pastePoints(int64_t);
    void offsetPoints(double);
    void setHold(bool);
    void beginTouch();
    void touchValue(double);
    void endTouch();
    void finishInteraction();
    bool contextValid() const;
    const juce::String owner;
    std::set<juce::String> selection;

  private:
    DesignSession &session;
    TimelineViewport &viewport;
    SnapSettings &snap;
    const uint64_t incarnation;
    const juce::String documentId, variation;
    ui::TextActionButton target{"Volume"}, read{"Read", true}, touch{"Touch", true};
    juce::Slider write;
    juce::Label valueLabel;
    AutomationParameter parameter, capturedParameter;
    AutomationTarget capturedTarget;
    DesignDocument original;
    juce::Point<float> anchor;
    bool dragging = false, range = false, touching = false, updating = false, hostedWriting = false;
    std::vector<AutomationPoint> touchPoints;
    int64_t firstTouch = 0;
    AutomationLane *lane();
    AutomationCurve *curve();
    juce::Point<float> position(const AutomationPoint &) const;
    double valueAt(float) const;
    int pointAt(juce::Point<float>) const;
    int64_t frameAt(float, bool bypass = false) const;
    void numericDialog();
    void requestRepair();
};
} // namespace sp
