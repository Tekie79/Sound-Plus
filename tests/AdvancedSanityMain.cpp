#include <juce_core/juce_core.h>
#include <iostream>
void runAdvancedEffectsTests(const juce::File &);
int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    try
    {
        runAdvancedEffectsTests(juce::File(argv[1]));
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "FAIL " << e.what() << "\n";
        return 1;
    }
}
