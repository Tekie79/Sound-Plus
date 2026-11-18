#include "ViewCommandTests.h"
#include "DesignerView.h"
#include <iostream>
namespace
{
int checks = 0;
void check(bool ok, const char *why)
{
    ++checks;
    if (!ok)
        throw std::runtime_error(why);
}
void tone(const juce::File &file, double hz)
{
    juce::WavAudioFormat wav;
    auto stream = file.createOutputStream();
    std::unique_ptr<juce::AudioFormatWriter> writer(
        wav.createWriterFor(stream.release(), 48000, 2, 24, {}, 0));
    juce::AudioBuffer<float> pcm(2, 96000);
    for (int ch = 0; ch < 2; ++ch)
        for (int n = 0; n < 96000; ++n)
        {
            auto ramp = std::min({1., n / 480., (95999 - n) / 480.});
            pcm.setSample(
                ch, n,
                (float)(.2 * ramp *
                        std::sin(juce::MathConstants<double>::twoPi * (hz + ch * 220) * n / 48000.)));
        }
    check(writer && writer->writeFromAudioSampleBuffer(pcm, 0, 96000),
          "Generated stereo QA source finalized");
}
} // namespace
namespace sp
{
struct DesignerViewTestAccess
{
    static void run(DesignerView &view, DesignDocument d, const juce::File &saved)
    {
        view.session.protectStartupEdits();
        view.session.document = d;
        view.session.selectedIds = {d.selectedLayer};
        view.refresh();
        check(!view.snapOptions.isToggleable() && !view.compare.isToggleable(),
              "Popup and comparison are momentary actions");
        for (auto *b : {&view.snap, &view.scrub, &view.inspect, &view.keep})
            check(b->isToggleable() && !b->getClickingTogglesState(),
                  "Model-owned toggles are explicit and never auto-toggle");
        auto &group = view.groups.front();
        auto expanded = group.open;
        group.header->onClick();
        check(group.header->isToggleable() && group.open != expanded &&
                  group.header->getToggleState() == group.open,
              "Disclosure reports its real expanded state");
        group.header->onClick();
        auto select = [&](int id) { view.candidates.setSelectedId(id, juce::sendNotificationSync); };
        select(2);
        auto first = d.variations[0].id;
        check(view.session.document.activeVariation == first &&
                  view.compare.getTitle() == "Switch to Original",
              "Selector sets actual mix and next comparison action");
        view.compare.onClick();
        check(view.session.document.activeVariation.isEmpty() && view.candidates.getSelectedId() == 1 &&
                  view.compare.getTitle() == "Switch back to Candidate A",
              "Compare switches to Original and names the return candidate");
        view.compare.onClick();
        check(view.session.document.activeVariation == first && view.candidates.getSelectedId() == 2,
              "Compare switches back and synchronizes selector");
        select(3);
        view.compare.onClick();
        check(view.compare.getTooltip() == "Switch back to Candidate B",
              "Choosing a different candidate replaces the return target");
        select(3);
        view.discard.onClick();
        check(view.session.document.activeVariation.isEmpty() && view.candidates.getSelectedId() == 1 &&
                  !view.compare.isEnabled(),
              "Removing the comparison candidate invalidates return state");
        select(2);
        view.compare.onClick();
        auto other = d;
        other.id = juce::Uuid().toString();
        other.activeVariation.clear();
        view.session.document = other;
        view.refresh();
        check(!view.compare.isEnabled() && view.candidates.getSelectedId() == 1,
              "Installing another document clears comparison state even with reused candidate IDs");
        select(2);
        view.compare.onClick();
        view.session.edit("Remove inactive candidate",
                          [](auto &doc) { doc.variations.erase(doc.variations.begin()); });
        check(!view.compare.isEnabled(),
              "Removing the cached candidate while Original is active invalidates comparison");
        view.session.document = d;
        view.session.selectedIds = {d.selectedLayer};
        view.refresh();
        view.inspectorOpen = true;
        for (int width : {688, 774})
        {
            view.setSize(width, 648);
            auto inside = view.getLocalBounds();
            for (auto *b : std::initializer_list<IconButton *>{&view.newButton,
                                                               &view.openButton,
                                                               &view.saveButton,
                                                               &view.recover,
                                                               &view.undoButton,
                                                               &view.redoButton,
                                                               &view.add,
                                                               &view.duplicate,
                                                               &view.remove,
                                                               &view.up,
                                                               &view.down,
                                                               &view.inspect,
                                                               &view.zoomIn,
                                                               &view.zoomOut,
                                                               &view.verticalIn,
                                                               &view.verticalOut,
                                                               &view.fitAll,
                                                               &view.fitSelected,
                                                               &view.snap,
                                                               &view.snapOptions,
                                                               &view.scrub,
                                                               &view.generate,
                                                               &view.variationSettings,
                                                               &view.compare,
                                                               &view.keep,
                                                               &view.rename,
                                                               &view.discard,
                                                               &view.dragOriginal,
                                                               &view.dragOutput})
                check(inside.contains(b->getBounds()),
                      "Minimum-size toolbar/output targets remain inside the Designer");
            check(view.canvasViewport.getRight() < view.inspectorViewport.getX(),
                  "Minimum layout keeps Browser-side canvas separate from Inspector");
        }
        select(2);
        view.compare.onClick();
        view.session.open(saved);
        check(view.session.transitionPending() && view.lastComparison.isEmpty() && !view.compare.isEnabled(),
              "Same-ID document reopening clears return state during the transition");
    }
};
} // namespace sp
void runViewCommandTests(const juce::File &parent, const juce::File &application)
{
    juce::ScopedJuceInitialiser_GUI gui;
    auto directory = parent.getChildFile(juce::Uuid().toString());
    directory.createDirectory();
    auto sources = directory.getChildFile("Sources");
    sources.createDirectory();
    auto first = sources.getChildFile("QA-original-48k-stereo.wav"),
         second = sources.getChildFile("QA-layer-48k-stereo.wav");
    tone(first, 330);
    tone(second, 880);
    sp::DesignDocument doc;
    doc.name = "SoundPlus Resolve Delivery QA";
    doc.outputFolder = directory.getChildFile("Renders").getFullPathName();
    auto a = sp::makeDesignLayer(sp::inspectDesignSource(first), 0, 96000, true),
         b = sp::makeDesignLayer(sp::inspectDesignSource(second), 0, 96000, true);
    a.gainDb = -6;
    b.gainDb = -9;
    b.placement = 24000;
    a.fadeIn = a.fadeOut = b.fadeIn = b.fadeOut = 480;
    doc.original.layers = {a, b};
    doc.selectedLayer = b.id;
    doc.renderSettings.tailSeconds = .5;
    doc.renderSettings.bits = 24;
    sp::DesignVariation va, vb;
    va.name = "Candidate A";
    vb.name = "Candidate B";
    va.mix = vb.mix = doc.original;
    va.mix.layers[0].gainDb = -12;
    vb.mix.layers[1].gainDb = -15;
    doc.variations = {va, vb};
    auto saved = directory.getChildFile("delivery-qa.soundplus");
    sp::saveDesignAtomic(saved, doc);
    auto alternate = doc;
    alternate.id = juce::Uuid().toString();
    alternate.name = "SoundPlus Alternate Comparison QA";
    auto alternateFile = directory.getChildFile("comparison-other.soundplus");
    sp::saveDesignAtomic(alternateFile, alternate);
    {
        sp::AudioClient audio(application);
        juce::PropertiesFile prefs(directory.getChildFile("interface.settings"), sp::ui::preferenceOptions());
        sp::DesignerView view(directory.getChildFile("library.sqlite"), audio, prefs);
        sp::DesignerViewTestAccess::run(view, doc, saved);
    }
    auto info = juce::var(new juce::DynamicObject());
    info.getDynamicObject()->setProperty("design", saved.getFullPathName());
    info.getDynamicObject()->setProperty("alternateDesign", alternateFile.getFullPathName());
    info.getDynamicObject()->setProperty("sources", sources.getFullPathName());
    info.getDynamicObject()->setProperty("checks", checks);
    parent.getChildFile("latest.json").replaceWithText(juce::JSON::toString(info));
    std::cout << "PASS actual view comparison, model toggles and minimum geometry: " << checks << " checks\n";
}
