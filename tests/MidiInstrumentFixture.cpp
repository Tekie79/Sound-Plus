#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <cstdlib>
class MidiFixture final : public juce::AudioProcessor
{
    juce::AudioParameterFloat *gain, *latency;
    std::array<std::array<float, 128>, 16> voices{}, releases{};
    std::array<bool, 16> sustain{};
    std::array<std::array<bool, 128>, 16> held{};
    std::array<std::array<float, 258>, 2> delay{};
    int pos = 0, latencyFrames = 0, program = 0;

  public:
    MidiFixture()
        : AudioProcessor(BusesProperties().withOutput("Instrument", juce::AudioChannelSet::stereo(), true))
    {
        addParameter(gain = new juce::AudioParameterFloat({"gain", 1}, "Gain", 0.f, 1.f, 1.f));
        addParameter(latency = new juce::AudioParameterFloat({"latency", 1}, "Latency",
                                                             juce::NormalisableRange<float>(0, 256, 1), 31));
    }
    const juce::String getName() const override
    {
        return "SoundPlus MIDI Oracle";
    }
    bool acceptsMidi() const override
    {
        return true;
    }
    bool producesMidi() const override
    {
        return false;
    }
    bool isMidiEffect() const override
    {
        return false;
    }
    bool isBusesLayoutSupported(const BusesLayout &l) const override
    {
        return l.getMainInputChannels() == 0 &&
               (l.getMainOutputChannels() == 1 || l.getMainOutputChannels() == 2);
    }
    void prepareToPlay(double, int) override
    {
        reset();
        latencyFrames = (int)latency->get();
        setLatencySamples(latencyFrames);
    }
    void releaseResources() override {}
    void reset() override
    {
        voices = {};
        releases = {};
        held = {};
        sustain = {};
        delay = {};
        pos = 0;
    }
    void processBlock(juce::AudioBuffer<float> &audio, juce::MidiBuffer &midi) override
    {
        if (auto *fault = std::getenv("SOUNDPLUS_MIDI_FIXTURE_FAULT"))
        {
            juce::String f(fault);
            if (f == "hang")
                for (;;)
                    juce::Thread::sleep(100);
            if (f == "crash")
                std::abort();
            if (f == "latency")
                setLatencySamples(latencyFrames + 1);
        }
        audio.clear();
        auto at = midi.begin();
        for (int n = 0; n < audio.getNumSamples(); ++n)
        {
            while (at != midi.end() && (*at).samplePosition <= n)
            {
                auto m = (*at).getMessage();
                int ch = m.getChannel() - 1;
                if (ch >= 0 && ch < 16)
                {
                    if (m.isNoteOn())
                    {
                        voices[ch][m.getNoteNumber()] = m.getFloatVelocity() * .25f;
                        held[ch][m.getNoteNumber()] = true;
                        releases[ch][m.getNoteNumber()] = 0;
                    }
                    else if (m.isNoteOff())
                    {
                        int p = m.getNoteNumber();
                        held[ch][p] = false;
                        if (!sustain[ch])
                            releases[ch][p] = voices[ch][p] / 64;
                    }
                    else if (m.isController())
                    {
                        int cc = m.getControllerNumber();
                        if (cc == 64)
                        {
                            sustain[ch] = m.getControllerValue() >= 64;
                            if (!sustain[ch])
                                for (int p = 0; p < 128; ++p)
                                    if (!held[ch][p])
                                        releases[ch][p] = voices[ch][p] / 64;
                        }
                        if (cc == 123)
                        {
                            held[ch] = {};
                            sustain[ch] = false;
                            for (int p = 0; p < 128; ++p)
                                releases[ch][p] = voices[ch][p] / 64;
                        }
                        if (cc == 120)
                        {
                            voices[ch] = {};
                            releases[ch] = {};
                            held[ch] = {};
                            sustain[ch] = false;
                        }
                    }
                }
                ++at;
            }
            float value = 0;
            for (int c = 0; c < 16; ++c)
                for (int p = 0; p < 128; ++p)
                {
                    value += voices[c][p];
                    voices[c][p] = std::max(0.f, voices[c][p] - releases[c][p]);
                }
            value *= gain->get() * (program ? .5f : 1.f);
            for (int c = 0; c < audio.getNumChannels(); ++c)
            {
                delay[c][pos] = value;
                audio.setSample(c, n, delay[c][(pos + 258 - latencyFrames) % 258]);
            }
            pos = (pos + 1) % 258;
        }
    }
    double getTailLengthSeconds() const override
    {
        return 64. / 48000;
    }
    int getNumPrograms() override
    {
        return 2;
    }
    int getCurrentProgram() override
    {
        return program;
    }
    void setCurrentProgram(int p) override
    {
        program = juce::jlimit(0, 1, p);
    }
    const juce::String getProgramName(int p) override
    {
        return p ? "Half" : "Full";
    }
    void changeProgramName(int, const juce::String &) override {}
    bool hasEditor() const override
    {
        return true;
    }
    juce::AudioProcessorEditor *createEditor() override
    {
        return new juce::GenericAudioProcessorEditor(*this);
    }
    void getStateInformation(juce::MemoryBlock &b) override
    {
        juce::MemoryOutputStream s(b, false);
        s.writeFloat(gain->get());
        s.writeFloat(latency->get());
        s.writeInt(program);
    }
    void setStateInformation(const void *data, int size) override
    {
        if (size != 12)
            return;
        juce::MemoryInputStream s(data, (size_t)size, false);
        gain->setValueNotifyingHost(s.readFloat());
        latency->setValueNotifyingHost(s.readFloat() / 256.f);
        program = s.readInt();
    }
};
juce::AudioProcessor *JUCE_CALLTYPE createPluginFilter()
{
    return new MidiFixture;
}
