#pragma once
#include "DesignModel.h"
#include "HostedRack.h"
#include <thread>
#include <mutex>
#include <condition_variable>
namespace sp
{
// Owns isolated slot services independently of view selection and transport graphs.
// All process launch, state I/O and supervision run on its control worker.
class LiveInsertSession
{
  public:
    struct Capture
    {
        juce::String document, variation, owner, slot, generation, expected, settings, gesture;
        uint64_t incarnation = 0;
        bool native = false;
        juce::var evidence;
    };
    using Receiver = std::function<void(Capture)>;
    LiveInsertSession(juce::File, Receiver, std::function<void(juce::String)>);
    ~LiveInsertSession();
    juce::File rootFor(const DesignDocument &, uint64_t) const;
    void sync(const DesignDocument &, uint64_t);
    void editor(const juce::String &, const juce::File &, const juce::String &, bool = true);
    void flush(std::function<void(bool)>);
    void suspendEditors(bool value)
    {
        editorsSuspended.store(value);
        wake.notify_one();
    }
    bool accepts(const Capture &);
    void trace(const juce::var &);
    juce::String problem(const juce::String &slot);

  private:
    struct Snapshot
    {
        DesignDocument document;
        uint64_t incarnation;
    };
    struct Entry;
    juce::File base;
    Receiver receive;
    std::function<void(juce::String)> failure;
    std::atomic<bool> quitting{false}, editorsSuspended{false};
    std::mutex mutex;
    std::condition_variable wake;
    std::optional<Snapshot> latest;
    std::map<juce::String, juce::String> generations, problems;
    struct EditorRequest
    {
        juce::File root;
        juce::String owner, generation;
        bool show = true;
    };
    std::map<juce::String, EditorRequest> open;
    std::vector<std::function<void(bool)>> flushes;
    std::vector<juce::var> traces;
    std::thread worker;
    void run();
};
} // namespace sp
