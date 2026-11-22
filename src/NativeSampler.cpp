#include "NativeSampler.h"
#include "SourcePools.h"
#include <cmath>
namespace sp
{
namespace
{
void require(bool b, const char *s)
{
    if (!b)
        throw std::runtime_error(s);
}
} // namespace
NativeSampler::NativeSampler(const InstrumentSlot &s, const ChannelLayout &b, double r)
    : settings(s), bus(b), rate(r)
{
    require(s.format == "SoundPlus" && s.identifier == "sampler-v1" && !s.samples.empty(),
            "Choose a sampler source");
    volume.fill(1);
    expression.fill(1);
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    for (auto reference : s.samples)
        if (reference.enabled)
        {
            verifyPoolEntry(reference);
            require(reference.reviewed && reference.reviewedBus == b && !reference.routing.needsSelection,
                    "Review sampler source channels for this Instrument bus");
            auto frames = reference.region.endSample - reference.region.startSample;
            require(frames > 0 && reference.source.channels <= 16 &&
                        frames * reference.source.channels <= 8 * 1024 * 1024,
                    "Sampler source exceeds8Mi prepared float samples; choose a shorter range");
            auto reader = std::unique_ptr<juce::AudioFormatReader>(
                formats.createReaderFor(juce::File(reference.source.path)));
            require(reader != nullptr, "Sampler source unavailable");
            Sample sample;
            sample.reference = reference;
            sample.pcm.setSize(reference.source.channels, (int)frames);
            require(reader->read(&sample.pcm, 0, (int)frames, reference.region.startSample, true, true),
                    "Cannot decode sampler source");
            for (int c = 0; c < sample.pcm.getNumChannels(); ++c)
                for (int n = 0; n < sample.pcm.getNumSamples(); ++n)
                    require(std::isfinite(sample.pcm.getSample(c, n)), "Nonfinite sampler source");
            sample.matrix = routingMatrix(reference.routing, b, 0);
            samples.push_back(std::move(sample));
        }
    require(!samples.empty(), "Enable a sampler source");
    verify();
}
void NativeSampler::verify() const
{
    for (auto &s : samples)
    {
        FileStamp stamp;
        require(fileStamp(juce::File(s.reference.source.path), stamp) &&
                    stamp.size == s.reference.source.size && stamp.modified == s.reference.source.modified &&
                    stamp.inode == s.reference.source.inode,
                "Sampler source changed or went offline; original retained");
    }
}
size_t NativeSampler::sourceFloats() const
{
    size_t n = 0;
    for (auto &s : samples)
        n += (size_t)s.pcm.getNumChannels() * s.pcm.getNumSamples();
    return n;
}
int NativeSampler::activeVoices() const
{
    int n = 0;
    for (auto &v : voices)
        n += v.active;
    return n;
}
void NativeSampler::stage(Voice &v, int s)
{
    v.stage = s;
    if (s == 0)
    {
        v.level = 0;
        v.remaining = (int64_t)std::llround(settings.attack * rate);
        v.step = v.remaining ? 1. / v.remaining : 0;
        if (!v.remaining)
        {
            v.level = 1;
            stage(v, 1);
        }
    }
    else if (s == 1)
    {
        v.remaining = (int64_t)std::llround(settings.decay * rate);
        v.step = v.remaining ? (1 - settings.sustain) / v.remaining : 0;
        if (!v.remaining)
        {
            v.level = settings.sustain;
            v.stage = 2;
        }
    }
    else if (s == 3)
    {
        v.remaining = (int64_t)std::llround(settings.release * rate);
        v.step = v.remaining ? v.level / v.remaining : 0;
        if (!v.remaining)
            v.active = false;
    }
}
void NativeSampler::release(Voice &v)
{
    v.keyDown = false;
    v.pedal = false;
    stage(v, 3);
}
void NativeSampler::advance(Voice &v)
{
    if (v.stage == 0)
    {
        v.level += v.step;
        if (--v.remaining <= 0)
        {
            v.level = 1;
            stage(v, 1);
        }
    }
    else if (v.stage == 1)
    {
        v.level -= v.step;
        if (--v.remaining <= 0)
        {
            v.level = settings.sustain;
            v.stage = 2;
        }
    }
    else if (v.stage == 3)
    {
        v.level = std::max(0., v.level - v.step);
        if (--v.remaining <= 0)
            v.active = false;
    }
}
void NativeSampler::allNotesOff(bool immediate)
{
    sustain.fill(false);
    for (auto &v : voices)
        if (v.active)
        {
            if (immediate)
                v.active = false;
            else
                release(v);
        }
}
void NativeSampler::update(const InstrumentSlot &s)
{
    require(s.id == settings.id && s.samples.size() == settings.samples.size(), "Sampler topology changed");
    settings = s;
    for (size_t i = (size_t)s.polyphony; i < voices.size(); ++i)
        voices[i].active = false;
}
void NativeSampler::event(const MidiPacket &p)
{
    int type = p.status & 0xf0, ch = p.status & 15;
    if (type == 0x90 && p.data2)
    {
        Voice *chosen = nullptr;
        for (int i = 0; i < settings.polyphony; ++i)
            if (!voices[(size_t)i].active)
            {
                chosen = &voices[(size_t)i];
                break;
            }
        if (!chosen)
        {
            chosen = &voices[0];
            for (int i = 1; i < settings.polyphony; ++i)
            {
                auto &v = voices[(size_t)i];
                if ((v.stage == 3 && chosen->stage != 3) ||
                    (v.stage == chosen->stage && v.age < chosen->age) ||
                    (chosen->stage != 3 && v.age < chosen->age))
                    chosen = &v;
            }
            ++stolen;
        }
        *chosen = {};
        chosen->active = chosen->keyDown = true;
        chosen->note = p.data1;
        chosen->channel = ch;
        chosen->token = p.voice;
        chosen->age = ++age;
        chosen->velocity = p.data2 / 127.;
        chosen->sample = (programs[ch] + (settings.roundRobin ? ordinal++ : 0)) % samples.size();
        stage(*chosen, 0);
    }
    else if (type == 0x80 || (type == 0x90 && !p.data2))
    {
        Voice *chosen = nullptr;
        for (auto &v : voices)
            if (v.active && v.keyDown && v.channel == ch && v.note == p.data1 &&
                (!p.voice || v.token == p.voice) && (!chosen || v.age < chosen->age))
                chosen = &v;
        if (chosen && !settings.oneShot)
        {
            chosen->keyDown = false;
            if (sustain[ch])
                chosen->pedal = true;
            else
                release(*chosen);
        }
    }
    else if (type == 0xe0)
        bend[ch] = ((p.data1 + (p.data2 << 7)) - 8192) / 8192. * settings.bendRange;
    else if (type == 0xc0)
        programs[ch] = p.data1;
    else if (type == 0xb0)
    {
        if (p.data1 == 64)
        {
            sustain[ch] = p.data2 >= 64;
            if (!sustain[ch])
                for (auto &v : voices)
                    if (v.active && v.channel == ch && v.pedal)
                        release(v);
        }
        else if (p.data1 == 7)
            volume[ch] = p.data2 / 127.;
        else if (p.data1 == 11)
            expression[ch] = p.data2 / 127.;
        else if (p.data1 == 120 || p.data1 == 123)
        {
            sustain[ch] = false;
            for (auto &v : voices)
                if (v.active && v.channel == ch)
                {
                    if (p.data1 == 120)
                        v.active = false;
                    else
                        release(v);
                }
        }
    }
}
void NativeSampler::render(juce::AudioBuffer<float> &out, int count, int64_t first,
                           const std::vector<MidiPacket> &events, size_t &next,
                           const CompiledAutomation *automation, int64_t inputEnd)
{
    double gain = settings.instrumentGainDb, transpose = settings.transpose;
    int64_t controlEnd = -1;
    for (int n = 0; n < count; ++n)
    {
        auto frame = first + n;
        if (inputEnd >= 0 && frame >= inputEnd && !gated)
        {
            allNotesOff();
            gated = true;
        }
        while (next < events.size() && events[next].frame <= frame)
        {
            if (inputEnd < 0 || events[next].frame < inputEnd)
                event(events[next]);
            ++next;
        }
        if (automation && frame >= controlEnd)
        {
            auto t = automation->segmentFrame(std::max<int64_t>(0, frame));
            gain = automation->value({settings.id, "instrument-gain"}, settings.instrumentGainDb, t);
            transpose = automation->value({settings.id, "tuning"}, settings.transpose, t);
            controlEnd = frame + automation->untilBoundary(frame, count - n);
        }
        for (auto &v : voices)
            if (v.active)
            {
                auto &s = samples[(size_t)v.sample];
                int length = s.pcm.getNumSamples();
                if (v.position >= length)
                {
                    if (settings.sampleLoop)
                        v.position = std::fmod(v.position, (double)length);
                    else
                    {
                        v.active = false;
                        continue;
                    }
                }
                int i = (int)v.position;
                double f = v.position - i;
                auto amplitude = (float)(v.level * v.velocity * volume[v.channel] * expression[v.channel] *
                                         std::pow(10., gain / 20));
                for (int c = 0; c < bus.size(); ++c)
                {
                    float value = 0;
                    for (int k = 0; k < s.pcm.getNumChannels(); ++k)
                    {
                        int j = i + 1 < length ? i + 1 : settings.sampleLoop ? 0 : i;
                        auto sample = s.pcm.getSample(k, i) * (1 - f) + s.pcm.getSample(k, j) * f;
                        value += (float)(sample * s.matrix[(size_t)c][(size_t)k]);
                    }
                    out.addSample(c, n, value * amplitude);
                }
                v.position += s.reference.source.sampleRate / rate *
                              std::pow(2., (v.note - settings.rootNote + transpose + bend[v.channel]) / 12.);
                advance(v);
            }
    }
}
} // namespace sp
