#include "AudioEngine.h"
#include "MacSources.h"

int main()
{
    juce::ScopedJuceInitialiser_GUI initialize;
    sp::makeAudioProcessBackground();
    sp::AudioEngine engine;
    juce::MessageManager::getInstance()->runDispatchLoop();
    return 0;
}
