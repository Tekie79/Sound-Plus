#pragma once
#include "MidiModel.h"
#include "TrackModel.h"
namespace sp::qa
{
struct ContinuationFixture
{
    DesignDocument document;
    juce::String owner, other, clip;
};
inline ContinuationFixture continuationFixture(double rate)
{
    ContinuationFixture f;
    auto &d = f.document;
    d.sampleRate = rate;
    d.original.layout = channelLayout("mono-v1");
    d.original.masterDb = 0;
    f.owner = addTrack(d.original, TrackKind::midiInstrument);
    f.other = addTrack(d.original, TrackKind::midiInstrument);
    f.clip = createMidiClip(d, f.owner, 0);
    auto tick = [&](int64_t frame) { return midiTick(frame, d.original.midiTiming, rate); };
    auto *c = findMidiClip(d.original, f.clip);
    c->length = tick(1024);
    MidiNote n;
    n.tick = tick(16);
    n.duration = tick(784);
    n.velocity = 100;
    c->notes = {n};
    for (auto [frame, type, number, value] :
         {std::tuple{32, "cc", 64, 127}, {64, "bend", 0, 10000}, {200, "cc", 1, 83}, {900, "cc", 64, 0}})
    {
        MidiControl e;
        e.tick = tick(frame);
        e.type = type;
        e.number = number;
        e.value = value;
        c->controls.push_back(e);
    }
    // An overlapping independent clip and an adjacent clip must retain independent articulation.
    for (int first : {128, 1024})
    {
        auto id = createMidiClip(d, f.owner, first);
        auto *neighbor = findMidiClip(d.original, id);
        neighbor->length = tick(128);
        MidiNote other;
        other.pitch = 67;
        other.velocity = 64;
        other.channel = 2;
        other.duration = tick(64);
        neighbor->notes = {other};
    }
    return f;
}
inline void interleave(ContinuationFixture &f)
{
    for (int start : {64, 320, 700})
    {
        auto id = createMidiClip(f.document, f.other, start);
        auto *c = findMidiClip(f.document.original, id);
        c->length = midiTick(100, f.document.original.midiTiming, f.document.sampleRate);
        c->notes = {MidiNote()};
        c->notes[0].duration = c->length;
    }
}
inline bool sameMidi(const std::vector<MidiPacket> &a, const std::vector<MidiPacket> &b)
{
    if (a.size() != b.size())
        return false;
    for (size_t n = 0; n < a.size(); ++n)
        if (a[n].frame != b[n].frame || a[n].voice != b[n].voice || a[n].scope != b[n].scope ||
            a[n].status != b[n].status || a[n].data1 != b[n].data1 || a[n].data2 != b[n].data2 ||
            a[n].flags != b[n].flags)
            return false;
    return true;
}
} // namespace sp::qa
