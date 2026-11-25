#include "ViewCommandTests.h"
#include "DesignerView.h"
#include "PerformanceState.h"
#include "SfxPanel.h"
#include <CoreFoundation/CoreFoundation.h>
#include <juce_cryptography/juce_cryptography.h>
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
void tone(const juce::File &file, double hz, int rate = 48000)
{
    juce::WavAudioFormat wav;
    auto stream = file.createOutputStream();
    std::unique_ptr<juce::AudioFormatWriter> writer(
        wav.createWriterFor(stream.release(), rate, 2, 24, {}, 0));
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
        auto layerId = view.session.document.selectedLayer;
        addMacroMapping(view.session.document.activeMix(), 0,
                        {{layerId, {}, "gain"}, -20, 0, "linear", false});
        view.session.document.activeMix().performance.macros[0].enabled = true;
        view.refresh();
        juce::Slider *gain = nullptr;
        for (auto *c : view.controls)
            if (auto *v = dynamic_cast<juce::Slider *>(c); v && v->getTitle() == "Layer gain (dB)")
                gain = v;
        check(gain && std::abs(gain->getValue() + 10) < 1e-8,
              "Inspector shows the effective macro-controlled value");
        auto base = view.session.selectedLayer()->gainDb;
        gain->setValue(base, juce::sendNotificationSync);
        check(!performanceOwns(view.session.document.activeMix(), {layerId, {}, "gain"}),
              "Editing back to the unchanged base value explicitly unlinks the macro target");
        view.session.undo();
        check(performanceOwns(view.session.document.activeMix(), {layerId, {}, "gain"}),
              "Undo restores inspector mapping ownership");
        view.session.document = d;
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
        view.useWorkspaceShell();
        for (bool performing : {false, true})
        {
            view.setPerformMode(performing);
            view.setSize(ui::workspace::minimumWindowWidth - ui::workspace::sidebarWidth - 6,
                         ui::workspace::minimumWindowHeight);
            auto &send = view.sendTransferButton();
            auto &drag = view.dragTransferButton();
            auto &options = view.transferSettingsButton();
            check(view.getLocalBounds().contains(send.getBounds()) && send.isVisible() &&
                      send.getY() == drag.getY() + 1 && send.getX() >= drag.getRight() &&
                      options.getX() >= send.getRight(),
                  "Direct Send stays beside drag and options at minimum size in Edit and Perform");
            for (auto *c : std::initializer_list<juce::Component *>{&view.candidates, &view.readyOutputs,
                      &view.dragOriginal, &view.dragOutput, &view.play, &view.monitorControl})
                check(!send.getBounds().intersects(c->getBounds()),
                      "Direct Send stays clear of variations, export selection and transport");
        }
        view.shell = false;
        view.setPerformMode(false);
        auto legacyDocument = view.session.document;
        view.session.edit("Materialize QA tracks",
                          [](auto &document)
                          {
                              enableTrackSchema(document.activeMix());
                              document.selectedLayer.clear();
                              document.selectedTrack = addTrack(document.activeMix(), TrackKind::audio);
                          });
        view.refresh();
        for (auto &g : view.groups)
        {
            if (g.name == "LAYER" || g.name == "SOURCE & TIMING" || g.name == "MIX" || g.name == "FADES")
                check(!g.header->isVisible(), "Empty Track hides clip-only Inspector groups");
            if (g.name == "TRACK INSERTS")
                check(g.header->isVisible(), "Empty Track exposes real insert owner immediately");
        }
        auto midiDocument = view.session.document;
        auto midiOwner = addTrack(midiDocument.activeMix(), TrackKind::midiInstrument);
        auto midiId = createMidiClip(midiDocument, midiOwner, 0);
        view.session.document = midiDocument;
        view.refresh();
        auto rows = visibleTracks(view.session.document.activeMix());
        auto at = std::find(rows.begin(), rows.end(), midiOwner);
        auto point = juce::Point<float>(view.tracks->viewport.x(24000, 48000),
                                        28.f + (float)(at - rows.begin()) * 86 + 35);
        auto event = juce::MouseEvent(
            juce::Desktop::getInstance().getMainMouseSource(), point, {}, 1, 0, 0, 0, 0, view.tracks.get(),
            view.tracks.get(), juce::Time::getCurrentTime(), point, juce::Time::getCurrentTime(), 1, false);
        view.session.document.selectedMidiClip.clear();
        view.tracks->mouseDown(event);
        view.tracks->mouseUp(event);
        check(view.session.document.selectedMidiClip == midiId,
              "Actual MIDI canvas pointer selects clip independently of Track selection");
        view.tracks->mouseDoubleClick(event);
        check(view.midiEditorOpen && view.midiEditor->isVisible(),
              "Actual MIDI canvas double-click opens visible contextual piano roll");
        auto onlyMidi = midiDocument;
        onlyMidi.activeMix().layers.clear();
        view.session.document = onlyMidi;
        view.refresh();
        check(view.render.isEnabled() && view.recordTake.isEnabled(),
              "Final view reconciliation allows MIDI-only Render and frozen Perform recording");
        view.midiEditorOpen = false;
        view.session.document = midiDocument;
        view.setPerformMode(false);
        view.setSize(688, 680);
        view.mixerButton.onClick();
        check(view.mixer->isVisible() && view.mixer->getBottom() <= view.getHeight() - 204,
              "Docked mixer fits the native Edit layout");
        std::function<juce::Slider *(juce::Component &)> findFader = [&](juce::Component &part) -> juce::Slider *
        {
            if (auto *slider = dynamic_cast<juce::Slider *>(&part);
                slider && slider->getTitle() == "Volume for Master")
                return slider;
            for (auto *child : part.getChildren())
                if (auto *found = findFader(*child))
                    return found;
            return nullptr;
        };
        auto *mixFader = findFader(*view.mixer);
        const auto oldGain = view.session.document.activeMix().masterDb;
        check(mixFader != nullptr && std::abs(mixFader->getValue() - oldGain) < 1e-8, "Mixer reads canonical Master fader");
        mixFader->setValue(-14, juce::sendNotificationSync);
        check(std::abs(view.session.document.activeMix().masterDb + 14.) < 1e-8, "Mixer edits canonical Master fader");
        view.session.undo();
        check(view.session.document.activeMix().masterDb == oldGain && std::abs(mixFader->getValue() - oldGain) < 1e-8,
              "Undo updates mixer from canonical model");
        view.session.redo();
        check(std::abs(mixFader->getValue() + 14.) < 1e-8, "Redo updates mixer fader");
        view.mixerButton.onClick();
        view.session.document = legacyDocument;
        view.refresh();
        select(2);
        view.compare.onClick();
        view.session.open(saved);
        check(view.session.transitionPending() && view.lastComparison.isEmpty() && !view.compare.isEnabled(),
              "Same-ID document reopening clears return state during the transition");
    }
};
} // namespace sp
namespace sp::sfx
{
struct PreviewTestAccess
{
    struct LocalKey final : Credentials
    {
        juce::String read() override { return "synthetic"; }
        void save(const juce::String &) override {}
        void erase() override {}
    };
    static void run(const juce::File &root, const juce::File &app, const juce::File &libraryTone,
                    const juce::File &design)
    {
        const auto catalog = root.getChildFile("preview.sqlite");
        LibraryView library(catalog, app, false);
        auto &audio = library.audioClient();
        audio.gain(.02f);
        juce::PropertiesFile preferences(root.getChildFile("preview.settings"), ui::preferenceOptions());
        DesignerView designer(catalog, audio, preferences);
        juce::MemoryBlock bytes;
        auto localResponse = root.getChildFile("provider-44k.wav");
        tone(localResponse, 220, 44100);
        check(localResponse.loadFileAsData(bytes), "Read the synthetic SFX response");
        std::atomic<int> submissions{0};
        auto provider = elevenLabs([&](const auto &, const auto &, const auto &, const auto &)
        {
            ++submissions;
            return Response{200, "audio/wav", "local-preview", bytes};
        });
        const auto cache = root.getChildFile("Generation");
        Generation generation(cache, std::move(provider), std::make_unique<LocalKey>());
        auto wait = [](auto done, const char *why, bool pump = true)
        {
            const auto deadline = juce::Time::getMillisecondCounterHiRes() + 10000;
            while (!done() && juce::Time::getMillisecondCounterHiRes() < deadline)
            {
                if (pump)
                    CFRunLoopRunInMode(kCFRunLoopDefaultMode, .01, false);
                juce::Thread::sleep(3);
            }
            check(done(), why);
        };
        auto idle = [&] { return !generation.snapshot().busy; };
        auto panel = std::make_unique<Panel>(generation, library, designer, [](int) {});
        wait(idle, "Local SFX journal opened");
        panel->timerCallback();
        check(!panel->playPreview.isEnabled(), "Empty history disables Play");
        Request request;
        request.prompt = "Native preview test";
        request.wav = request.approved = true;
        request.candidates = 2;
        check(generation.submit(request), "Seed two local candidates");
        wait(idle, "Both synthetic candidates verified");
        panel->timerCallback();
        const auto candidates = generation.snapshot().history;
        check(candidates.size() == 2 && panel->playPreview.isEnabled(), "Ready WAV enables Play");
        const auto selectedFile = cache.getChildFile(panel->selected + ".wav");
        const auto digest = juce::SHA256(selectedFile).toHexString();
        auto intent = [&] { return (uint64_t)(juce::int64)audio.status()["intentGeneration"]; };
        auto originalIntent = intent();
        panel->togglePreview();
        check(panel->preparingPreview && panel->playPreview.getTitle() == "Pause preview",
              "Pause remains accessible during local file verification");
        wait(idle, "Local verification finished behind the message-thread barrier", false);
        panel->togglePreview();
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, .04, false);
        check(intent() == originalIntent, "Pause cancels the queued verification callback");
        panel->timerCallback();

        auto playing = [&]
        {
            const auto status = audio.status();
            return panel->previewActive(status) && (bool)status["playing"];
        };
        auto filePosition = [&] { return (int64_t)audio.status()["sourceSample"]; };
        panel->togglePreview();
        wait([&] { return playing() && filePosition() > 4800; }, "Actual helper consumes initial SFX frames");
        const auto firstGeneration = intent();
        panel->togglePreview();
        const auto parked = panel->previewPosition;
        check(parked > 4800 && panel->playPreview.getTitle() == "Resume preview" && intent() > firstGeneration,
              "Pause attests its last reported frame and stops its generation");
        wait(idle, "Local worker available to resume");
        panel->timerCallback();
        panel->togglePreview();
        wait([&] { return playing() && filePosition() >= parked; }, "Resume starts a new owned playback");
        wait([&] { return (bool)audio.status()["completed"]; }, "Audition reaches real EOF");
        panel->timerCallback();
        check(panel->playPreview.getTitle() == "Play preview", "EOF restores Play");
        panel->togglePreview();
        wait([&] { return playing() && filePosition() < parked; }, "Restart consumes frames from the beginning");

        panel->history.setSelectedId(2, juce::sendNotificationSync);
        panel->timerCallback();
        check(panel->selected == candidates[1].id && !panel->previewGeneration,
              "Changing selector stops and clears the previous SFX preview");
        wait(idle, "Worker is ready after selection");
        panel->togglePreview();
        wait(idle, "Second preview verification is queued for UI", false);
        const auto libraryIntent = audio.playRegion("library-owner", libraryTone, {});
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, .04, false);
        check(intent() == libraryIntent && audio.status()["intentId"].toString() == "library-owner",
              "A deferred SFX callback preserves the newer Library owner");
        panel->timerCallback();

        panel->togglePreview();
        wait(playing, "Audition takes ownership when explicitly requested");
        const auto designerIntent = audio.playDesign("design:preview-takeover", design, 0);
        panel->timerCallback();
        check(designerIntent == intent() && !panel->previewActive(audio.status()),
              "A new Designer intent supersedes SFX");
        panel.reset();
        check(intent() == designerIntent, "Closing a superseded SFX panel preserves the Designer owner");
        audio.stop();

        panel = std::make_unique<Panel>(generation, library, designer, [](int) {});
        panel->togglePreview();
        wait(playing, "Reopened dialog starts its selected ready candidate");
        const auto closeIntent = intent();
        panel.reset();
        check(intent() > closeIntent && audio.status()["intentId"].toString().isEmpty(),
              "Closing the audition panel stops the owned transport");

        panel = std::make_unique<Panel>(generation, library, designer, [](int) {});
        const auto missing = cache.getChildFile(panel->selected + ".wav");
        const auto relocated = missing.withFileExtension("unavailable");
        check(missing.moveFileTo(relocated), "Temporarily move the synthetic fixture");
        originalIntent = intent();
        panel->togglePreview();
        wait(idle, "Missing audio is rejected by local verification");
        panel->timerCallback();
        check(intent() == originalIntent && !panel->previewGeneration,
              "Missing candidate never starts transport");
        check(relocated.moveFileTo(missing) && juce::SHA256(selectedFile).toHexString() == digest &&
                  submissions == 2 && generation.snapshot().history.front().published.isEmpty(),
              "Preview never alters a WAV, publishes it, or requests another provider submission");
    }
};
} // namespace sp::sfx
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
    sp::sfx::PreviewTestAccess::run(directory, application, first, saved);
    auto info = juce::var(new juce::DynamicObject());
    info.getDynamicObject()->setProperty("design", saved.getFullPathName());
    info.getDynamicObject()->setProperty("alternateDesign", alternateFile.getFullPathName());
    info.getDynamicObject()->setProperty("sources", sources.getFullPathName());
    info.getDynamicObject()->setProperty("checks", checks);
    parent.getChildFile("latest.json").replaceWithText(juce::JSON::toString(info));
    std::cout << "PASS actual view comparison, model toggles and minimum geometry: " << checks << " checks\n";
}
