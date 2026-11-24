#pragma once
#include "TrackModel.h"
#include <juce_audio_devices/juce_audio_devices.h>

namespace sp
{
// A dedicated control surface input, separate from the note recorder. Each
// input has one SPSC queue; only the session thread touches device handles.
class AutomationMidiInput final : private juce::MidiInputCallback
{
  public:
    struct CC
    {
        int channel = 0, number = 0, value = 0;
    };
    bool open(const juce::String &identifier);
    ~AutomationMidiInput() override;
    void drain(const std::function<void(CC)> &);
    bool overflowed()
    {
        return overflow.exchange(false);
    }

  private:
    static constexpr unsigned capacity = 256;
    std::array<CC, capacity> queue{};
    std::atomic<unsigned> write{0}, read{0};
    std::atomic<bool> overflow{false};
    std::unique_ptr<juce::MidiInput> input;
    void handleIncomingMidiMessage(juce::MidiInput *, const juce::MidiMessage &) override;
};
struct ControllerPickup
{
    int previous = -1, last = -1;
    bool acquired = false;
    std::optional<double> move(const AutomationMidiBinding &, const AutomationParameter &, int value,
                               double underlying);
};
} // namespace sp
