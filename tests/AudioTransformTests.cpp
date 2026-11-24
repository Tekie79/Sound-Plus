#include "AudioTransform.h"
#include <iostream>
using namespace sp;
using namespace sp::sfx;
namespace
{
void check(bool ok, const char *why)
{
    if (!ok)
        throw std::runtime_error(why);
}
} // namespace
void runAudioTransformTest(const juce::File &dir)
{
    TransferRequest r;
    r.source = inspectDesignSource(dir.getChildFile("original.wav"));
    r.scope = TransferScope::sourceRange;
    r.first = 1001;
    r.last = 2001;
    auto never = [] { return false; };
    auto range = freezeTransform(r, false, dir, never);
    check(range.first == 1001 && range.last == 2001 && range.frames == 1000, "Half-open source input");
    check(TransformInput::fromJson(range.json()).sha256 == range.sha256, "Snapshot serialization");
    check(juce::File(range.path).copyFileTo(dir.getChildFile("excerpt.wav")), "Save excerpt oracle");
    r.scope = TransferScope::original;
    auto input = freezeTransform(r, false, dir, never);
    AudioEdit e{input, true, 300, 600, 8};
    check(validateEdit(e).isEmpty(), "Valid editing input");
    compositeTransform(e, dir.getChildFile("returned.wav"), dir.getChildFile("composite.wav"), never);
    bool rejected = false;
    try
    {
        compositeTransform(e, juce::File(range.path), dir.getChildFile("bad.wav"), never);
    }
    catch (const std::exception &)
    {
        rejected = true;
    }
    check(rejected && !dir.getChildFile("bad.wav").exists(), "Mismatched return rejected");
    r.source = inspectDesignSource(dir.getChildFile("surround.wav"));
    rejected = false;
    try
    {
        freezeTransform(r, false, dir, never);
    }
    catch (const std::exception &)
    {
        rejected = true;
    }
    check(rejected, "Downmix requires explicit permission");
    auto stereo = freezeTransform(r, true, dir, never);
    check(stereo.channels == 2 && stereo.downmixed && stereo.originalLayout.isNotEmpty(),
          "Downmix provenance");
    check(juce::File(stereo.path).copyFileTo(dir.getChildFile("fold.wav")), "Save downmix oracle");
    dir.getChildFile("original.wav").appendText("changed");
    rejected = false;
    try
    {
        verifyTransform(input);
    }
    catch (const std::exception &)
    {
        rejected = true;
    }
    check(rejected, "Changed original blocked");
    std::cout << "PASS immutable source selection, validated mask composite, downmix and stale input\n";
}
