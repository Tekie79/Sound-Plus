#pragma once
#include "DesignSession.h"
#include "DesignControls.h"
#include "TrackModel.h"
#include <set>

namespace sp
{
class SurroundPanner final : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
  public:
    SurroundPanner(DesignSession &, juce::String owner, std::function<int64_t(int64_t)> snapTime,
                   std::function<bool(const juce::KeyPress &)> command);
    ~SurroundPanner() override;
    void resized() override;
    void paint(juce::Graphics &) override;
    void mouseMove(const juce::MouseEvent &) override;
    void mouseDown(const juce::MouseEvent &) override;
    void mouseDrag(const juce::MouseEvent &) override;
    void mouseUp(const juce::MouseEvent &) override;
    void mouseDoubleClick(const juce::MouseEvent &) override;
    void focusLost(FocusChangeType) override;
    bool keyPressed(const juce::KeyPress &) override;
    void finish(bool cancel = false);
    bool valid() const;

  private:
    DesignSession &session;
    const juce::String owner, doc, variation;
    const uint64_t incarnation;
    std::function<int64_t(int64_t)> snapTime;
    std::function<bool(const juce::KeyPress &)> command;
    ui::LookAndFeel look;
    juce::Label title, busLabel, timeLabel, xLabel, yLabel, spreadLabel, feedback;
    juce::TextEditor xValue, yValue;
    juce::Slider spread;
    ui::TextActionButton enable{"Enable XY", true}, touch{"Touch", true};
    IconButton add{"Add motion point at playhead", ActionIcon::marker},
        remove{"Delete selected motion points", ActionIcon::trash},
        options{"Motion options", ActionIcon::more};
    std::vector<SurroundNode> nodes, dragStartNodes;
    std::optional<int64_t> pathRevision;
    std::set<int64_t> selected, selectionAtDown;
    juce::Rectangle<float> field;
    juce::Point<float> anchor, pointer;
    std::optional<juce::MouseInputSource> dragSource;
    bool dragging = false, selecting = false, edited = false, updating = false;
    bool moved = false, writing = false, touchArmed = false;
    const DesignTrack *track() const;
    void timerCallback() override;
    void refresh();
    void motionFeedback(const juce::String &);
    void editTrack(const juce::String &, const std::function<void(DesignTrack &)> &);
    void editPosition(juce::Point<double>);
    void addPoint(juce::Point<double>, bool snap);
    void removePoints();
    void motionMenu();
    void numericPoint();
    void applyShape(MotionShape);
    void copy();
    void paste();
    void finishDrag(bool cancel = false);
    int64_t selectedFrame() const;
    int64_t nearest(juce::Point<float>) const;
    juce::Point<float> pixel(double x, double y) const;
    juce::Point<double> position(juce::Point<float>) const;
};
} // namespace sp
