#include <juce_audio_processors/juce_audio_processors.h>
#include <cstdlib>
#include <cmath>
#include <unistd.h>
namespace
{
juce::String topology()
{
    auto *mode = std::getenv("SOUNDPLUS_FIXTURE_TOPOLOGY");
    return mode ? juce::String(mode) : juce::String();
}

} // namespace
class AliasParameter final : public juce::AudioParameterFloat
{
  public:
    explicit AliasParameter(int n, bool automatable)
        : AudioParameterFloat({"alias-" + juce::String(n), 1}, "Alias " + juce::String(n),
                              juce::NormalisableRange<float>(0, 1), 0,
                              juce::AudioParameterFloatAttributes().withAutomatable(automatable))
    {
    }
};
class Fixture final : public juce::AudioProcessor
{
    static BusesProperties fixtureBuses()
    {
        BusesProperties p;
        if (topology() == "midi-only")
            return p;
        p = p.withInput("Main", juce::AudioChannelSet::stereo(), true)
                .withOutput("Main", juce::AudioChannelSet::stereo(), true);
        if (topology() == "optional" || topology() == "mandatory")
            p = p.withInput("Sidechain", juce::AudioChannelSet::stereo(), true);
        return p;
    }
    juce::AudioParameterFloat *gain, *latency, *creative, *tail, *bias;
    juce::AudioParameterBool *invert;
    juce::AudioParameterChoice *mode;
    std::vector<std::vector<float>> delay;
    int position = 0, delayFrames = 0, sleepMs = 0, program = 0;
    std::atomic<float> opaqueGain{1.f};
    std::atomic<int> processingDepth{0};
    float randomGain = .5f + juce::Random::getSystemRandom().nextFloat() * .5f;

  public:
    Fixture() : AudioProcessor(fixtureBuses())
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
        if (auto *schema = std::getenv("SOUNDPLUS_FIXTURE_SCHEMA"))
        {
            auto name = juce::String(schema);
            int count = name == "overflow" ? 16385 : 580;
            for (int i = 0; i < count; ++i)
                addParameter(
                    new AliasParameter(name == "reversed" ? count - i - 1 : i, name == "inline-overflow"));
        }
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
        addParameter(bias = new juce::AudioParameterFloat({"bias", 1}, "QA bias", -1.f, 1.f, 0.f));
    }
    const juce::String getName() const override
    {
        return JucePlugin_Name;
    }
    bool acceptsMidi() const override
    {
        return topology() == "midi" || topology() == "midi-only";
    }
    bool producesMidi() const override
    {
        return topology() == "midi" || topology() == "midi-only";
    }
    bool isMidiEffect() const override
    {
        return topology() == "midi-only";
    }
    double getTailLengthSeconds() const override
    {
        return mode->getIndex() == 6 ? std::numeric_limits<double>::infinity() : tail->get();
    }
    bool isBusesLayoutSupported(const BusesLayout &b) const override
    {
        auto aux = topology() == "optional" || topology() == "mandatory";
        if (topology() == "mandatory")
            return false; // Deliberately unsupported required processing topology.
        if (aux && (b.inputBuses.size() != 2 || (topology() == "mandatory" && b.inputBuses[1].isDisabled())))
            return false;
        return b.inputBuses.size() == (aux ? 2 : 1) && b.outputBuses.size() == 1 &&
               b.getMainInputChannelSet() == b.getMainOutputChannelSet() &&
               b.getMainInputChannelSet().size() >= 1 && b.getMainInputChannelSet().size() <= 16
#if SOUNDPLUS_FIXTURE_STEREO
               && b.getMainInputChannelSet() == juce::AudioChannelSet::stereo()
#endif
            ;
    }
    void prepareToPlay(double, int) override
    {
        if (auto *log = std::getenv("SOUNDPLUS_FIXTURE_PREPARE_LOG"))
            juce::File(juce::String::fromUTF8(log))
                .appendText(juce::String((int)::getpid()) + "\n", false, false, "\n");
        setLatencySamples((int)std::lround(latency->get()));
        delayFrames = getLatencySamples() + (int)std::lround(creative->get());
        delay.assign(16, std::vector<float>(2048));
        reset();
    }
    void releaseResources() override
    {
        if (auto *s = std::getenv("SOUNDPLUS_FIXTURE_RELEASE_SLEEP_MS"))
            juce::Thread::sleep(juce::jlimit(0, 1000, juce::String(s).getIntValue()));
    }
    void reset() override
    {
        if (processingDepth.load() != 0)
            if (auto *dir = std::getenv("SOUNDPLUS_FIXTURE_LIVE_QA"))
                juce::File(dir)
                    .getChildFile(juce::String((int)::getpid()) + ".overlap")
                    .replaceWithText("reset overlapped DSP");
        if (std::getenv("SOUNDPLUS_FIXTURE_SLOW_FAULT"))
            if (auto *dir = std::getenv("SOUNDPLUS_FIXTURE_LIVE_QA"))
                juce::File(dir)
                    .getChildFile(juce::String((int)::getpid()) + ".operations")
                    .appendText("reset\n");
        for (auto &c : delay)
            std::fill(c.begin(), c.end(), 0.f);
        position = 0;
        if (auto *dir = std::getenv("SOUNDPLUS_FIXTURE_LIVE_QA"))
            juce::File(dir).getChildFile(juce::String((int)::getpid()) + ".resets").appendText("reset\n");
    }
    void processBlock(juce::AudioBuffer<float> &b, juce::MidiBuffer &midi) override
    {
        struct ProcessingScope
        {
            std::atomic<int> &depth;
            explicit ProcessingScope(std::atomic<int> &d) : depth(d)
            {
                ++depth;
            }
            ~ProcessingScope()
            {
                --depth;
            }
        } inFlight(processingDepth);
        if (b.getNumSamples() > 0 && std::getenv("SOUNDPLUS_FIXTURE_SLOW_FAULT"))
            if (auto *dir = std::getenv("SOUNDPLUS_FIXTURE_LIVE_QA"))
            {
                auto stem = juce::File(dir).getChildFile(juce::String((int)::getpid()));
                auto fault = stem.withFileExtension("slow");
                if (fault.existsAsFile())
                {
                    int ms = juce::jlimit(300, 1500, fault.loadFileAsString().getIntValue());
                    fault.deleteFile();
                    stem.withFileExtension("operations").appendText("begin\n");
                    juce::Thread::sleep(ms);
                    stem.withFileExtension("operations").appendText("end\n");
                }
            }
        if (topology() == "midi")
        {
            if (!midi.isEmpty())
                std::abort();
            midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), 0);
        }
        if (sleepMs)
            juce::Thread::sleep(sleepMs);
        if (mode->getIndex() == 3)
            std::abort();
        if (mode->getIndex() == 4)
            for (;;)
                juce::Thread::sleep(100);
        if (mode->getIndex() == 5)
            setLatencySamples(getLatencySamples() + 1);
        auto g = gain->get() * opaqueGain.load() * (invert->get() ? -1.f : 1.f) *
                 (mode->getIndex() == 1 ? randomGain : 1.f);
        for (int s = 0; s < b.getNumSamples(); ++s)
        {
            for (int c = 0; c < b.getNumChannels(); ++c)
            {
                auto x = b.getSample(c, s);
                delay[(size_t)c][(size_t)position] = x;
                auto y = delay[(size_t)c][(size_t)((position + 2048 - delayFrames) % 2048)] * g + bias->get();
                b.setSample(c, s, mode->getIndex() == 2 ? std::numeric_limits<float>::quiet_NaN() : y);
            }
            position = (position + 1) % 2048;
        }
    }
    struct Editor final : juce::AudioProcessorEditor, private juce::Timer
    {
        Fixture &fixture;
        int last = 0;
        bool nativeGesture = false;
        double lastUi = std::numeric_limits<double>::quiet_NaN();
        juce::Slider gainControl;
        explicit Editor(Fixture &f) : AudioProcessorEditor(f), fixture(f)
        {
            gainControl.setRange(0, 1, .001);
            gainControl.setValue(static_cast<juce::AudioProcessorParameter *>(f.gain)->getValue());
            gainControl.onDragStart = [this] { fixture.gain->beginChangeGesture(); };
            gainControl.onDragEnd = [this] { fixture.gain->endChangeGesture(); };
            gainControl.onValueChange = [this]
            { fixture.gain->setValueNotifyingHost((float)gainControl.getValue()); };
            addAndMakeVisible(gainControl);
            gainControl.setBounds(20, 20, 280, 40);
            setSize(320, 80);
            startTimer(10);
        }
        void timerCallback() override
        {
            auto *dir = std::getenv("SOUNDPLUS_FIXTURE_LIVE_QA");
            if (!dir)
                return;
            auto value = static_cast<juce::AudioProcessorParameter *>(fixture.gain)->getValue();
            if (!gainControl.isMouseButtonDown())
                gainControl.setValue(value, juce::dontSendNotification);
            if (gainControl.getValue() != lastUi)
            {
                lastUi = gainControl.getValue();
                auto record = juce::var(new juce::DynamicObject());
                auto *o = record.getDynamicObject();
                o->setProperty("value", lastUi);
                o->setProperty("showing", isShowing());
                o->setProperty("ticks", (juce::int64)juce::Time::getHighResolutionTicks());
                auto destination = juce::File(dir).getChildFile(juce::String((int)::getpid()) + ".ui.json");
                juce::TemporaryFile temporary(destination);
                temporary.getFile().replaceWithText(juce::JSON::toString(record, true));
                temporary.overwriteTargetFileWithTemporary();
            }
            auto file = juce::File(dir).getChildFile(juce::String((int)::getpid()) + ".json");
            auto v = juce::JSON::parse(file.loadFileAsString());
            if (!v.isObject() || (int)v["sequence"] <= last)
                return;
            last = (int)v["sequence"];
            if (v.hasProperty("opaque"))
            {
                fixture.opaqueGain = (float)v["opaque"];
                fixture.updateHostDisplay(ChangeDetails().withNonParameterStateChanged(true));
            }
            else if (v.hasProperty("silentOpaque"))
                fixture.opaqueGain = (float)v["silentOpaque"];
            else if (v.hasProperty("program"))
                fixture.setCurrentProgram((int)v["program"]);
            else
            {
                auto *p = fixture.getParameters()[(int)v["index"]];
                if (!nativeGesture)
                    p->beginChangeGesture();
                p->setValueNotifyingHost((float)v["value"]);
                auto stage = v["gestureStage"].toString();
                nativeGesture = stage == "begin" || stage == "continue";
                if (!nativeGesture)
                    p->endChangeGesture();
            }
        }
    };
    bool hasEditor() const override
    {
        return std::getenv("SOUNDPLUS_FIXTURE_LIVE_QA") != nullptr;
    }
    juce::AudioProcessorEditor *createEditor() override
    {
        if (!hasEditor())
            return nullptr;
        if (std::getenv("SOUNDPLUS_FIXTURE_GENERIC_EDITOR"))
            return new juce::GenericAudioProcessorEditor(*this);
        return new Editor(*this);
    }
    int getNumPrograms() override
    {
        return std::getenv("SOUNDPLUS_FIXTURE_PROGRAMS") ? 2 : 1;
    }
    int getCurrentProgram() override
    {
        return program;
    }
    void setCurrentProgram(int index) override
    {
        if (std::getenv("SOUNDPLUS_FIXTURE_PROGRAMS"))
        {
            program = index;
            gain->setValueNotifyingHost(index ? .75f : .25f);
            updateHostDisplay(ChangeDetails().withProgramChanged(true));
        }
    }
    const juce::String getProgramName(int index) override
    {
        return index ? "Loud" : "Default";
    }
    void changeProgramName(int, const juce::String &) override {}
    void getStateInformation(juce::MemoryBlock &b) override
    {
        b.setSize((size_t)(getParameters().size() + 1) * sizeof(float));
        auto *p = static_cast<float *>(b.getData());
        for (auto *v : getParameters())
            *p++ = v->getValue();
        *p = opaqueGain.load();
    }
    void setStateInformation(const void *data, int bytes) override
    {
        if (bytes == getParameters().size() * (int)sizeof(float) ||
            bytes == (getParameters().size() + 1) * (int)sizeof(float))
        {
            auto *p = static_cast<const float *>(data);
            for (auto *v : getParameters())
                v->setValueNotifyingHost(*p++);
            opaqueGain = bytes > (getParameters().size() * (int)sizeof(float)) ? *p : 1.f;
        }
    }
};
juce::AudioProcessor *JUCE_CALLTYPE createPluginFilter()
{
    return new Fixture();
}
