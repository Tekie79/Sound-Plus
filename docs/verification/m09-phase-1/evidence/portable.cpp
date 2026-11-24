#include "AdvancedEffects.h"

#include "AdvancedIR.h"
#include "DesignAudio.h"
#include "NativeEffects.h"
#include "ProjectPackage.h"
#include "TakeCapture.h"
#include "TrackModel.h"
#include <iostream>

using namespace sp;
using juce::File;

static void verify(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

static DesignDocument makeDocument(const File& source, const ChannelLayout& layout)
{
    DesignDocument document;
    document.original.masterDb = 0;
    document.original.layout = layout;
    document.renderSettings.layout = "same";
    document.renderSettings.tailSeconds = 0;
    auto track = addTrack(document.original, TrackKind::audio);
    auto group = addTrack(document.original, TrackKind::groupBus);
    routeTrack(document.original, track, group);
    auto layer = makeDesignLayer(inspectDesignSource(source), 0, -1, true);
    initialiseRouting(layer, layout);
    layer.fadeIn = layer.fadeOut = 0;
    document.original.layers.push_back(layer);
    attachClip(document, document.original.layers.back(), track);
    return document;
}

static void printMix(const DesignDocument& doc, const File& folder)
{
    auto out = renderDesign(doc, folder, doc.renderSettings, [] {return false;}, [](double){});
    std::cout << folder.getFileName() << " " << out.samples << " frames " << out.sha256 << std::endl;
}

int main(int argc, char** argv)
{
    try
    {
        verify(argc == 2, "Supply an isolated fixture root");
        juce::ScopedJuceInitialiser_GUI init;
        File root(argv[1]);
        auto sources = root.getChildFile("sources");
        for (auto id : {"5.1-side-v1", "5.1-rear-v1"})
        {
            auto surround = makeDocument(sources.getChildFile(juce::String(id)+".wav"), channelLayout(id));
            printMix(surround,root.getChildFile(id));
        }
        auto doc = makeDocument(sources.getChildFile("stereo.wav"), channelLayout("stereo-v1"));
        auto& track = doc.original.tracks.front();
        printMix(doc,root.getChildFile("unity"));
        track.volumeDb = -6;
        printMix(doc,root.getChildFile("attenuated"));
        track.volumeDb = 0;
        auto fx = makeStockEffect("convolution-reverb");
        auto ir = sources.getChildFile("response.wav");
        fx.impulse = inspectImpulse(ir,doc.original.layout,"mono-diagonal");
        nativeValue(fx,"mix",1);
        doc.original.inserts.stock = {fx};
        auto take = freezeTake(doc,"Convolution echo");
        auto capture = std::make_unique<TakeCapture>(take);
        capture->phase = TakeCapture::recording;
        capture->finish(8192,true,8192);
        doc.takes = {capture->snapshot()};
        doc.activeTake = take.id;
        printMix(doc,root.getChildFile("before"));
        auto plan = planPackage(doc);
        std::set<juce::String> approved;
        for (const auto& file : plan.files) approved.insert(file.original.getFullPathName());
        verify(plan.files.size()==2,"Expected source and IR");
        auto pack = root.getChildFile("collected");
        collectPackage(plan,approved,pack);
        auto offline = root.getChildFile("offline-fixtures");
        verify(sources.moveFileTo(offline),"Disconnect QA originals");
        auto reopened = openPackage(pack);
        verifyTakeSources(reopened.takes.front());
        verify(reopened.takes.front().id==take.id && reopened.takes.front().eventHash==doc.takes.front().eventHash
               && reopened.takes.front().collectedFromRecipe==doc.takes.front().recipeHash,"Take lineage");
        printMix(reopened,root.getChildFile("after"));
        std::cout << "take " << take.id << " events " << doc.takes.front().eventHash << " original recipe "
                  << doc.takes.front().recipeHash << std::endl;
        verify(offline.moveFileTo(sources),"Reconnect QA fixtures for changed IR check");
        auto copy = sources.getChildFile("ir-backup.wav");
        verify(ir.moveFileTo(copy) && sources.getChildFile("stereo.wav").copyFileTo(ir),"Replace QA IR");
        juce::String failure;
        try { collectPackage(plan,approved,root.getChildFile("should-fail")); }
        catch (const std::exception& e) { failure=e.what(); }
        verify(failure.isNotEmpty() && !root.getChildFile("should-fail").exists(),"Changed IR published");
        std::cout << "Changed IR refused: " << failure << std::endl;
        return 0;
    }
    catch (const std::exception& e) {std::cerr << e.what() << std::endl;return 1;}
}
