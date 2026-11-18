#pragma once
#include "DesignSession.h"
#include "Waveform.h"
#include "UIComponents.h"
#include "DesignControls.h"
#include "PerformView.h"
#include <map>
namespace sp
{
class DesignerView final : public juce::Component,
                           private juce::Timer,
                           private juce::KeyListener,
                           public juce::FileDragAndDropTarget,
                           public juce::DragAndDropTarget
{
  public:
    DesignerView(const juce::File &catalog, AudioClient &, juce::PropertiesFile &);
    ~DesignerView() override;
    DesignSession session;
    std::function<void()> onLibrary;
    std::function<void()> onToggleBrowser;
    std::function<void()> onSearch;
    void addSource(const Asset &, int64_t first, int64_t last, bool external = false, int pair = 0);
    void paint(juce::Graphics &) override;
    void resized() override;
    bool isInterestedInFileDrag(const juce::StringArray &) override;
    void filesDropped(const juce::StringArray &, int, int) override;
    bool isInterestedInDragSource(const SourceDetails &) override;
    void itemDropped(const SourceDetails &) override;
    void itemDragMove(const SourceDetails &) override;
    void itemDragExit(const SourceDetails &) override;
    void fileDragMove(const juce::StringArray &, int, int) override;
    void fileDragExit(const juce::StringArray &) override;
    juce::var interactionMetrics() const;
    void benchmarkGesture(int phase, double displacement);
    void benchmarkEndGesture();
    void benchmarkPerformance(int phase, double value)
    {
        if (perform)
            perform->benchmarkControls(phase, value);
    }
    void preparePerformanceBenchmark();
    bool benchmarkWorkspaceSwitch();
    void benchmarkStockControls(int phase, double value);
    juce::var benchmarkSelectionCommands();
    void commitNames();
    void setBrowserVisible(bool visible)
    {
        browse.setToggleState(visible, juce::dontSendNotification);
        browse.setTooltip(visible ? "Hide sound browser" : "Show sound browser");
    }
    bool commandKey(const juce::KeyPress &key)
    {
        return keyPressed(key, nullptr);
    }

  private:
    friend struct DesignerViewTestAccess;
    void showSnapOptions();
    IconButton zoomIn{"Zoom in (Command-wheel)", ActionIcon::zoom}, zoomOut{"Zoom out", ActionIcon::zoomOut},
        fitAll{"Fit design", ActionIcon::fit}, fitSelected{"Fit selected layers", ActionIcon::fitSelection},
        snap{"Enable snapping (Command temporarily bypasses)", ActionIcon::snap},
        snapOptions{"Snap targets and grid", ActionIcon::chevron},
        scrub{"Audible forward-grain preview", ActionIcon::scrub},
        verticalIn{"Increase track height (Command-Option-wheel)", ActionIcon::verticalIn},
        verticalOut{"Decrease track height", ActionIcon::verticalOut};
    int benchmarkPhase = -1;
    int benchmarkStockPhase = -1;
    juce::Point<float> benchmarkAnchor;
    juce::Array<juce::var> heardEdits;
    uint64_t lastHeardEdit = 0;
    struct Canvas;
    std::unique_ptr<Canvas> canvas;
    std::unique_ptr<PerformView> perform;
    ui::TextActionButton editWorkspace{"Edit", true}, performWorkspace{"Perform", true},
        snapshotButton{"Save Snapshot"};
    bool performMode = false;
    void setPerformMode(bool);
    ui::FileDragButton dragOutput{"Drag WAV"}, dragOriginal{"Drag Original"};
    AudioClient &audio;
    ui::LookAndFeel look;
    void mouseDown(const juce::MouseEvent &e) override
    {
        if (e.originalComponent == &meter)
        {
            clipHeld = false;
            repaint();
        }
    }
    juce::PropertiesFile &preferences;
    struct InspectorGroup
    {
        juce::String name;
        int start = 0, height = 0;
        bool open = true;
        std::unique_ptr<IconButton> header;
        std::vector<std::pair<juce::Component *, juce::Rectangle<int>>> children;
    };
    std::vector<InspectorGroup> groups;
    void finishGroups();
    void layoutInspector();
    bool inspectorOpen = true;
    ui::TextActionButton browse{"Browser", true}, library{"Library"};
    IconButton inspect{"Show layer inspector", ActionIcon::inspector};
    ui::FadeShapeButton fadeInShape{"Fade-in shape", true}, fadeOutShape{"Fade-out shape", false};
    juce::Viewport canvasViewport, inspectorViewport;
    juce::Component inspector;
    juce::OwnedArray<juce::Component> controls;
    std::vector<std::function<void()>> refreshers;
    int inspectorY = 0;
    bool updating = false;
    DesignMix effectiveDisplay;
    IconButton add{"Add source", ActionIcon::addLayer}, newButton{"New design", ActionIcon::newDocument},
        openButton{"Open design", ActionIcon::open}, saveButton{"Save design", ActionIcon::save};
    IconButton undoButton{"Undo", ActionIcon::undo}, redoButton{"Redo", ActionIcon::redo},
        duplicate{"Duplicate layer", ActionIcon::duplicate}, remove{"Remove layer", ActionIcon::trash},
        up{"Move layer up", ActionIcon::up}, down{"Move layer down", ActionIcon::down};
    IconButton play{"Play design", ActionIcon::play},
        accurate{"Accurate audition from playhead", ActionIcon::accurate},
        generate{"Generate variations", ActionIcon::variations},
        variationSettings{"Variation bounds and enabled parameters", ActionIcon::settings};
    IconButton keep{"Keep variation", ActionIcon::keep}, rename{"Rename variation", ActionIcon::rename},
        discard{"Discard variation", ActionIcon::trash},
        compare{"Compare with original", ActionIcon::compare};
    IconButton render{"Render Design", ActionIcon::render},
        renderSelection{"Render selected layer", ActionIcon::renderSelection},
        renderSettings{"Render settings", ActionIcon::settings}, cancel{"Cancel render", ActionIcon::cancel},
        addPrint{"Add completed output as layer", ActionIcon::addLayer},
        copyOriginal{"Copy Original to Project", ActionIcon::copyOriginal};
    IconButton recover{"Recover autosave", ActionIcon::history},
        chooseFolder{"Choose permanent output folder", ActionIcon::folder};
    juce::Label title, info, status, position, outputInfo, meter;
    juce::ComboBox candidates, rackTarget, effectChoice, channelMap, deviceRate;
    IconButton addEffect{"Add stock insert", ActionIcon::processing},
        removeEffect{"Remove insert", ActionIcon::trash}, effectUp{"Move insert up", ActionIcon::up},
        effectDown{"Move insert down", ActionIcon::down};
    juce::ToggleButton effectBypass{"Bypass insert"}, effectLock{"Lock insert for variations"};
    std::array<juce::Slider, 4> effectValues;
    std::array<juce::Label, 4> effectLabels;
    std::array<IconButton, 4> effectParameterLocks{{{"Lock insert parameter 1", ActionIcon::lock},
                                                    {"Lock insert parameter 2", ActionIcon::lock},
                                                    {"Lock insert parameter 3", ActionIcon::lock},
                                                    {"Lock insert parameter 4", ActionIcon::lock}}};
    juce::Slider monitor;
    juce::Label sourceInfo, layerName, pitchInfo;
    juce::String candidateSignature, effectSignature, lastComparison, comparisonDocument;
    bool clipHeld = false;
    float peak = 0;
    WaveformCache peaks;
    juce::String requestedPeakKey;
    uint64_t peakRequest = 0;
    std::map<juce::String, std::shared_ptr<const WaveformData>> waveforms;
    std::unique_ptr<juce::FileChooser> chooser;
    void timerCallback() override;
    bool keyPressed(const juce::KeyPress &, juce::Component *) override;
    void refresh();
    void editLayer(const juce::String &, std::function<void(DesignLayer &)>);
    void number(const juce::String &, const juce::String &, std::function<double(const DesignLayer &)>,
                std::function<void(DesignLayer &, double)>, uint32_t lock = 0);
    void slider(const juce::String &, const juce::String &, double lo, double hi, double step,
                std::function<double(const DesignLayer &)>, std::function<void(DesignLayer &, double)>,
                uint32_t lock = 0);
    void toggle(const juce::String &, std::function<bool(const DesignLayer &)>,
                std::function<void(DesignLayer &, bool)>);
    void section(const juce::String &);
    void chooseFile(int kind);
    void saveDesign();
    void variationPopup();
    void renderPopup();
    void renameVariation();
    std::vector<StockEffect> *rack();
    void changeEffect(const juce::String &, std::function<void(std::vector<StockEffect> &)>);
    void updateEffects();
    juce::String waveformKey(const Asset &) const;
};
} // namespace sp
