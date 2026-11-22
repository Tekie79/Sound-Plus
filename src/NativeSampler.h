#pragma once
#include "DesignModel.h"
#include "TrackModel.h"
namespace sp
{
// Prepared on the existing graph worker. No decoding/allocation in render().
class NativeSampler
{
  public:
    NativeSampler(const InstrumentSlot &, const ChannelLayout &, double rate);
    void render(juce::AudioBuffer<float> &, int frames, int64_t first, const std::vector<MidiPacket> &,
                size_t &next, const CompiledAutomation * = nullptr, int64_t inputEnd = -1);
    void event(const MidiPacket &);
    void allNotesOff(bool immediate = false);
    void update(const InstrumentSlot &);
    void verify() const;
    size_t sourceFloats() const;
    int activeVoices() const;
    uint64_t stolenVoices() const
    {
        return stolen;
    }

  private:
    struct Sample
    {
        SourcePoolEntry reference;
        juce::AudioBuffer<float> pcm;
        ChannelMatrix matrix;
    };
    struct Voice
    {
        bool active = false, keyDown = false, pedal = false;
        int sample = 0, note = 60, channel = 0, stage = 0;
        uint64_t token = 0, age = 0;
        double position = 0, level = 0, step = 0;
        int64_t remaining = 0;
        double velocity = 1;
    };
    std::vector<Sample> samples;
    std::array<Voice, 32> voices{};
    std::array<bool, 16> sustain{};
    std::array<double, 16> bend{}, volume{}, expression{};
    std::array<int, 16> programs{};
    InstrumentSlot settings;
    ChannelLayout bus;
    double rate;
    uint64_t age = 0, ordinal = 0, stolen = 0;
    bool gated = false;
    void release(Voice &);
    void stage(Voice &, int);
    void advance(Voice &);
};
} // namespace sp
