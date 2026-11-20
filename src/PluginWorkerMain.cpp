#include "PluginProcessing.h"
#include "RegisteredAudioUnits.h"
#include <thread>
#include <future>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_cryptography/juce_cryptography.h>
#include <CoreFoundation/CoreFoundation.h>
#include <cmath>
namespace
{
using namespace juce;
std::atomic<bool> editorCloseRequested{false};
void check(bool b, const String &why)
{
    if (!b)
        throw std::runtime_error(why.toStdString());
}
var obj()
{
    return var(new DynamicObject());
}
void put(var &o, const char *k, const var &v)
{
    o.getDynamicObject()->setProperty(k, v);
}
String hash(const MemoryBlock &b)
{
    return SHA256(b.getData(), b.getSize()).toHexString();
}
// Inspect the bundle executable, not JUCE's moduleinfo cache or this worker's CPU.
String binaryArchitectures(const File &file)
{
    auto path = file.getFullPathName();
    auto url = CFURLCreateFromFileSystemRepresentation(
        nullptr, reinterpret_cast<const UInt8 *>(path.toRawUTF8()), path.getNumBytesAsUTF8(), true);
    if (!url)
        return "unknown";
    auto bundle = CFBundleCreate(nullptr, url);
    CFRelease(url);
    if (!bundle)
        return "unknown";
    auto executable = CFBundleCopyExecutableURL(bundle);
    CFRelease(bundle);
    if (!executable)
        return "unknown";
    UInt8 name[4096]{};
    bool found = CFURLGetFileSystemRepresentation(executable, true, name, sizeof(name));
    CFRelease(executable);
    if (!found)
        return "unknown";
    FileInputStream input(File(String::fromUTF8(reinterpret_cast<const char *>(name))));
    uint8_t bytes[4096]{};
    int count = input.read(bytes, sizeof(bytes));
    if (count < 8)
        return "unknown";
    auto word = [&](int at, bool big) -> uint32_t
    {
        if (at < 0 || at + 4 > count)
            return 0;
        uint32_t value = 0;
        for (int n = 0; n < 4; ++n)
            value |= uint32_t(bytes[at + n]) << (8 * (big ? 3 - n : n));
        return value;
    };
    StringArray architectures;
    auto add = [&](uint32_t cpu)
    {
        architectures.addIfNotAlreadyThere(cpu == 0x0100000c   ? "arm64"
                                           : cpu == 0x01000007 ? "x86_64"
                                           : cpu == 12         ? "arm"
                                           : cpu == 7          ? "x86"
                                                               : "cpu-" + String::toHexString((int)cpu));
    };
    auto magic = word(0, true);
    if (magic == 0xfeedface || magic == 0xfeedfacf || magic == 0xcefaedfe || magic == 0xcffaedfe)
        add(word(4, magic == 0xfeedface || magic == 0xfeedfacf));
    else if (magic == 0xcafebabe || magic == 0xcafebabf || magic == 0xbebafeca || magic == 0xbfbafeca)
    {
        bool big = magic == 0xcafebabe || magic == 0xcafebabf;
        int stride = magic == 0xcafebabf || magic == 0xbfbafeca ? 32 : 20;
        auto entries = word(4, big);
        if (entries > 64 || 8 + uint64_t(entries) * stride > uint64_t(count))
            return "unknown";
        for (uint32_t n = 0; n < entries; ++n)
            add(word(8 + (int)n * stride, big));
    }
    architectures.sort(true);
    return architectures.isEmpty() ? "unknown" : architectures.joinIntoString("+");
}
template <typename F> void onPluginMessageThread(F fn)
{
    if (MessageManager::getInstance()->isThisTheMessageThread())
    {
        fn();
        return;
    }
    std::promise<void> done;
    auto future = done.get_future();
    MessageManager::callAsync(
        [&done, fn = std::move(fn)]() mutable
        {
            try
            {
                fn();
                done.set_value();
            }
            catch (...)
            {
                done.set_exception(std::current_exception());
            }
        });
    future.get();
}
struct PluginDeleter
{
    void operator()(AudioPluginInstance *p) const
    {
        if (!p)
            return;
        if (MessageManager::getInstance()->isThisTheMessageThread())
            delete p;
        else
            onPluginMessageThread([p] { delete p; });
    }
};
using HostedInstance = std::unique_ptr<AudioPluginInstance, PluginDeleter>;
HostedInstance instance(AudioPluginFormatManager &formats, const var &identity, double rate, int block)
{
    PluginDescription d;
    auto xml = parseXML(identity["xml"].toString());
    check(xml != nullptr && d.loadFromXml(*xml), "Invalid plugin identity");
    check(d.name == identity["name"].toString() && d.manufacturerName == identity["vendor"].toString() &&
              d.version == identity["version"].toString() &&
              d.createIdentifierString() == identity["id"].toString() &&
              d.fileOrIdentifier == identity["path"].toString(),
          "Plugin identity fields disagree with its class descriptor");
    check(!d.isInstrument && (d.pluginFormatName == "VST3" || d.pluginFormatName == "AudioUnit"),
          "Only VST3 / Audio Unit audio effects are supported");
    check(d.pluginFormatName == identity["format"].toString(), "Plugin format identity mismatch");
    check(sp::pluginFingerprint(
              File(d.pluginFormatName == "AudioUnit" ? identity["bundle"].toString() : d.fileOrIdentifier)) ==
              identity["fingerprint"].toString(),
          "Plugin is missing/changed; rescan and explicitly choose it for a new run");
    String error;
    std::promise<std::pair<std::unique_ptr<AudioPluginInstance>, String>> created;
    auto future = created.get_future();
    formats.createPluginInstanceAsync(d, rate, block, [&created](auto p, const String &why)
                                      { created.set_value({std::move(p), why}); });
    auto outcome = future.get();
    HostedInstance p(outcome.first.release());
    error = outcome.second;
    check(p != nullptr, "Plugin unavailable/native architecture unsupported: " + error);
    check(!p->acceptsMidi() && !p->producesMidi() && !p->isMidiEffect(),
          "MIDI/instruments are not supported");
    check(p->getBusCount(true) == 1 && p->getBusCount(false) == 1,
          "One main input/output bus required; sidechains unsupported");
    return p;
}
void layout(AudioPluginInstance &p, const sp::ChannelLayout &l)
{
    sp::validateLayout(l);
    check(!l.id.startsWith("unknown-"), "Explicit channel interpretation required");
    AudioProcessor::BusesLayout buses;
    buses.inputBuses.add(sp::juceChannelSet(l));
    buses.outputBuses.add(sp::juceChannelSet(l));
    check(p.checkBusesLayoutSupported(buses) && p.setBusesLayout(buses) && p.getBusesLayout() == buses,
          "Incompatible layout: effect does not accept exact " + sp::layoutName(l) +
              " input/output roles; convert explicitly before processing");
}
var describe(AudioPluginInstance &p)
{
    auto result = obj();
    Array<var> params;
    check(p.getParameters().size() <= 512, "More than 512 parameters is unsupported");
    for (int n = 0; n < p.getParameters().size(); ++n)
    {
        auto *v = p.getParameters()[n];
        auto item = obj();
        put(item, "index", n);
        put(item, "name", v->getName(128));
        put(item, "id", p.getParameterID(n));
        put(item, "value", v->getValue());
        put(item, "default", v->getDefaultValue());
        put(item, "text", v->getCurrentValueAsText());
        put(item, "steps", v->getNumSteps());
        params.add(item);
    }
    MemoryBlock state;
    p.getStateInformation(state);
    check(state.getSize() <= 4 * 1024 * 1024, "Plugin state exceeds 4 MiB");
    put(result, "parameters", params);
    put(result, "program", p.getCurrentProgram());
    put(result, "programName", p.getProgramName(p.getCurrentProgram()));
    put(result, "state", state.toBase64Encoding());
    put(result, "stateHash", hash(state));
    put(result, "latency", p.getLatencySamples());
    auto tail = p.getTailLengthSeconds();
    put(result, "reportedTail", std::isfinite(tail) ? tail : -1.);
    return result;
}
void restore(AudioPluginInstance &p, const var &job)
{
    if (job.hasProperty("program") && p.getNumPrograms() > 0)
    {
        int program = (int)job["program"];
        check(isPositiveAndBelow(program, p.getNumPrograms()), "Saved program is unavailable");
        p.setCurrentProgram(program);
    }
    if (job["state"].toString().isNotEmpty())
    {
        MemoryBlock state;
        check(state.fromBase64Encoding(job["state"].toString()) && state.getSize() <= 4 * 1024 * 1024,
              "Invalid state bytes");
        check(hash(state) == job["stateHash"].toString(), "Plugin state checksum mismatch");
        p.setStateInformation(state.getData(), (int)state.getSize());
    }
    if (auto *params = job["parameters"].getArray())
    {
        check(params->size() <= 512, "Parameter bound exceeded");
        for (auto &v : *params)
        {
            int n = (int)v["index"];
            double x = (double)v["value"];
            check(isPositiveAndBelow(n, p.getParameters().size()) && std::isfinite(x) && x >= 0 && x <= 1,
                  "Invalid normalized parameter");
            if (v["id"].isString())
                check(v["id"].toString() == p.getParameterID(n),
                      "Saved parameter ID no longer matches its processor");
            p.getParameters()[n]->setValueNotifyingHost((float)x);
        }
    }
}
} // namespace
#include "HostedRackService.h"
namespace
{
var execute(const var &job)
{
    AudioPluginFormatManager formats;
    formats.addFormat(new VST3PluginFormat());
    formats.addFormat(new AudioUnitPluginFormat());
    auto mode = job["mode"].toString();
    if (mode == "rack")
        return serveRack(job, formats);
    if (mode == "enumerate-au")
        return sp::registeredAudioUnits();
    if (mode == "scan")
    {
        auto path = job["path"].toString();
        OwnedArray<PluginDescription> types;
        bool au = job["format"].toString() == "AudioUnit";
        formats.getFormat(au ? 1 : 0)->findAllTypesForFile(types, path);
        check(!types.isEmpty(),
              "No native arm64 VST3 effect found (corrupt, incompatible architecture or unsupported bundle)");
        Array<var> plugins;
        auto bundle = au ? File(job["bundle"].toString()) : File(path);
        auto fingerprint = sp::pluginFingerprint(bundle);
        if (au)
            check(fingerprint == job["fingerprint"].toString(), "Audio Unit bundle changed during discovery");
        auto architectures = binaryArchitectures(bundle);
        for (auto *d : types)
        {
            auto p = obj();
            put(p, "name", d->name);
            put(p, "vendor", d->manufacturerName);
            put(p, "version", d->version);
            put(p, "format", au ? "AudioUnit" : "VST3");
            if (au)
            {
                put(p, "bundle", bundle.getFullPathName());
                put(p, "auVersion", bundle.hasFileExtension("appex") ? 3 : 2);
            }
            put(p, "category", d->category);
            put(p, "architecture", architectures);
            put(p, "path", d->fileOrIdentifier);
            put(p, "fingerprint", fingerprint);
            put(p, "instrument", d->isInstrument);
            put(p, "xml", d->createXml()->toString());
            put(p, "id", d->createIdentifierString());
            String reason;
            Array<var> supported;
            try
            {
                check(StringArray::fromTokens(architectures, "+", "").contains("arm64"),
                      "Native arm64 required; bundle architecture: " + architectures + "; no bridging");
                auto effect = instance(formats, p, 48000, 512);
                for (auto l : sp::namedLayouts())
                {
                    try
                    {
                        layout(*effect, l);
                        supported.add(l.id);
                    }
                    catch (...)
                    {
                    }
                }
                for (int n : {4, 8, 16})
                {
                    auto l = sp::discreteLayout(n);
                    try
                    {
                        layout(*effect, l);
                        supported.add(l.id);
                    }
                    catch (...)
                    {
                    }
                }
            }
            catch (const std::exception &e)
            {
                reason = e.what();
            }
            put(p, "layouts", supported);
            put(p, "reason", reason);
            plugins.add(p);
        }
        auto result = obj();
        put(result, "plugins", plugins);
        return result;
    }
    double rate = (double)job["rate"];
    int block = (int)job["block"];
    check(rate >= 8000 && rate <= 192000 && block >= 32 && block <= 4096,
          "Unsupported sample rate/block bound");
    auto identity = job["plugin"];
    auto effect = instance(formats, identity, rate, block);
    auto bus = sp::layoutFromJson(job["layout"]);
    var state;
    onPluginMessageThread(
        [&]
        {
            restore(*effect, job);
            layout(*effect, bus);
            effect->setNonRealtime(true);
            effect->setRateAndBufferSizeDetails(rate, block);
            effect->prepareToPlay(rate, block);
            effect->reset();
            state = describe(*effect);
        });
    if (mode == "editor")
    {
        struct EditorWindow final : DocumentWindow
        {
            std::atomic<bool> &closed;
            EditorWindow(const String &name, std::atomic<bool> &value)
                : DocumentWindow(name, Colour(0xff171d25), DocumentWindow::closeButton), closed(value)
            {
            }
            void closeButtonPressed() override
            {
                setVisible(false);
                closed = true;
            }
        };
        std::atomic<bool> closed{false};
        std::unique_ptr<EditorWindow> window;
        onPluginMessageThread(
            [&]
            {
                auto *editor = effect->hasEditor() ? effect->createEditorIfNeeded() : nullptr;
                if (!editor)
                    editor = new GenericAudioProcessorEditor(*effect);
                window = std::make_unique<EditorWindow>(
                    "SoundPlus / " + effect->getName() + " / apply on close", closed);
                window->setUsingNativeTitleBar(true);
                window->setContentOwned(editor, true);
                window->setResizable(editor->isResizable(), false);
                window->centreWithSize(editor->getWidth(), editor->getHeight());
                Process::makeForegroundProcess();
                window->setVisible(true);
                window->toFront(true);
            });
        while (!closed && !editorCloseRequested.load())
            Thread::sleep(10);
        onPluginMessageThread(
            [&]
            {
                window.reset();
                state = describe(*effect);
                onPluginMessageThread([&] { effect->releaseResources(); });
            });
        return state;
    }
    if (mode == "describe")
    {
        onPluginMessageThread([&] { effect->releaseResources(); });
        return state;
    }
    check(mode == "process", "Unknown worker operation");
    auto input = job["input"];
    auto path = File(input["path"].toString());
    check(sp::processingHash(path) == input["sha256"].toString(), "Staged input checksum mismatch");
    AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<AudioFormatReader> r(fm.createReaderFor(path));
    check(r != nullptr && (int)r->numChannels == bus.size() && r->sampleRate == rate,
          "Input rate/layout mismatch");
    check(sp::layoutFromJson(input["layout"]) == bus, "Input ordered layout differs");
    check(r->lengthInSamples == (int64_t)input["frames"] && r->lengthInSamples > 0 &&
              r->lengthInSamples / rate <= 600,
          "Invalid input frame count");
    int latency = effect->getLatencySamples();
    check(latency >= 0 && latency <= rate * 10, "Reported latency exceeds ten-second bound");
    double tail = (double)job["tailSeconds"];
    check(std::isfinite(tail) && tail >= 0 && tail <= 30, "Tail must be finite, 0–30 seconds");
    int64_t frames = r->lengthInSamples + (int64_t)std::llround(tail * rate), total = frames + latency;
    bool bypass = (bool)job["bypass"];
    if (bypass)
    {
        latency = 0;
        total = frames;
    }
    auto output = File(job["output"].toString());
    check(output != path && output != File(input["selection"]["source"]["path"].toString()),
          "Output must not overwrite the original/staged input");
    output.getParentDirectory().createDirectory();
    TemporaryFile staged(output);
    WavAudioFormat wav;
    StringPairArray meta;
    meta.set(WavAudioFormat::bwavDescription,
             "SoundPlus layout:" + bus.id + "; order:" + bus.roles.joinIntoString(","));
    std::unique_ptr<AudioFormatWriter> w(wav.createWriterFor(staged.getFile().createOutputStream().release(),
                                                             rate, sp::juceChannelSet(bus), 32, meta, 0));
    check(w != nullptr, "Cannot stage processed PCM");
    AudioBuffer<float> buffer(bus.size(), block);
    MidiBuffer midi;
    double peak = 0;
    int64_t clipped = 0, written = 0;
    auto initialBuses = effect->getBusesLayout();
    for (int64_t at = 0; at < total; at += block)
    {
        int count = (int)std::min<int64_t>(block, total - at);
        buffer.clear();
        if (at < r->lengthInSamples)
            check(r->read(&buffer, 0, (int)std::min<int64_t>(count, r->lengthInSamples - at), at, true, true),
                  "Input read failed");
        if (!bypass)
        {
            AudioBuffer<float> view(buffer.getArrayOfWritePointers(), bus.size(), count);
            onPluginMessageThread([&] { effect->processBlock(view, midi); });
            midi.clear();
        }
        check(effect->getBusesLayout() == initialBuses &&
                  effect->getLatencySamples() == (int)state["latency"],
              "Plugin changed bus/latency; reprepare with new configuration");
        int offset = (int)std::clamp<int64_t>(latency - at, 0, count), n = count - offset;
        for (int c = 0; c < bus.size(); ++c)
            for (int s = offset; s < count; ++s)
            {
                auto x = buffer.getSample(c, s);
                check(std::isfinite(x), "Plugin produced non-finite output");
                peak = std::max(peak, std::abs((double)x));
                if (std::abs(x) > 1)
                    ++clipped;
            }
        if (n)
        {
            check(w->writeFromAudioSampleBuffer(buffer, offset, n), "Processed disk write failed");
            written += n;
        }
    }
    w.reset();
    onPluginMessageThread([&] { effect->releaseResources(); });
    check(written == frames, "Latency compensation frame mismatch");
    check(sp::processingHash(path) == input["sha256"].toString(), "Input changed before publication");
    check(sp::pluginFingerprint(File(identity["format"].toString() == "AudioUnit"
                                         ? identity["bundle"].toString()
                                         : identity["path"].toString())) ==
              identity["fingerprint"].toString(),
          "Plugin changed before publication");
    check(staged.overwriteTargetFileWithTemporary(), "Cannot publish processed PCM");
    auto result = obj();
    put(result, "version", 1);
    put(result, "kind", "plugin-preview-v1");
    auto frozen = job.clone();
    put(frozen, "state", state["state"]);
    put(frozen, "stateHash", state["stateHash"]);
    put(frozen, "parameters", state["parameters"]);
    put(result, "job", frozen);
    put(result, "plugin", identity);
    put(result, "input", input);
    put(result, "generation", job["generation"]);
    put(result, "state", state["state"]);
    put(result, "stateHash", state["stateHash"]);
    put(result, "parameters", state["parameters"]);
    put(result, "rate", rate);
    put(result, "block", block);
    put(result, "frames", frames);
    put(result, "layout", sp::layoutJson(bus));
    put(result, "latencyDiscarded", latency);
    put(result, "reportedTail", state["reportedTail"]);
    put(result, "tailSeconds", tail);
    put(result, "history", "reset-before-frame-zero-v1");
    put(result, "peak", peak);
    put(result, "clippedSamples", clipped);
    put(result, "path", output.getFullPathName());
    put(result, "sha256", sp::processingHash(output));
    return result;
}
} // namespace
int runWorkerRequest(const juce::File &request, bool pumpMain)
{
    juce::var job;
    try
    {
        job = sp::loadProcessingJson(request);
        check((int)job["protocol"] == 1, "Unsupported worker protocol");
        juce::var reply;
        if (pumpMain)
        {
            // The console scanner/processor services the plugin message thread.
            std::atomic<bool> done{false};
            std::exception_ptr failure;
            std::thread work(
                [&]
                {
                    try
                    {
                        reply = execute(job);
                    }
                    catch (...)
                    {
                        failure = std::current_exception();
                    }
                    done = true;
                });
            while (!done.load())
                CFRunLoopRunInMode(kCFRunLoopDefaultMode, .005, false);
            work.join();
            if (failure)
                std::rethrow_exception(failure);
        }
        else
            reply = execute(job); // GUI application already owns its native run loop.
        put(reply, "protocol", 1);
        sp::saveProcessingJson(juce::File(job["reply"].toString()), reply);
        return 0;
    }
    catch (const std::exception &e)
    {
        if (job.isObject())
        {
            auto reply = obj();
            put(reply, "protocol", 1);
            put(reply, "error", juce::String(e.what()));
            try
            {
                sp::saveProcessingJson(juce::File(job["reply"].toString()), reply);
                return 0;
            }
            catch (...)
            {
            }
        }
        return 3;
    }
}

#if SOUNDPLUS_PLUGIN_EDITOR_APP
class PluginEditorApplication final : public juce::JUCEApplication
{
  public:
    const juce::String getApplicationName() override
    {
        return "SoundPlusPluginEditor";
    }
    const juce::String getApplicationVersion() override
    {
        return "1.0";
    }
    bool moreThanOneInstanceAllowed() override
    {
        return true;
    }
    void initialise(const juce::String &commandLine) override
    {
        auto args = juce::StringArray::fromTokens(commandLine, true);
        if (args.size() != 1)
        {
            setApplicationReturnValue(2);
            quit();
            return;
        }
        auto request = juce::File(args[0].unquoted());
        service = std::thread(
            [this, request]
            {
                auto code = runWorkerRequest(request, false);
                juce::MessageManager::callAsync(
                    [this, code]
                    {
                        setApplicationReturnValue(code);
                        quit();
                    });
            });
    }
    void systemRequestedQuit() override
    {
        editorCloseRequested = true;
    }
    void shutdown() override
    {
        if (service.joinable())
            service.join();
    }

  private:
    std::thread service;
};
START_JUCE_APPLICATION(PluginEditorApplication)
#else
int main(int argc, char **argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    if (argc != 2)
        return 2;
    return runWorkerRequest(juce::File(juce::String::fromUTF8(argv[1])), true);
}
#endif
