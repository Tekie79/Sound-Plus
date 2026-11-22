#include "MidiInput.h"
#include "AudioStream.h"
#include "PluginProcessing.h"
#include <juce_cryptography/juce_cryptography.h>
#include <deque>
#include <fcntl.h>
#include <unistd.h>
namespace sp
{
namespace
{
juce::var obj()
{
    return juce::var(new juce::DynamicObject);
}
void put(juce::var &v, const char *key, juce::var value)
{
    v.getDynamicObject()->setProperty(key, value);
}
juce::String endpointUid(MIDIEndpointRef e)
{
    SInt32 id = 0;
    MIDIObjectGetIntegerProperty(e, kMIDIPropertyUniqueID, &id);
    return juce::String(id);
}
juce::String endpointName(MIDIEndpointRef e)
{
    CFStringRef n = nullptr;
    MIDIObjectGetStringProperty(e, kMIDIPropertyDisplayName, &n);
    if (!n)
        return "MIDI input";
    auto s = juce::String::fromCFString(n);
    CFRelease(n);
    return s;
}
juce::String hash(const juce::var &v)
{
    auto text = juce::JSON::toString(v, true);
    return juce::SHA256(text.toRawUTF8(), text.getNumBytesAsUTF8()).toHexString();
}
} // namespace
MidiInputService::MidiInputService(InputTimelineClock &c) : clock(c)
{
    mach_timebase_info(&timebase);
    MIDIClientCreate(CFSTR("SoundPlus MIDI"), notify, this, &client);
}
MidiInputService::~MidiInputService()
{
    accepting = false;
    monitorRequested = false;
    if (port)
        MIDIPortDispose(port);
    if (client)
        MIDIClientDispose(client);
    try
    {
        interrupt("MIDI helper exited");
        service(nullptr);
    }
    catch (...)
    {
    }
}
juce::Array<juce::var> MidiInputService::devices()
{
    juce::Array<juce::var> out;
    for (ItemCount i = 0; i < MIDIGetNumberOfSources(); ++i)
    {
        auto e = MIDIGetSource(i);
        auto v = obj();
        put(v, "uid", endpointUid(e));
        put(v, "name", endpointName(e));
        out.add(v);
    }
    return out;
}
void MidiInputService::notify(const MIDINotification *, void *context)
{
    static_cast<MidiInputService *>(context)->changed = true;
}
void MidiInputService::input(const MIDIPacketList *packets, void *context, void *)
{
    auto &s = *static_cast<MidiInputService *>(context);
    if (!s.accepting.load(std::memory_order_acquire))
        return;
    auto *p = packets->packet;
    for (UInt32 n = 0; n < packets->numPackets; ++n)
    {
        uint64_t ns = (uint64_t)((long double)(p->timeStamp ? p->timeStamp : mach_absolute_time()) *
                                 s.timebase.numer / s.timebase.denom);
        for (UInt16 b = 0; b < p->length; ++b)
        {
            uint8_t v = p->data[b];
            if (v >= 0xf8)
                continue;
            if (v >= 0x80)
            {
                s.running = v < 0xf0 ? v : 0;
                s.received = 0;
                continue;
            }
            if (!s.running)
                continue;
            s.data[s.received++] = v;
            int count = (s.running & 0xf0) == 0xc0 || (s.running & 0xf0) == 0xd0 ? 1 : 2;
            if (s.received == count)
            {
                s.receive(ns, s.running, s.data[0], count == 2 ? s.data[1] : 0);
                s.received = 0;
            }
        }
        p = MIDIPacketNext(p);
    }
}
void MidiInputService::receive(uint64_t host, uint8_t status, uint8_t a, uint8_t b) noexcept
{
    auto c = clock.read();
    int type = status & 0xf0;
    if (type != 0x80 && type != 0x90 && type != 0xb0 && type != 0xc0 && type != 0xe0)
        return;

    auto w = write.load(std::memory_order_relaxed);
    if (w - read.load(std::memory_order_acquire) >= capacity)
    {
        overflow = true;
        return;
    }
    auto frame = c.hostNs && c.rate > 0
                     ? c.frame + (int64_t)std::llround(((long double)host - c.hostNs) * c.rate / 1.e9L)
                     : 0;
    ring[w % capacity] = {epoch.load(std::memory_order_relaxed),
                          host,
                          c.hostNs && c.rate > 0 ? c.generation : 0,
                          std::max<int64_t>(0, frame),
                          status,
                          a,
                          b};
    write.store(w + 1, std::memory_order_release);
}
void MidiInputService::receiveForTest(uint64_t host, uint8_t status, uint8_t a, uint8_t b) noexcept
{
    receive(host, status, a, b);
}
void MidiInputService::configure(const juce::var &v)
{
    if (capturing.load())
        throw std::runtime_error("Stop MIDI recording before changing input");
    accepting = false;
    monitorRequested = false;
    if (port)
    {
        MIDIPortDispose(port);
        port = 0;
    }
    ++epoch;
    running = 0;
    received = 0;
    std::lock_guard lock(mutex);
    if (recording)
        throw std::runtime_error("Stop MIDI recording before changing input");
    panic = true;
    softwareQueue.clear();
    liveNotes.clear();
    softwareNotes.clear();
    uid = v["uid"].toString();
    channel = (int)v["channel"];
    if (channel < 0 || channel > 16)
        throw std::runtime_error("MIDI channel must be All or 1–16");
    owners.clear();
    monitored.clear();
    if (auto *a = v["owners"].getArray())
        for (auto &o : *a)
            owners.add(o.toString());
    if (auto *a = v["monitored"].getArray())
        for (auto &o : *a)
            monitored.add(o.toString());
    if (owners.size() > 16)
        throw std::runtime_error("At most16 MIDI input owners");
    error.clear();
    schedulingCount = 0;
    schedulingMaxMs = 0;
    endpoint = 0;
    if (uid.isEmpty())
        return;
    for (ItemCount i = 0; i < MIDIGetNumberOfSources(); ++i)
        if (endpointUid(MIDIGetSource(i)) == uid)
            endpoint = MIDIGetSource(i);
    if (!endpoint)
        throw std::runtime_error("MIDI device unavailable");
    if (MIDIInputPortCreate(client, CFSTR("SoundPlus MIDI input"), input, this, &port) != noErr ||
        MIDIPortConnectSource(port, endpoint, nullptr) != noErr)
    {
        if (port)
            MIDIPortDispose(port);
        port = 0;
        throw std::runtime_error("CoreMIDI input connection failed");
    }
    accepting = true;
    monitorRequested = !monitored.isEmpty();
}
void MidiInputService::software(const juce::var &v)
{
    std::lock_guard lock(mutex);
    if (softwareQueue.size() >= 1024)
        throw std::runtime_error("Software-key MIDI queue full");
    int status = (int)v["status"], a = (int)v["a"], b = (int)v["b"];
    if ((status != 0x90 && status != 0x80) || a < 0 || a > 127 || b < 0 || b > 127)
        throw std::runtime_error("Invalid software-key MIDI");
    MidiPacket p;
    p.frame = clock.read().frame;
    p.status = (uint8_t)status;
    p.data1 = (uint8_t)a;
    p.data2 = (uint8_t)b;
    softwareQueue.emplace_back(v["owner"].toString(), p);
}
void MidiInputService::poll()
{
    if (!changed.exchange(false))
        return;
    std::lock_guard lock(mutex);
    if (endpoint)
    {
        bool present = false;
        for (ItemCount i = 0; i < MIDIGetNumberOfSources(); ++i)
            present |= MIDIGetSource(i) == endpoint;
        if (!present)
        {
            accepting = false;
            error = "MIDI device disconnected";
            panic = true;
            if (recording)
            {
                finishing = true;
                auto c = clock.read();
                if (c.generation == generation)
                    last = std::max(last, c.frame);
            }
        }
    }
}
void MidiInputService::begin(const juce::var &v)
{
    std::lock_guard lock(mutex);
    if (recording || owners.isEmpty() || !accepting)
        throw std::runtime_error("Arm a connected MIDI input before recording");
    id = v["id"].toString();
    document = v["document"].toString();
    variation = v["variation"].toString();
    first = (int64_t)v["first"];
    last = first;
    generation = (uint64_t)(int64_t)v["generation"];
    rate = (double)v["rate"];
    timing = {(double)v["bpm"], (int)v["numerator"], (int)v["denominator"]};
    if ((id.length() != 32 && id.length() != 36) || !id.containsOnly("0123456789abcdefABCDEF-") ||
        first < 0 || !std::isfinite(rate) || rate < 8000 || rate > 192000 || first >= rate * 3600)
        throw std::runtime_error("Invalid MIDI recording plan");
    (void)midiFrame(0, timing, rate);
    journal = juce::File(v["root"].toString()).getChildFile(id + ".midi-partial.json");
    eventLimit =
        std::min(v.hasProperty("maxEvents") ? (int)v["maxEvents"] : 8192, 8192 / std::max(1, owners.size()));
    if (eventLimit < 1 || eventLimit > 8192)
        throw std::runtime_error("MIDI recording event budget unavailable");
    liveNotes.clear();
    recordOwners = owners;
    events.clear();
    events.reserve(65536);
    receipt = juce::var();
    recording = true;
    capturing = true;
    finishing = cancelled = false;
    overflow = false;
    error.clear();
    late = maxLate = receivedCount = 0;
    schedulingCount = 0;
    schedulingMaxMs = 0;
    nextCheckpoint = 0;
    try
    {
        checkpoint(false);
    }
    catch (const std::exception &e)
    {
        recording = false;
        capturing = false;
        error = e.what();
        receipt = payload();
        put(receipt, "complete", false);
        throw;
    }
}
void MidiInputService::finish(bool cancel, juce::String reason)
{
    std::lock_guard lock(mutex);
    if (!recording)
        return;
    finishing = true;
    cancelled = cancel;
    auto c = clock.read();
    if (c.generation == generation)
        last = std::max(last, c.frame);
    else if (reason.isEmpty())
        reason = "MIDI transport generation changed";
    if (reason.isNotEmpty())
        error = reason;
}
void MidiInputService::interrupt(const juce::String &reason)
{
    finish(false, reason);
}
std::vector<MidiClip> MidiInputService::clips(const std::vector<CapturedMidi> &events,
                                              const juce::StringArray &owners, int64_t first, int64_t last,
                                              const MidiTiming &timing, double rate, const juce::var &origin)
{
    MidiClip base;
    base.start = midiTick(first, timing, rate);
    base.length = std::max<int64_t>(1, midiTick(std::max(first + 1, last), timing, rate) - base.start);
    base.origin = base.id;
    base.name = "MIDI recording";
    base.recordingOrigin = origin;
    std::array<std::array<std::deque<size_t>, 128>, 16> held;
    auto tick = [&](int64_t frame)
    { return std::clamp<int64_t>(midiTick(frame, timing, rate) - base.start, 0, base.length); };
    for (auto &r : events)
    {
        auto &p = r.event;
        if (p.frame < first || p.frame >= last)
            continue;
        int type = p.status & 0xf0, ch = p.status & 15;
        if (type == 0x90 && p.data2)
        {
            MidiNote n;
            n.tick = tick(p.frame);
            n.duration = std::max<int64_t>(1, base.length - n.tick);
            n.pitch = p.data1;
            n.velocity = p.data2;
            n.channel = ch + 1;
            held[ch][n.pitch].push_back(base.notes.size());
            base.notes.push_back(n);
        }
        else if (type == 0x80 || (type == 0x90 && !p.data2))
        {
            auto &q = held[ch][p.data1];
            if (!q.empty())
            {
                auto &n = base.notes[q.front()];
                q.pop_front();
                n.duration = std::max<int64_t>(1, tick(p.frame) - n.tick);
                n.releaseVelocity = p.data2;
            }
        }
        else if (type == 0xb0 || type == 0xe0 || type == 0xc0)
        {
            MidiControl c;
            c.tick = tick(p.frame);
            c.channel = ch + 1;
            c.type = type == 0xe0 ? "bend" : type == 0xc0 ? "program" : "cc";
            c.number = type == 0xb0 ? p.data1 : 0;
            c.value = type == 0xe0 ? p.data1 + 128 * p.data2 : type == 0xc0 ? p.data1 : p.data2;
            base.controls.push_back(c);
        }
    }
    std::vector<MidiClip> out;
    for (auto &owner : owners)
    {
        auto c = base;
        c.id = juce::Uuid().toString();
        c.origin = c.id;
        c.track = owner;
        for (auto &n : c.notes)
        {
            n.id = juce::Uuid().toString();
            n.tie = n.id;
        }
        for (auto &e : c.controls)
            e.id = juce::Uuid().toString();
        out.push_back(std::move(c));
    }
    return out;
}
juce::var MidiInputService::payload() const
{
    auto v = obj();
    put(v, "version", 1);
    put(v, "policy", "timestamped-applied-frame-midi-v1");
    put(v, "id", id);
    put(v, "document", document);
    put(v, "variation", variation);
    put(v, "first", (juce::int64)first);
    put(v, "last", (juce::int64)last);
    put(v, "rate", rate);
    put(v, "generation", (juce::int64)generation);
    put(v, "bpm", timing.bpm);
    put(v, "numerator", timing.numerator);
    put(v, "denominator", timing.denominator);
    put(v, "lateEvents", (juce::int64)late);
    put(v, "maxLateFrames", (juce::int64)maxLate);
    put(v, "reason", error);
    juce::Array<juce::var> own, es;
    for (auto &s : recordOwners)
        own.add(s);
    put(v, "owners", own);
    for (auto &r : events)
    {
        auto e = obj();
        put(e, "frame", (juce::int64)r.event.frame);
        put(e, "acquired", (juce::int64)r.acquired);
        put(e, "hostNs", (juce::int64)r.hostNs);
        put(e, "status", r.event.status);
        put(e, "a", r.event.data1);
        put(e, "b", r.event.data2);
        es.add(e);
    }
    put(v, "events", es);
    return v;
}
void MidiInputService::checkpoint(bool complete)
{
    auto v = payload();
    put(v, "complete", complete);
    put(v, "cancelled", cancelled);
    auto wrapper = obj();
    put(wrapper, "payload", v);
    put(wrapper, "sha256", hash(v));
    journal.getParentDirectory().createDirectory();
    juce::TemporaryFile temp(journal);
    auto output = temp.getFile().createOutputStream();
    if (!output || !output->writeText(juce::JSON::toString(wrapper, true), false, false, nullptr))
        throw std::runtime_error("Cannot stage MIDI checkpoint");
    output->flush();
    if (output->getStatus().failed())
        throw std::runtime_error("Cannot fsync MIDI checkpoint");
    output.reset();
    if (!temp.overwriteTargetFileWithTemporary())
        throw std::runtime_error("Cannot publish MIDI checkpoint");
    int directory = ::open(journal.getParentDirectory().getFullPathName().toRawUTF8(), O_RDONLY);
    if (directory >= 0)
    {
        ::fsync(directory);
        ::close(directory);
    }

    if (complete)
    {
        auto origin = obj();
        put(origin, "kind", "coremidi-v1");
        put(origin, "journal", journal.getFullPathName());
        put(origin, "sha256", hash(v));
        auto cs = clips(events, recordOwners, first, last, timing, rate, origin);
        juce::Array<juce::var> list;
        for (auto &c : cs)
            list.add(midiClipJson(c));
        put(v, "clips", list);
        receipt = v;
    }
    else
        receipt = v;
}
void MidiInputService::service(PcmStream *stream)
{
    std::lock_guard lock(mutex);
    if (overflow.exchange(false))
    {
        error = "MIDI queue overflow; incomplete pass retained";
        read.store(write.load(std::memory_order_acquire), std::memory_order_release);
        panic = true;
        if (recording)
        {
            finishing = true;
            auto c = clock.read();
            if (c.generation == generation)
                last = std::max(last, c.frame);
        }
    }
    if (panic && stream)
    {
        liveNotes.clear();
        softwareNotes.clear();
        stream->midiPanic({});
        panic = false;
    }
    if (stream && stream->isDesign())
    {
        while (!softwareQueue.empty())
        {
            auto p = std::move(softwareQueue.front());
            softwareQueue.pop_front();
            try
            {
                auto key = p.first + ":" + juce::String(p.second.data1);
                if ((p.second.status & 0xf0) == 0x90 && p.second.data2)
                {
                    p.second.voice = ++noteSequence;
                    auto applied = stream->injectMidi(p.first, p.second);
                    softwareNotes[key] = {p.second.voice, applied};
                }
                else
                {
                    auto at = softwareNotes.find(key);
                    if (at != softwareNotes.end())
                    {
                        p.second.voice = at->second.first;
                        p.second.frame = std::max(
                            p.second.frame, at->second.second + (int64_t)(stream->originalRate() * .03));
                        softwareNotes.erase(at);
                    }
                    stream->injectMidi(p.first, p.second);
                }
            }
            catch (const std::exception &e)
            {
                error = e.what();
            }
        }
    }
    auto r = read.load(std::memory_order_relaxed), w = write.load(std::memory_order_acquire);
    for (; r < w; ++r)
    {
        auto raw = ring[r % capacity];
        if (!raw.generation)
        {
            auto current = clock.read();
            if (!current.hostNs || current.rate <= 0)
            {
                if (recording)
                    break;
                else
                    continue;
            }
            raw.generation = current.generation;
            raw.acquired = std::max<int64_t>(
                0, current.frame + (int64_t)std::llround(((long double)raw.host - current.hostNs) *
                                                         current.rate / 1.e9L));
        }

        if (raw.epoch != epoch.load() || (channel && channel != (raw.status & 15) + 1))
            continue;
        ++receivedCount;
        auto c = clock.read();
        if (!c.hostNs || c.rate <= 0)
        {
            if (recording)
                break;
            continue;
        }
        if (raw.generation != c.generation || (recording && raw.generation != generation))
        {
            if (recording && raw.generation > generation)
            {
                error = "MIDI transport generation changed";
                finishing = true;
                last = std::max(first + 1, last);
            }
            continue;
        }
        if (std::abs((long double)raw.acquired - c.frame) > c.rate * 5)
        {
            error = "MIDI timestamp outside the shared clock safety window";
            panic = true;
            if (recording)
                finishing = true;
            continue;
        }
        MidiPacket p;
        p.frame = raw.acquired;
        p.status = raw.status;
        p.data1 = raw.a;
        p.data2 = raw.b;
        auto key = std::pair{(int)(p.status & 15), (int)p.data1};
        if ((p.status & 0xf0) == 0x90 && p.data2)
        {
            if (liveNotes[key].size() >= 256)
            {
                error = "Held MIDI note safety limit";
                panic = true;
                continue;
            }
            p.voice = ++noteSequence;
            liveNotes[key].push_back(p.voice);
        }
        else if ((p.status & 0xf0) == 0x80 || ((p.status & 0xf0) == 0x90 && !p.data2))
        {
            auto at = liveNotes.find(key);
            if (at != liveNotes.end() && !at->second.empty())
            {
                p.voice = at->second.front();
                at->second.pop_front();
                if (at->second.empty())
                    liveNotes.erase(at);
            }
            else
                p.voice = UINT64_MAX;
        }

        int64_t applied = p.frame;
        if (stream)
            for (auto &o : monitored)
                applied = std::max(applied, stream->injectMidi(o, p));
        p.frame = applied;
        const auto delayMs = std::max<int64_t>(0, applied - raw.acquired) * 1000. / c.rate;
        schedulingDelayMs[schedulingCount++ % schedulingDelayMs.size()] = delayMs;
        schedulingMaxMs = std::max(schedulingMaxMs, delayMs);
        if (recording)
        {
            if (events.size() >= (size_t)eventLimit)
            {
                error = "MIDI pass event capacity reached";
                finishing = true;
                last = std::max(first + 1, last);
                break;
            }
            if (p.frame > raw.acquired)
            {
                ++late;
                maxLate = std::max(maxLate, (uint64_t)(p.frame - raw.acquired));
            }
            events.push_back({p, raw.acquired, raw.host});
        }
    }
    read.store(r, std::memory_order_release);
    if (recording)
    {
        if (!finishing)
        {
            auto c = clock.read();
            last = c.generation == generation ? std::max(last, c.frame) : last;
            if (last - first >= rate * 120 || last >= rate * 3600)
            {
                error = "MIDI recording reached120-second limit";
                finishing = true;
            }
        }
        auto now = juce::Time::getMillisecondCounterHiRes();
        if (finishing)
        {
            last =
                std::clamp(last, first + 1, std::min(first + (int64_t)(rate * 120), (int64_t)(rate * 3600)));
            bool complete = !cancelled && error.isEmpty();
            try
            {
                checkpoint(complete);
            }
            catch (const std::exception &e)
            {
                error = e.what();
                receipt = payload();
                put(receipt, "complete", false);
            }
            recording = false;
            capturing = false;
            finishing = false;
            panic = true;
        }
        else if (now >= nextCheckpoint)
        {
            try
            {
                checkpoint(false);
            }
            catch (const std::exception &e)
            {
                error = e.what();
                recording = false;
                capturing = false;
                receipt = payload();
                put(receipt, "complete", false);
                panic = true;
            }
            nextCheckpoint = now + 500;
        }
    }
}
juce::var MidiInputService::status()
{
    std::lock_guard lock(mutex);
    auto v = obj();
    put(v, "open", accepting.load());
    put(v, "uid", uid);
    put(v, "recording", recording);
    put(v, "error", error);
    auto summary = obj();
    for (auto key : {"id", "document", "variation", "complete", "cancelled", "reason", "first", "last"})
        put(summary, key, receipt[key]);
    put(summary, "journal", journal.getFullPathName());
    put(v, "receipt", summary);
    put(v, "events", (juce::int64)receivedCount);
    put(v, "lateEvents", (juce::int64)late);
    put(v, "maxLateFrames", (juce::int64)maxLate);
    auto latency = obj();
    auto count = std::min<uint64_t>(schedulingCount, schedulingDelayMs.size());
    std::vector<double> sorted(schedulingDelayMs.begin(), schedulingDelayMs.begin() + (ptrdiff_t)count);
    std::sort(sorted.begin(), sorted.end());
    auto percentile = [&](double p)
    { return sorted.empty() ? 0. : sorted[(size_t)std::ceil(p * (sorted.size() - 1))]; };
    put(latency, "policy", "acquisition-to-applied-v1");
    put(latency, "events", (juce::int64)schedulingCount);
    put(latency, "window", (int)count);
    put(latency, "p50Ms", percentile(.5));
    put(latency, "p95Ms", percentile(.95));
    put(latency, "p99Ms", percentile(.99));
    put(latency, "maximumMs", schedulingMaxMs);
    put(v, "schedulingLatency", latency);
    put(v, "availableDevices", devices());
    return v;
}
juce::var MidiInputService::recover(const juce::File &file)
{
    auto wrapper = loadProcessingJson(file), v = wrapper["payload"];
    if ((int)v["version"] != 1 || v["policy"].toString() != "timestamped-applied-frame-midi-v1" ||
        wrapper["sha256"].toString() != hash(v) || !v["events"].isArray() || v["events"].size() > 8192 ||
        !v["owners"].isArray() || v["owners"].size() > 16)
        throw std::runtime_error("MIDI recovery integrity/policy failed; journal retained");
    auto first = (int64_t)v["first"], last = (int64_t)v["last"];
    auto rate = (double)v["rate"];
    auto ownerCount = v["owners"].size();
    if (ownerCount < 1 || v["events"].size() > 8192 / ownerCount || !std::isfinite(rate) || rate < 8000 ||
        rate > 192000 || first < 0 || last <= first || last - first > rate * 120 + 1 || last > rate * 3600)
        throw std::runtime_error("Invalid retained MIDI clock/event budget");
    std::vector<CapturedMidi> events;
    for (auto &e : *v["events"].getArray())
    {
        int status = (int)e["status"], a = (int)e["a"], b = (int)e["b"];
        if (status < 0x80 || status > 0xef || a < 0 || a > 127 || b < 0 || b > 127)
            throw std::runtime_error("Invalid retained MIDI packet");
        events.push_back({{(int64_t)e["frame"], 0, 0, (uint8_t)status, (uint8_t)a, (uint8_t)b, 0},
                          (int64_t)e["acquired"],
                          (uint64_t)(int64_t)e["hostNs"]});
    }
    juce::StringArray owners;
    for (auto &o : *v["owners"].getArray())
        owners.add(o.toString());
    auto origin = obj();
    put(origin, "kind", (bool)v["complete"] ? "coremidi-v1" : "recovered-coremidi-v1");
    put(origin, "journal", file.getFullPathName());
    put(origin, "sha256", wrapper["sha256"]);
    juce::Array<juce::var> out;
    for (auto &c :
         clips(events, owners, (int64_t)v["first"], (int64_t)v["last"],
               {(double)v["bpm"], (int)v["numerator"], (int)v["denominator"]}, (double)v["rate"], origin))
    {
        c.name = (bool)v["complete"] ? "MIDI recording" : "Recovered MIDI";
        out.add(midiClipJson(c));
    }
    put(v, "clips", out);
    put(v, "recovered", true);
    return v;
}
} // namespace sp
