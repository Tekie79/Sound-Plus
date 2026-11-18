#pragma once
#include "DesignAudio.h"
#include "AudioIPC.h"
#include <condition_variable>
#include <thread>
namespace sp
{
class DesignSession final : public juce::Component,private juce::Timer
{
  public:
    DesignSession(juce::File catalog,AudioClient &);
    ~DesignSession() override;
    DesignDocument document;
    juce::File savedFile,outputFolder,lastOutput;
    juce::String message;
    std::function<void()> onChange;
    std::function<void()> onBeforePlay;
    bool dirty=false,working=false;
    std::atomic<bool> rendering{false};
    std::atomic<double> renderProgress{0};
    void edit(const juce::String &,const std::function<void(DesignDocument &)> &);
    void beginGesture();void endGesture();void undo();void redo();
    void newDesign();void open(const juce::File &);void save(const juce::File &);
    void add(const Asset &,int64_t first,int64_t last,bool external=false,int pair=0);
    void addExternal(const juce::File &);void replaceSource(const juce::String &layer,const juce::File &);
    void play();void seek(int64_t);void stop();bool ownsPlayback();
    void render(bool selection=false);void copyOriginal(const juce::File &folder);void cancelRender();
    void selectLayer(const juce::String &);void selectVariation(const juce::String &);
    void generate(uint64_t seed);void recoverAutosave();
    bool canUndo() const{return !undoHistory.empty();}bool canRedo() const{return !redoHistory.empty();}
    DesignLayer *selectedLayer();
  private:
    struct History{DesignDocument document;juce::String label;};
    std::vector<History> undoHistory,redoHistory;
    std::optional<DesignDocument> gesture;
    juce::File catalog,directory;
    AudioClient &audio;
    std::mutex mutex;std::condition_variable wake;
    std::deque<std::function<void()>> jobs;
    struct Preview{DesignDocument document;int64_t position;uint64_t generation;};
    std::optional<Preview> preview;
    std::thread worker,renderWorker;
    std::atomic<bool> quitting{false},renderCancelled{false};
    std::atomic<uint64_t> previewGeneration{0};
    bool previewWaiting=false;
    double lastEdit=0,previewDue=0;
    int64_t autoRevision=0;
    void notify();void enqueue(std::function<void()>);void run();void timerCallback() override;
    void schedulePreview();void changed();
    void startOutput(DesignDocument,bool copy,const juce::File &folder,juce::String kind="design-render");
};
}
