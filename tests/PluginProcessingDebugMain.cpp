#include <juce_core/juce_core.h>
#include <iostream>
void runPluginProcessingTests(const juce::File &, const juce::File &, const juce::File &, const juce::File &);
int main(int argc, char **argv)
{
    try
    {
        if (argc != 5)
        {
            std::cerr
                << "Usage: plugin-QA output-directory app-executable fixture.vst3 stereo-fixture.vst3\n";
            return 2;
        }
        runPluginProcessingTests(juce::File(argv[1]), juce::File(argv[2]), juce::File(argv[3]),
                                 juce::File(argv[4]));
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
