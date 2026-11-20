#include <juce_audio_processors/juce_audio_processors.h>
#include <cstdlib>
#include <cmath>
class Fixture final : public juce::AudioProcessor
{
    juce::AudioParameterFloat *gain, *latency, *creative, *tail;
    juce::AudioParameterBool *invert;
    juce::AudioParameterChoice *mode;
    std::vector<std::vector<float>> delay;
    int position = 0, delayFrames = 0, sleepMs = 0;
    float randomGain = .5f + juce::Random::getSystemRandom().nextFloat() * .5f;

  public:
    Fixture()
        : AudioProcessor(BusesProperties()
                             .withInput("Main", juce::AudioChannelSet::stereo(), true)
                             .withOutput("Main", juce::AudioChannelSet::stereo(), true))
    {
        if (auto *test = std::getenv("SOUNDPLUS_FIXTURE_SCAN_BEHAVIOR"))
        {
            if (juce::String(test) == "crash")
                std::abort();
            if (juce::String(test) == "hang")
                for (;;)
                    juce::Thread::sleep(100);
        }
        if (auto *marker = std::getenv("SOUNDPLUS_FIXTURE_SCAN_MARKER"))
        {
            juce::File(juce::String::fromUTF8(marker)).replaceWithText("Actual fixture constructor entered");
            juce::Thread::sleep(200);
        }
        if (auto *s = std::getenv("SOUNDPLUS_FIXTURE_BLOCK_SLEEP_MS"))
            sleepMs = juce::jlimit(0, 10, juce::String(s).getIntValue());
        addParameter(gain = new juce::AudioParameterFloat({"gain", 1}, "Gain", 0.f, 2.f, 1.f));
        addParameter(invert = new juce::AudioParameterBool({"invert", 1}, "Invert", false));
        addParameter(latency =
                         new juce::AudioParameterFloat({"latency", 1}, "Reported latency", 0.f, 256.f, 0.f));
        addParameter(creative =
                         new juce::AudioParameterFloat({"creative", 1}, "Creative delay", 0.f, 1024.f, 0.f));
        addParameter(tail = new juce::AudioParameterFloat({"tail", 1}, "Reported tail", 0.f, 2.f, 0.f));
        addParameter(mode =
                         new juce::AudioParameterChoice({"mode", 1}, "Test behavior",
                                                        {"Normal", "Nondeterministic", "Non-finite", "Crash",
                                                         "Hang", "Changing latency", "Unknown tail"},
                                                        0));
    }
    const juce::String getName() const override
    {
        return JucePlugin_Name;
    }
    bool acceptsMidi() const override
    {
        return false;
    }
    bool producesMidi() const override
    {
        return false;
    }
    bool isMidiEffect() const override
    {
        return false;
    }
    double getTailLengthSeconds() const override
    {
        return mode->getIndex() == 6 ? std::numeric_limits<double>::infinity() : tail->get();
    }
    bool isBusesLayoutSupported(const BusesLayout &b) const override
    {
        return b.inputBuses.size() == 1 && b.outputBuses.size() == 1 &&
               b.getMainInputChannelSet() == b.getMainOutputChannelSet() &&
               b.getMainInputChannelSet().size() >= 1 && b.getMainInputChannelSet().size() <= 16
#if SOUNDPLUS_FIXTURE_STEREO
               && b.getMainInputChannelSet() == juce::AudioChannelSet::stereo()
#endif
            ;
    }
    void prepareToPlay(double, int) override
    {
        setLatencySamples((int)std::lround(latency->get()));
        delayFrames = getLatencySamples() + (int)std::lround(creative->get());
        delay.assign(16, std::vector<float>(2048));
        reset();
    }
    void releaseResources() override {}
    void reset() override
    {
        for (auto &c : delay)
            std::fill(c.begin(), c.end(), 0.f);
        position = 0;
    }
    void processBlock(juce::AudioBuffer<float> &b, juce::MidiBuffer &) override
    {
        if (sleepMs)
            juce::Thread::sleep(sleepMs);
        if (mode->getIndex() == 3)
            std::abort();
        if (mode->getIndex() == 4)
            for (;;)
                juce::Thread::sleep(100);
        if (mode->getIndex() == 5)
            setLatencySamples(getLatencySamples() + 1);
        auto g = gain->get() * (invert->get() ? -1.f : 1.f) * (mode->getIndex() == 1 ? randomGain : 1.f);
        for (int s = 0; s < b.getNumSamples(); ++s)
        {
            for (int c = 0; c < b.getNumChannels(); ++c)
            {
                auto x = b.getSample(c, s);
                delay[(size_t)c][(size_t)position] = x;
                auto y = delay[(size_t)c][(size_t)((position + 2048 - delayFrames) % 2048)] * g;
                b.setSample(c, s, mode->getIndex() == 2 ? std::numeric_limits<float>::quiet_NaN() : y);
            }
            position = (position + 1) % 2048;
        }
    }
    bool hasEditor() const override
    {
        return false;
    }
    juce::AudioProcessorEditor *createEditor() override
    {
        return nullptr;
    }
    int getNumPrograms() override
    {
        return 1;
    }
    int getCurrentProgram() override
    {
        return 0;
    }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override
    {
        return "Default";
    }
    void changeProgramName(int, const juce::String &) override {}
    void getStateInformation(juce::MemoryBlock &b) override
    {
        b.setSize((size_t)getParameters().size() * sizeof(float));
        auto *p = static_cast<float *>(b.getData());
        for (auto *v : getParameters())
            *p++ = v->getValue();
    }
    void setStateInformation(const void *data, int bytes) override
    {
        if (bytes == getParameters().size() * (int)sizeof(float))
        {
            auto *p = static_cast<const float *>(data);
            for (auto *v : getParameters())
                v->setValueNotifyingHost(*p++);
        }
    }
};
juce::AudioProcessor *JUCE_CALLTYPE createPluginFilter()
{
    return new Fixture();
}
