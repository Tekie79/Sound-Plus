#include "MidiModel.h"
#include "DesignModel.h"
#include "TrackModel.h"
#include "SourcePools.h"
#include <algorithm>
#include <cmath>
namespace sp
{
namespace
{
juce::var obj()
{
    return juce::var(new juce::DynamicObject);
}
void put(juce::var &v, const char *key, const juce::var &x)
{
    v.getDynamicObject()->setProperty(key, x);
}
void require(bool b, const char *s)
{
    if (!b)
        throw std::runtime_error(s);
}
int64_t integer(const juce::var &v, const char *key)
{
    return (int64_t)v[key];
}
MidiClip &needed(DesignDocument &d, const juce::String &id)
{
    auto *c = findMidiClip(d.activeMix(), id);
    require(c != nullptr, "MIDI clip unavailable");
    return *c;
}
void rekey(MidiClip &c, bool independent)
{
    c.id = juce::Uuid().toString();
    if (independent)
        c.origin = c.id;
    for (auto &n : c.notes)
    {
        if (n.tie.isEmpty())
            n.tie = n.id;
        n.id = juce::Uuid().toString();
        if (independent)
            n.tie = n.id;
    }
    for (auto &e : c.controls)
        e.id = juce::Uuid().toString();
}
bool selected(const std::set<juce::String> &ids, const juce::String &id)
{
    return ids.empty() || ids.contains(id);
}
} // namespace
juce::var instrumentJson(const InstrumentSlot &s)
{
    auto v = obj();
    put(v, "id", s.id);
    put(v, "format", s.format);
    put(v, "identifier", s.identifier);
    put(v, "state", s.state);
    if (s.version)
    {
        put(v, "midiInstrumentVersion", s.version);
        put(v, "rootNote", s.rootNote);
        put(v, "polyphony", s.polyphony);
        put(v, "attack", s.attack);
        put(v, "decay", s.decay);
        put(v, "sustain", s.sustain);
        put(v, "release", s.release);
        put(v, "bendRange", s.bendRange);
        put(v, "gainDb", s.instrumentGainDb);
        put(v, "transpose", s.transpose);
        put(v, "loop", s.sampleLoop);
        put(v, "oneShot", s.oneShot);
        put(v, "roundRobin", s.roundRobin);
        juce::Array<juce::var> samples;
        for (auto &x : s.samples)
            samples.add(poolEntryToJson(x));
        put(v, "samples", samples);
        if (!s.hostedInstrument.hosted.slots.empty())
            put(v, "hostedInstrument", insertChainToJson(s.hostedInstrument));
    }
    return v;
}
InstrumentSlot instrumentFromJson(const juce::var &v)
{
    require(v.isObject(), "Invalid Instrument slot");
    InstrumentSlot s;
    s.id = v["id"].toString();
    s.format = v["format"].toString();
    s.identifier = v["identifier"].toString();
    s.state = v["state"].toString();
    if (v.hasProperty("midiInstrumentVersion"))
    {
        s.version = (int)v["midiInstrumentVersion"];
        require(s.version == 1 && v["samples"].isArray() && v["samples"].size() <= 8,
                "Unsupported Instrument schema");
        s.rootNote = (int)v["rootNote"];
        s.polyphony = (int)v["polyphony"];
        s.attack = (double)v["attack"];
        s.decay = (double)v["decay"];
        s.sustain = (double)v["sustain"];
        s.release = (double)v["release"];
        s.bendRange = (double)v["bendRange"];
        s.instrumentGainDb = (double)v["gainDb"];
        s.transpose = (double)v["transpose"];
        s.sampleLoop = (bool)v["loop"];
        s.oneShot = (bool)v["oneShot"];
        s.roundRobin = (bool)v["roundRobin"];
        for (auto &x : *v["samples"].getArray())
            s.samples.push_back(poolEntryFromJson(x));
        if (v.hasProperty("hostedInstrument"))
            s.hostedInstrument = insertChainFromJson(v["hostedInstrument"]);
    }
    return s;
}
void validateInstrument(const InstrumentSlot &s, const ChannelLayout &bus)
{
    require(s.version == 0 || s.version == 1, "Unsupported Instrument schema");
    if (!s.version)
        return;
    require(s.id.isNotEmpty() && s.polyphony >= 1 && s.polyphony <= 32 && s.rootNote >= 0 &&
                s.rootNote <= 127 && std::isfinite(s.attack) && s.attack >= 0 && s.attack <= 10 &&
                std::isfinite(s.decay) && s.decay >= 0 && s.decay <= 10 && std::isfinite(s.sustain) &&
                s.sustain >= 0 && s.sustain <= 1 && std::isfinite(s.release) && s.release >= 0 &&
                s.release <= 30 && std::isfinite(s.bendRange) && s.bendRange >= 0 && s.bendRange <= 24 &&
                std::isfinite(s.instrumentGainDb) && s.instrumentGainDb >= -90 && s.instrumentGainDb <= 24 &&
                std::isfinite(s.transpose) && std::abs(s.transpose) <= 48 && !(s.oneShot && s.sampleLoop) &&
                s.samples.size() <= 8,
            "Invalid Instrument controls");
    if (s.format == "none")
    {
        require(s.samples.empty() && s.hostedInstrument.hosted.slots.empty(),
                "Empty Instrument contains processors");
        return;
    }
    if (s.format == "SoundPlus")
    {
        require(s.identifier == "sampler-v1" && s.hostedInstrument.hosted.slots.empty(),
                "Invalid native Instrument identity");
        for (auto &sample : s.samples)
        {
            (void)poolEntryFromJson(poolEntryToJson(sample));
            require(sample.reviewed && sample.reviewedBus == bus && !sample.routing.needsSelection,
                    "Review sampler source channels and declared output roles");
            validateRouting(sample.routing, sample.source.channels, bus);
        }
    }
    else
    {
        require((s.format == "VST3" || s.format == "AudioUnit") && s.samples.empty() &&
                    s.hostedInstrument.stock.empty() && s.hostedInstrument.hosted.slots.size() == 1 &&
                    s.hostedInstrument.hosted.instrument,
                "Instrument needs one isolated VST3/AUv2 slot");
        auto settings = juce::JSON::parse(s.hostedInstrument.hosted.slots[0].settings);
        require((bool)settings["plugin"]["instrument"] &&
                    settings["plugin"]["format"].toString() == s.format &&
                    settings["plugin"]["id"].toString() == s.identifier,
                "Instrument classification/identity mismatch");
        validateHostedRack(s.hostedInstrument.hosted);
    }
}
uint64_t midiIdentity(const juce::String &id)
{
    uint64_t h = 1469598103934665603ULL;
    for (auto c : id.toStdString())
    {
        h ^= (uint8_t)c;
        h *= 1099511628211ULL;
    }
    return h ? h : 1;
}
int64_t midiFrame(int64_t tick, const MidiTiming &t, double rate)
{
    require(std::isfinite(t.bpm) && t.bpm >= 20 && t.bpm <= 300 && std::isfinite(rate) && rate >= 8000 &&
                rate <= 384000,
            "Invalid MIDI project clock");
    return (int64_t)std::llround((long double)tick * rate * 60 / (t.bpm * midiPpq));
}
int64_t midiTick(int64_t frame, const MidiTiming &t, double rate)
{
    (void)midiFrame(0, t, rate);
    return (int64_t)std::llround((long double)frame * t.bpm * midiPpq / (rate * 60));
}
MidiClip *findMidiClip(DesignMix &m, const juce::String &id)
{
    for (auto &c : m.midiClips)
        if (c.id == id)
            return &c;
    return nullptr;
}
const MidiClip *findMidiClip(const DesignMix &m, const juce::String &id)
{
    for (auto &c : m.midiClips)
        if (c.id == id)
            return &c;
    return nullptr;
}
std::vector<const Asset *> mixAssets(const DesignMix &m)
{
    std::vector<const Asset *> out;
    for (auto &l : m.layers)
        out.push_back(&l.source);
    for (auto &t : m.tracks)
        if (t.instrument && t.instrument->version == 1)
            for (auto &s : t.instrument->samples)
                out.push_back(&s.source);
    return out;
}
std::vector<Asset *> mixAssets(DesignMix &m)
{
    std::vector<Asset *> out;
    for (auto &l : m.layers)
        out.push_back(&l.source);
    for (auto &t : m.tracks)
        if (t.instrument && t.instrument->version == 1)
            for (auto &s : t.instrument->samples)
                out.push_back(&s.source);
    return out;
}
bool hasMidi(const DesignMix &m)
{
    return m.midiEnabled || !m.midiClips.empty() ||
           std::any_of(m.tracks.begin(), m.tracks.end(),
                       [](const auto &t) {
                           return (t.instrument && t.instrument->version == 1) || t.midiInputUID.isNotEmpty();
                       });
}
bool hasMidi(const DesignDocument &d)
{
    if (hasMidi(d.original))
        return true;
    for (auto &v : d.variations)
        if (hasMidi(v.mix))
            return true;
    for (auto &t : d.takes)
        if (hasMidi(t.recipe))
            return true;
    return false;
}
void validateMidi(const DesignMix &m, double rate)
{
    if (!hasMidi(m))
        return;
    require(mixAssets(m).size() <= 64, "At most64 audio and sampler source references per design");
    require(m.trackSchema && m.midiClips.size() <= 64, "MIDI requires typed tracks; at most64 MIDI clips");
    (void)midiFrame(0, m.midiTiming, rate);
    require(m.midiTiming.numerator >= 1 && m.midiTiming.numerator <= 16 &&
                (m.midiTiming.denominator == 2 || m.midiTiming.denominator == 4 ||
                 m.midiTiming.denominator == 8 || m.midiTiming.denominator == 16),
            "Invalid time signature");
    std::set<juce::String> ids;
    size_t events = 0;
    auto maximum = midiTick((int64_t)(rate * 3600), m.midiTiming, rate);
    for (auto &c : m.midiClips)
    {
        auto *track = findTrack(m, c.track);
        require(track && track->kind == TrackKind::midiInstrument, "MIDI clip requires a MIDI Track");
        require(c.id.isNotEmpty() && ids.insert(c.id).second && c.start >= 0 && c.length > 0 &&
                    c.start <= maximum - c.length && c.offset >= 0 && c.offset <= maximum &&
                    c.loopLength >= 0 && c.loopLength <= maximum,
                "Invalid MIDI clip identity/timing");
        events += c.notes.size() + c.controls.size();
        require(events <= 8192, "MIDI design event bound exceeded");
        for (auto &n : c.notes)
            require(n.id.isNotEmpty() && ids.insert(n.id).second && n.tick >= 0 && n.duration > 0 &&
                        n.tick <= maximum - n.duration && n.pitch >= 0 && n.pitch < 128 && n.velocity > 0 &&
                        n.velocity < 128 && n.releaseVelocity >= 0 && n.releaseVelocity < 128 &&
                        n.channel >= 1 && n.channel <= 16,
                    "Invalid MIDI note");
        for (auto &e : c.controls)
            require(
                e.id.isNotEmpty() && ids.insert(e.id).second && e.tick >= 0 && e.tick <= maximum &&
                    e.channel >= 1 && e.channel <= 16 &&
                    ((e.type == "cc" && e.number >= 0 && e.number < 128 && e.value >= 0 && e.value < 128) ||
                     (e.type == "bend" && e.value >= 0 && e.value < 16384) ||
                     (e.type == "program" && e.value >= 0 && e.value < 128)),
                "Invalid MIDI controller");
    }
}
juce::var midiClipJson(const MidiClip &c)
{
    auto v = obj();
    put(v, "id", c.id);
    put(v, "track", c.track);
    put(v, "name", c.name);
    put(v, "origin", c.origin);
    put(v, "start", (juce::int64)c.start);
    put(v, "length", (juce::int64)c.length);
    put(v, "offset", (juce::int64)c.offset);
    put(v, "loopLength", (juce::int64)c.loopLength);
    put(v, "mute", c.mute);
    juce::Array<juce::var> notes, controls;
    for (auto &n : c.notes)
    {
        auto x = obj();
        put(x, "id", n.id);
        put(x, "tie", n.tie);
        put(x, "tick", (juce::int64)n.tick);
        put(x, "duration", (juce::int64)n.duration);
        put(x, "pitch", n.pitch);
        put(x, "velocity", n.velocity);
        put(x, "release", n.releaseVelocity);
        put(x, "channel", n.channel);
        notes.add(x);
    }
    for (auto &e : c.controls)
    {
        auto x = obj();
        put(x, "id", e.id);
        put(x, "type", e.type);
        put(x, "tick", (juce::int64)e.tick);
        put(x, "channel", e.channel);
        put(x, "number", e.number);
        put(x, "value", e.value);
        controls.add(x);
    }
    put(v, "notes", notes);
    put(v, "controls", controls);
    if (!c.recordingOrigin.isVoid())
        put(v, "recordingOrigin", c.recordingOrigin);
    return v;
}
MidiClip midiClipFromJson(const juce::var &v)
{
    require(v.isObject() && v["notes"].isArray() && v["controls"].isArray() &&
                v["notes"].size() + v["controls"].size() <= 8192,
            "Invalid MIDI clip data");
    MidiClip c;
    c.id = v["id"].toString();
    c.track = v["track"].toString();
    c.name = v["name"].toString();
    c.origin = v["origin"].toString();
    c.start = integer(v, "start");
    c.length = integer(v, "length");
    c.offset = integer(v, "offset");
    c.loopLength = integer(v, "loopLength");
    c.mute = (bool)v["mute"];
    c.recordingOrigin = v["recordingOrigin"];
    for (auto &x : *v["notes"].getArray())
    {
        MidiNote n;
        n.id = x["id"].toString();
        n.tie = x["tie"].toString();
        n.tick = integer(x, "tick");
        n.duration = integer(x, "duration");
        n.pitch = (int)x["pitch"];
        n.velocity = (int)x["velocity"];
        n.releaseVelocity = (int)x["release"];
        n.channel = (int)x["channel"];
        c.notes.push_back(n);
    }
    for (auto &x : *v["controls"].getArray())
    {
        MidiControl e;
        e.id = x["id"].toString();
        e.type = x["type"].toString();
        e.tick = integer(x, "tick");
        e.channel = (int)x["channel"];
        e.number = (int)x["number"];
        e.value = (int)x["value"];
        c.controls.push_back(e);
    }
    return c;
}
juce::var midiMixJson(const DesignMix &m)
{
    auto v = obj();
    put(v, "version", 1);
    put(v, "clock", "constant-tempo-microticks-v1");
    put(v, "ppq", (juce::int64)midiPpq);
    put(v, "bpm", m.midiTiming.bpm);
    put(v, "numerator", m.midiTiming.numerator);
    put(v, "denominator", m.midiTiming.denominator);
    juce::Array<juce::var> clips;
    for (auto &c : m.midiClips)
        clips.add(midiClipJson(c));
    put(v, "clips", clips);
    return v;
}
void midiMixFromJson(DesignMix &m, const juce::var &v)
{
    m.midiEnabled = true;
    require((int)v["version"] == 1 && v["clock"].toString() == "constant-tempo-microticks-v1" &&
                (int64_t)v["ppq"] == midiPpq && v["clips"].isArray() && v["clips"].size() <= 64,
            "Unsupported MIDI timeline contract");
    m.midiTiming = {(double)v["bpm"], (int)v["numerator"], (int)v["denominator"]};
    for (auto &c : *v["clips"].getArray())
        m.midiClips.push_back(midiClipFromJson(c));
}
std::vector<MidiPacket> compileMidi(const DesignMix &m, const juce::String &track, double rate)
{
    validateMidi(m, rate);
    std::vector<MidiPacket> out;
    auto add = [&](int64_t tick, int status, int a, int b, uint64_t voice, uint64_t scope, int flags = 0)
    {
        require(out.size() < 131072, "Compiled MIDI event bound exceeded");
        out.push_back({midiFrame(tick, m.midiTiming, rate), voice, scope, (uint8_t)status, (uint8_t)a,
                       (uint8_t)b, (uint8_t)flags});
    };
    // Joining untouched split fragments preserves one note articulation and controller history.
    auto clips = m.midiClips;
    std::stable_sort(clips.begin(), clips.end(), [](auto &a, auto &b) { return a.start < b.start; });
    auto payload = [](const MidiClip &c)
    {
        auto v = midiClipJson(c);
        auto *o = v.getDynamicObject();
        for (auto key : {"id", "start", "length", "offset", "track", "name", "origin", "recordingOrigin"})
            o->removeProperty(key);
        for (auto &n : *v["notes"].getArray())
            n.getDynamicObject()->removeProperty("id");
        for (auto &e : *v["controls"].getArray())
            e.getDynamicObject()->removeProperty("id");
        return juce::JSON::toString(v);
    };
    for (size_t i = 1; i < clips.size(); ++i)
        if (clips[i].track == track && !clips[i - 1].mute && !clips[i].mute && clips[i].origin.isNotEmpty() &&
            clips[i].origin == clips[i - 1].origin &&
            clips[i - 1].start + clips[i - 1].length == clips[i].start &&
            clips[i - 1].offset + clips[i - 1].length == clips[i].offset && clips[i].loopLength == 0 &&
            payload(clips[i]) == payload(clips[i - 1]))
        {
            clips[i].start = clips[i - 1].start;
            clips[i].offset = clips[i - 1].offset;
            clips[i].length += clips[i - 1].length;
            clips[i - 1].mute = true;
        }
    for (auto &c : clips)
        if (c.track == track && !c.mute)
        {
            int64_t period = c.loopLength ? c.loopLength : std::max<int64_t>(1, c.offset + c.length);
            int64_t firstLoop = c.offset / period, lastLoop = (c.offset + c.length - 1) / period;
            for (int64_t loop = firstLoop; loop <= lastLoop; ++loop)
            {
                auto left = std::max(c.offset, loop * period),
                     right = std::min(c.offset + c.length, (loop + 1) * period);
                auto scope = midiIdentity((c.origin.isEmpty() ? c.id : c.origin) + ":" + juce::String(loop));
                for (auto &n : c.notes)
                {
                    auto a = std::max(left, loop * period + n.tick),
                         b = std::min(right, loop * period + n.tick + n.duration);
                    if (b <= a)
                        continue;
                    auto voice = midiIdentity((n.tie.isEmpty() ? n.id : n.tie) + ":" + juce::String(loop));
                    add(c.start + a - c.offset, 0x90 + n.channel - 1, n.pitch, n.velocity, voice, scope);
                    add(c.start + b - c.offset, 0x80 + n.channel - 1, n.pitch, n.releaseVelocity, voice,
                        scope);
                }
                std::map<std::pair<int, int>, const MidiControl *> chase;
                for (auto &e : c.controls)
                {
                    auto at = loop * period + e.tick;
                    if (at < left)
                    {
                        auto key = std::pair(e.channel, e.type == "bend"      ? 128
                                                        : e.type == "program" ? 129
                                                                              : e.number);
                        auto it = chase.find(key);
                        if (it == chase.end() || it->second->tick <= e.tick)
                            chase[key] = &e;
                    }
                    if (at >= left && at < right)
                    {
                        int status = e.type == "bend" ? 0xe0 : e.type == "program" ? 0xc0 : 0xb0;
                        int a = e.type == "bend" ? (e.value & 127) : e.type == "program" ? e.value : e.number;
                        int b = e.type == "bend" ? (e.value >> 7) : e.value;
                        add(c.start + at - c.offset, status + e.channel - 1, a, b, 0, scope);
                    }
                }
                for (auto &[key, e] : chase)
                {
                    int status = e->type == "bend" ? 0xe0 : e->type == "program" ? 0xc0 : 0xb0;
                    add(c.start + left - c.offset, status + e->channel - 1,
                        e->type == "bend"      ? (e->value & 127)
                        : e->type == "program" ? e->value
                                               : e->number,
                        e->type == "bend" ? (e->value >> 7) : e->value, 0, scope);
                }
                for (int ch = 0; ch < 16; ++ch)
                {
                    add(c.start + right - c.offset, 0xb0 + ch, 64, 0, 0, scope, 1);
                    add(c.start + right - c.offset, 0xe0 + ch, 0, 64, 0, scope, 1);
                }
            }
        }
    auto order = [](const MidiPacket &p) {
        return p.flags ? 0 : ((p.status & 0xf0) == 0x80 ? 1 : (p.status & 0xf0) == 0x90 ? 3 : 2);
    };
    std::stable_sort(out.begin(), out.end(), [&](auto &a, auto &b)
                     { return a.frame != b.frame ? a.frame < b.frame : order(a) < order(b); });
    std::array<std::array<uint64_t, 2>, 16> owners{};
    std::erase_if(out,
                  [&](auto &p)
                  {
                      int ch = p.status & 15, type = p.status & 0xf0;
                      int index = type == 0xe0 ? 1 : 0;
                      bool controlled = type == 0xe0 || (type == 0xb0 && p.data1 == 64);
                      if (!controlled)
                          return false;
                      if (p.flags)
                      {
                          if (owners[ch][index] != p.scope)
                              return true;
                          owners[ch][index] = 0;
                      }
                      else
                          owners[ch][index] = p.scope;
                      return false;
                  });
    return out;
}
juce::String createMidiClip(DesignDocument &d, const juce::String &track, int64_t frame)
{
    auto *owner = findTrack(d.activeMix(), track);
    require(owner && owner->kind == TrackKind::midiInstrument, "Choose a MIDI Track");
    MidiClip c;
    c.track = track;
    c.start = midiTick(std::max<int64_t>(0, frame), d.activeMix().midiTiming, d.sampleRate);
    c.origin = c.id;
    auto id = c.id;
    d.activeMix().midiClips.push_back(c);
    d.selectedMidiClip = id;
    d.selectedLayer.clear();
    d.selectedTrack = track;
    return id;
}
void moveMidiClip(DesignDocument &d, const juce::String &id, const juce::String &track, int64_t frame,
                  bool copy)
{
    auto c = needed(d, id);
    auto *t = findTrack(d.activeMix(), track);
    require(t && t->kind == TrackKind::midiInstrument, "MIDI clips move only to MIDI Tracks");
    c.track = track;
    c.start = midiTick(std::max<int64_t>(0, frame), d.activeMix().midiTiming, d.sampleRate);
    if (copy)
    {
        rekey(c, true);
        d.activeMix().midiClips.push_back(c);
    }
    else
        needed(d, id) = c;
    d.selectedMidiClip = c.id;
    d.selectedLayer.clear();
    d.selectedTrack = track;
}
void trimMidiClip(DesignDocument &d, const juce::String &id, int64_t firstFrame, int64_t lastFrame)
{
    auto &c = needed(d, id);
    auto first = midiTick(firstFrame, d.activeMix().midiTiming, d.sampleRate),
         last = midiTick(lastFrame, d.activeMix().midiTiming, d.sampleRate);
    require(first >= 0 && last > first, "Invalid MIDI trim");
    auto delta = first - c.start;
    require(c.offset + delta >= 0, "MIDI trim exceeds available content");
    c.start = first;
    c.offset += delta;
    c.length = last - first;
}
void splitMidiClip(DesignDocument &d, const juce::String &id, int64_t frame)
{
    auto c = needed(d, id);
    auto cut = midiTick(frame, d.activeMix().midiTiming, d.sampleRate) - c.start;
    require(cut > 0 && cut < c.length, "Split inside the selected MIDI clip");
    if (c.origin.isEmpty())
        c.origin = c.id;
    for (auto &n : c.notes)
        if (n.tie.isEmpty())
            n.tie = n.id;
    needed(d, id) = c;
    needed(d, id).length = cut;
    c.start += cut;
    c.offset += cut;
    c.length -= cut;
    rekey(c, false);
    d.activeMix().midiClips.push_back(c);
    d.selectedMidiClip = c.id;
    d.selectedLayer.clear();
}
void quantizeMidiNotes(MidiClip &c, const std::set<juce::String> &ids, int64_t grid, double strength)
{
    require(grid > 0 && std::isfinite(strength) && strength >= 0 && strength <= 1,
            "Invalid quantize grid/strength");
    for (auto &n : c.notes)
        if (selected(ids, n.id))
        {
            auto target = (int64_t)std::llround((long double)n.tick / grid) * grid;
            n.tick = std::max<int64_t>(0, n.tick + (int64_t)std::llround((target - n.tick) * strength));
        }
}
void transposeMidiNotes(MidiClip &c, const std::set<juce::String> &ids, int semitones)
{
    for (auto &n : c.notes)
        if (selected(ids, n.id))
            n.pitch = std::clamp(n.pitch + semitones, 0, 127);
}
void velocityMidiNotes(MidiClip &c, const std::set<juce::String> &ids, int delta)
{
    for (auto &n : c.notes)
        if (selected(ids, n.id))
            n.velocity = std::clamp(n.velocity + delta, 1, 127);
}
} // namespace sp
