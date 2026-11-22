#pragma once
#include <juce_core/juce_core.h>
#include "ChannelLayout.h"
#include <vector>
#include <set>
namespace sp
{
// High-resolution musical ticks: <=0.12 project samples/tick at the supported extremes.
inline constexpr int64_t midiPpq = 9600000;
struct MidiTiming
{
    double bpm = 120;
    int numerator = 4, denominator = 4;
};
struct MidiNote
{
    juce::String id = juce::Uuid().toString(), tie;
    int64_t tick = 0, duration = midiPpq;
    int pitch = 60, velocity = 100, releaseVelocity = 64, channel = 1;
};
struct MidiControl
{
    juce::String id = juce::Uuid().toString(), type = "cc";
    int64_t tick = 0;
    int channel = 1, number = 1, value = 0;
};
struct MidiClip
{
    juce::String id = juce::Uuid().toString(), track, name = "MIDI", origin;
    int64_t start = 0, length = 4 * midiPpq, offset = 0, loopLength = 0;
    bool mute = false;
    std::vector<MidiNote> notes;
    std::vector<MidiControl> controls;
    juce::var recordingOrigin;
};
struct MidiPacket
{
    int64_t frame = 0;
    uint64_t voice = 0, scope = 0;
    uint8_t status = 0, data1 = 0, data2 = 0, flags = 0;
};
struct InstrumentSlot;
juce::var instrumentJson(const InstrumentSlot &);
InstrumentSlot instrumentFromJson(const juce::var &);
void validateInstrument(const InstrumentSlot &, const ChannelLayout &);
struct DesignMix;
struct DesignDocument;
int64_t midiFrame(int64_t ticks, const MidiTiming &, double rate);
int64_t midiTick(int64_t frame, const MidiTiming &, double rate);
MidiClip *findMidiClip(DesignMix &, const juce::String &);
const MidiClip *findMidiClip(const DesignMix &, const juce::String &);
struct Asset;
std::vector<const Asset *> mixAssets(const DesignMix &);
std::vector<Asset *> mixAssets(DesignMix &);
bool hasMidi(const DesignMix &);
bool hasMidi(const DesignDocument &);
void validateMidi(const DesignMix &, double rate);
juce::var midiMixJson(const DesignMix &);
void midiMixFromJson(DesignMix &, const juce::var &);
juce::var midiClipJson(const MidiClip &);
MidiClip midiClipFromJson(const juce::var &);
std::vector<MidiPacket> compileMidi(const DesignMix &, const juce::String &track, double rate);
juce::String createMidiClip(DesignDocument &, const juce::String &track, int64_t startFrame);
void moveMidiClip(DesignDocument &, const juce::String &, const juce::String &track, int64_t frame,
                  bool copy);
void trimMidiClip(DesignDocument &, const juce::String &, int64_t firstFrame, int64_t lastFrame);
void splitMidiClip(DesignDocument &, const juce::String &, int64_t frame);
void quantizeMidiNotes(MidiClip &, const std::set<juce::String> &, int64_t grid, double strength);
void transposeMidiNotes(MidiClip &, const std::set<juce::String> &, int semitones);
void velocityMidiNotes(MidiClip &, const std::set<juce::String> &, int delta);
uint64_t midiIdentity(const juce::String &);
} // namespace sp
