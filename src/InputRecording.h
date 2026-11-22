#pragma once
#include "DesignModel.h"
#include <array>
#include <atomic>
#include <mutex>
#include <thread>
#include <condition_variable>

namespace sp
{
static_assert(std::atomic<uint64_t>::is_always_lock_free && std::atomic<int64_t>::is_always_lock_free &&
              std::atomic<double>::is_always_lock_free && std::atomic<float>::is_always_lock_free);
struct InputDeviceInfo
{
    juce::String uid, name;
    double rate = 0;
    int channels = 0, latency = 0, safety = 0;
    juce::StringArray names;
    ChannelLayout reportedLayout;
    juce::var json() const;
};
struct InputTrackPlan
{
    juce::String owner, name;
    std::vector<int> channels;
    ChannelLayout layout;
};
struct InputRecordingPlan
{
    juce::String id, document, variation, deviceUID;
    juce::File root;
    double projectRate = 48000;
    int64_t start = 0;
    uint64_t generation = 0;
    std::vector<InputTrackPlan> tracks;
};
InputRecordingPlan inputPlanFromJson(const juce::var &);
juce::var inputPlanJson(const InputRecordingPlan &);
void validateInputPlan(const InputRecordingPlan &, const InputDeviceInfo &);

// A single output-callback writer; readers never wait on the callback.
struct InputTimelineClock
{
    struct Value
    {
        uint64_t hostNs = 0, generation = 0;
        int64_t frame = 0;
        double rate = 0;
    };
    void publish(Value) noexcept;
    Value read() const noexcept;

  private:
    std::atomic<uint64_t> sequence{0}, host{0}, generation{0};
    std::atomic<int64_t> frame{0};
    std::atomic<double> rate{0};
};

// Recoverable source PCM; finalized WAVs and receipt publish together by directory rename.
// Partial/cancelled data is retained. Recovery is a deliberate new, labelled publication.
class InputRecordingStore
{
  public:
    InputRecordingStore(InputRecordingPlan, InputDeviceInfo);
    ~InputRecordingStore();
    void append(const float *interleaved, int frames, int physicalChannels, int64_t deviceFrame,
                uint64_t hostNs, int64_t projectFrame);
    void checkpoint();
    juce::var finish(bool complete, const juce::String &reason, bool cancelled = false);
    static juce::var recover(const juce::File &partial);
    static juce::Array<juce::var> recoverable(const juce::File &root);
    int64_t frames() const
    {
        return written;
    }
    // Controlled writer failure only; never read by an audio callback.
    int64_t writeBudget = -1;
    void setDiagnostics(juce::var value)
    {
        diagnostics = std::move(value);
    }

  private:
    InputRecordingPlan plan;
    InputDeviceInfo device;
    juce::File partial;
    std::vector<int> handles;
    std::vector<float> selected;
    std::vector<float> peaks;
    int64_t written = 0, firstDeviceFrame = 0, placement = 0;
    uint64_t firstHostNs = 0;
    bool closed = false;
    juce::var diagnostics;
    juce::var journal(const juce::String &, const juce::String &) const;
    static juce::var publish(const juce::File &, juce::var, bool recovered);
};

// One actual input callback -> one writer. No allocation/lock/I/O in push or monitor.
class InputRecordingService
{
  public:
    static constexpr int maxChannels = 16, maxFrames = 4096, slots = 32;
    enum Fault
    {
        none,
        overflow,
        discontinuity,
        invalidTimestamp,
        invalidPcm,
        oversizedBlock,
        deviceLost
    };
    explicit InputRecordingService(InputTimelineClock &,
                                   const std::atomic<int64_t> *outputUnderruns = nullptr);
    ~InputRecordingService();
    void configure(const InputDeviceInfo &); // input callback must be stopped
    void start(InputRecordingPlan);
    void finish(const juce::String &operation, bool cancel = false);
    void interrupt(const juce::String &);
    void fault(Fault) noexcept;
    bool busy() const noexcept
    {
        return owned.load();
    }
    void push(const float *const *, int channels, int frames, int64_t deviceFrame,
              uint64_t acquisitionHostNs) noexcept;
    juce::var status();
    bool setMonitoring(const std::vector<InputTrackPlan> &, double outputRate);
    void monitor(float *const *out, int channels, int frames, float gain) noexcept;
    std::array<std::atomic<float>, maxChannels> peaks{};
    void observeNativeCallback(uint64_t ns) noexcept
    {
        if (ns > nativeCallbackNs.load(std::memory_order_relaxed))
            nativeCallbackNs.store(ns, std::memory_order_relaxed);
    }

  private:
    friend struct InputRecordingTestAccess;
    struct Block
    {
        std::array<float, maxChannels * maxFrames> pcm{};
        int frames = 0, channels = 0;
        int64_t deviceFrame = 0;
        uint64_t hostNs = 0;
    };
    InputTimelineClock &clock;
    const std::atomic<int64_t> *const outputUnderrunCounter;
    std::unique_ptr<std::array<Block, slots>> ring;
    std::atomic<uint64_t> write{0}, read{0};
    std::atomic<int> callbackUsers{0}, failure{none};
    std::atomic<bool> accepting{false}, owned{false}, quitting{false};
    std::mutex mutex;
    std::condition_variable wake;
    InputDeviceInfo device;
    std::optional<InputRecordingPlan> pending;
    juce::String finishId, interrupted;
    bool cancel = false;
    juce::var observation;
    std::thread worker;
    std::atomic<int64_t> budget{-1};
    std::atomic<bool> holdWriter{false};
    std::atomic<uint64_t> callbackCount{0}, maxCallbackNs{0}, overruns{0};
    std::atomic<uint64_t> nativeCallbackNs{0};
    static constexpr uint64_t monitorCapacity = 65536;
    std::array<std::array<float, monitorCapacity>, 2> monitored{};
    std::atomic<uint64_t> monitorWrite{0}, monitorRead{0}, monitorDrops{0};
    std::array<std::array<std::atomic<float>, maxChannels>, 2> monitorGains{};
    std::atomic<bool> monitoring{false};
    void run();
};
} // namespace sp
