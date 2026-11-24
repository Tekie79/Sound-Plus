#pragma once
#include "AutomationEditor.h"
#include "Waveform.h"
namespace sp
{
class MultitrackEditor final : public juce::Component,
                               public juce::FileDragAndDropTarget,
                               public juce::DragAndDropTarget
{
  public:
    using Peaks = std::function<std::shared_ptr<const WaveformData>(const Asset &)>;
    MultitrackEditor(DesignSession &, Peaks, juce::PropertiesFile &);
    ~MultitrackEditor() override;
    TimelineViewport viewport;
    SnapSettings snapping;
    bool audibleScrub = false;
    std::function<void()> onRowsChanged;
    std::function<void(const juce::String &)> onOpenMidi, onInstrument;
    std::function<bool(const juce::KeyPress &)> onCommand;
    void openSurroundPanner(const juce::String &);
    void closeSurroundPanner();
    std::function<void(juce::var, int64_t)> onAssetDrop;
    void refresh();
    void transportTick();
    int contentHeight() const;
    void paint(juce::Graphics &) override;
    void resized() override;
    void fitDesign();
    void fitSelection();
    void zoom(double);
    void zoomVertical(double);
    void addTrackMenu(juce::Component &);
    void createTrack(TrackKind);
    void selectTrack(const juce::String &);
    void renameTrack(const juce::String &, const juce::String &);
    void moveClip(const juce::String &, const juce::String &, int64_t, bool copy);
    void splitSelected();
    void midiInputMenu(const juce::String &, juce::Component &);
    void inputMenu(const juce::String &, juce::Component &);
    void openTrackOptions(const juce::String &id, juce::Component &anchor)
    {
        trackMenu(id, juce::PopupMenu::Options().withTargetComponent(anchor));
    }
    void mouseDown(const juce::MouseEvent &) override;
    void mouseDrag(const juce::MouseEvent &) override;
    void mouseUp(const juce::MouseEvent &) override;
    void mouseMove(const juce::MouseEvent &) override;
    void mouseDoubleClick(const juce::MouseEvent &) override;
    void mouseWheelMove(const juce::MouseEvent &, const juce::MouseWheelDetails &) override;
    bool keyPressed(const juce::KeyPress &) override;
    void focusLost(FocusChangeType) override;
    bool isInterestedInFileDrag(const juce::StringArray &) override;
    void filesDropped(const juce::StringArray &, int, int) override;
    bool isInterestedInDragSource(const SourceDetails &) override;
    void itemDropped(const SourceDetails &) override;

  private:
    struct Header;
    std::unique_ptr<juce::DocumentWindow> surroundWindow;
    juce::OwnedArray<Header> headers;
    juce::OwnedArray<AutomationEditor> lanes;
    DesignSession &session;
    Peaks peaks;
    juce::PropertiesFile &preferences;
    std::vector<juce::String> rows;
    juce::String signature, clipId, ownerAtDown, draggingMarker;
    uint64_t editorIncarnation = 0;
    bool refreshing = false;
    CanvasTarget hit = CanvasTarget::none;
    int fadePoint = -1;
    juce::Rectangle<float> midiGeometry(const MidiClip &) const;
    bool midiDrag = false;
    bool active = false, copied = false, slip = false;
    DesignDocument original;
    juce::Point<float> anchor;
    double height = 86, capturedHeight = 86;
    std::vector<float> tops;
    juce::String rowAt(float) const;
    CanvasClipGeometry geometry(const DesignLayer &, const juce::String &) const;
    void showAddTrackMenu(const juce::PopupMenu::Options &);
    void trackMenu(const juce::String &, const juce::PopupMenu::Options &);
    void fadeMenu(const juce::String &, bool);
    void updateRows();
    void dropOwner(float);
};
} // namespace sp
