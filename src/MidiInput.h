#pragma once
#include "InputRecording.h"
#include "MidiModel.h"
#include <CoreMIDI/CoreMIDI.h>
#include <mach/mach_time.h>
#include <mutex>
namespace sp
{
class PcmStream;
struct CapturedMidi
{
    MidiPacket event;
    int64_t acquired = 0;
    uint64_t hostNs = 0;
};
// CoreMIDI callback is a bounded SPSC producer only. The existing graph producer
// drains, schedules and checkpoints. Neither device callback performs MIDI I/O.
class MidiInputService
{
  public:
    explicit MidiInputService(InputTimelineClock &);
    ~MidiInputService();
    static juce::Array<juce::var> devices();
    void configure(const juce::var &);
    void begin(const juce::var &);
    void finish(bool cancel = false, juce::String reason = {});
    void poll();
    void service(PcmStream *);
    void interrupt(const juce::String &);
    juce::var status();
    bool busy() const
    {
        return capturing.load();
    }
    void receiveForTest(uint64_t hostNs, uint8_t status, uint8_t a, uint8_t b) noexcept;
    static std::vector<MidiClip> clips(const std::vector<CapturedMidi> &, const juce::StringArray &owners,
                                       int64_t first, int64_t last, const MidiTiming &, double rate,
                                       const juce::var &origin);
    static juce::var recover(const juce::File &);

  private:
    InputTimelineClock &clock;
    MIDIClientRef client = 0;
    MIDIPortRef port = 0;
    MIDIEndpointRef endpoint = 0;
    mach_timebase_info_data_t timebase{};
    std::atomic<bool> capturing{false};
    std::atomic<bool> accepting{false}, changed{false}, overflow{false};
    std::atomic<uint64_t> epoch{1};
    struct Raw
    {
        uint64_t epoch = 0, host = 0, generation = 0;
        int64_t acquired = 0;
        uint8_t status = 0, a = 0, b = 0;
    };
    static constexpr size_t capacity = 8192;
    std::array<Raw, capacity> ring{};
    std::atomic<uint64_t> write{0}, read{0};
    uint8_t running = 0, data[2]{};
    int received = 0;
    std::mutex mutex;
    juce::String uid, error, id, document, variation;
    juce::StringArray owners, monitored, recordOwners;
    int channel = 0, eventLimit = 8192;
    uint64_t generation = 0;
    bool recording = false, finishing = false, cancelled = false, panic = false;
    int64_t first = 0, last = 0;
    double rate = 48000, nextCheckpoint = 0;
    MidiTiming timing;
    std::vector<CapturedMidi> events;
    juce::File journal;
    juce::var receipt;
    uint64_t late = 0, maxLate = 0, receivedCount = 0;
    static void input(const MIDIPacketList *, void *, void *);
    static void notify(const MIDINotification *, void *);
    void receive(uint64_t, uint8_t, uint8_t, uint8_t) noexcept;
    juce::var payload() const;
    void checkpoint(bool complete);
};
} // namespace sp
