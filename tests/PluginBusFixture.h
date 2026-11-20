#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
// Direct processor fixture: JUCE's VST3 client wrapper intentionally masks a
// refused bus deactivation by supplying silence, so it cannot model this case.
class RequiredAuxiliaryFixture final : public juce::AudioProcessor
{
  public:
    RequiredAuxiliaryFixture()
        : AudioProcessor(BusesProperties()
                             .withInput("Main", juce::AudioChannelSet::stereo(), true)
                             .withInput("Required sidechain", juce::AudioChannelSet::stereo(), true)
                             .withOutput("Main", juce::AudioChannelSet::stereo(), true))
    {
    }
    bool isBusesLayoutSupported(const BusesLayout &b) const override
    {
        return b.inputBuses.size() == 2 && !b.inputBuses[1].isDisabled() &&
               b.getMainInputChannelSet() == b.getMainOutputChannelSet();
    }
    const juce::String getName() const override
    {
        return "Required auxiliary QA";
    }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float> &, juce::MidiBuffer &) override {}
    bool acceptsMidi() const override
    {
        return false;
    }
    bool producesMidi() const override
    {
        return false;
    }
    double getTailLengthSeconds() const override
    {
        return 0;
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
        return {};
    }
    void changeProgramName(int, const juce::String &) override {}
    void getStateInformation(juce::MemoryBlock &) override {}
    void setStateInformation(const void *, int) override {}
};
