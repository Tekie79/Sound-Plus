#include "InputRecording.h"
#include <juce_cryptography/juce_cryptography.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cmath>
#include <set>

namespace sp
{
namespace
{
juce::var obj()
{
    return juce::var(new juce::DynamicObject);
}
void put(juce::var &v, const char *k, const juce::var &x)
{
    v.getDynamicObject()->setProperty(k, x);
}
void require(bool ok, const char *why)
{
    if (!ok)
        throw std::runtime_error(why);
}
bool operationId(const juce::String &s)
{
    return s.length() >= 32 && s.length() <= 36 && s.containsOnly("0123456789abcdefABCDEF-");
}
juce::String prefixDigest(CC_SHA256_CTX context)
{
    std::array<unsigned char, CC_SHA256_DIGEST_LENGTH> bytes{};
    CC_SHA256_Final(bytes.data(), &context);
    return juce::String::toHexString(bytes.data(), (int)bytes.size(), 0);
}
juce::String verifyPrefix(const juce::File &raw, int64_t bytes, const juce::String &expected)
{
    auto input = raw.createInputStream();
    require(input != nullptr && raw.getSize() >= bytes, "Recording prefix integrity: truncated source");
    CC_SHA256_CTX hash;
    CC_SHA256_Init(&hash);
    std::array<float, 16384> buffer{};
    for (int64_t at = 0; at < bytes;)
    {
        int count = (int)std::min<int64_t>(bytes - at, sizeof(buffer));
        require(input->read(buffer.data(), count) == count, "Recording prefix integrity: short read");
        for (int n = 0; n < count / 4; ++n)
            require(std::isfinite(buffer[(size_t)n]), "Recording prefix integrity: nonfinite PCM");
        CC_SHA256_Update(&hash, buffer.data(), (CC_LONG)count);
        at += count;
    }
    auto digest = prefixDigest(hash);
    require(expected.isEmpty() || digest == expected, "Recording prefix integrity: checksum mismatch");
    return digest;
}
void writeAll(int fd, const void *data, size_t bytes)
{
    auto *p = static_cast<const char *>(data);
    while (bytes)
    {
        auto n = ::write(fd, p, bytes);
        if (n < 0 && errno == EINTR)
            continue;
        require(n > 0, "Recording storage write failed; recover the retained prefix");
        p += n;
        bytes -= (size_t)n;
    }
}
void syncPath(const juce::File &file)
{
    int fd = ::open(file.getFullPathName().toRawUTF8(), O_RDONLY);
    bool ok = fd >= 0 && ::fsync(fd) == 0;
    if (fd >= 0)
        ::close(fd);
    require(ok, "Recording storage sync failed");
}
void atomicJson(const juce::File &path, const juce::var &value)
{
    juce::TemporaryFile temp(path);
    auto data = juce::JSON::toString(value, true);
    int fd = ::open(temp.getFile().getFullPathName().toRawUTF8(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    require(fd >= 0, "Cannot create recording journal");
    try
    {
        writeAll(fd, data.toRawUTF8(), data.getNumBytesAsUTF8());
        require(::fsync(fd) == 0, "Cannot sync recording journal");
    }
    catch (...)
    {
        ::close(fd);
        throw;
    }
    ::close(fd);
    require(temp.overwriteTargetFileWithTemporary(), "Cannot publish recording journal");
    syncPath(path.getParentDirectory());
}
juce::var readJson(const juce::File &file)
{
    require(file.existsAsFile() && file.getSize() < 4 * 1024 * 1024,
            "Recording journal missing or oversized");
    auto v = juce::JSON::parse(file);
    require(v.isObject() && ((int)v["version"] == 1 || (int)v["version"] == 2), "Invalid recording journal");
    return v;
}
InputDeviceInfo deviceFrom(const juce::var &v)
{
    InputDeviceInfo d;
    d.uid = v["uid"].toString();
    d.name = v["name"].toString();
    d.rate = (double)v["rate"];
    d.channels = (int)v["channels"];
    d.latency = (int)v["latency"];
    d.safety = (int)v["safety"];
    if (auto *a = v["names"].getArray())
        for (auto &n : *a)
            d.names.add(n.toString());
    if (v["layout"].isObject())
        d.reportedLayout = layoutFromJson(v["layout"]);
    return d;
}
const char *faultName(int n)
{
    switch (n)
    {
    case InputRecordingService::overflow:
        return "Input buffer overrun; retained prefix is incomplete";
    case InputRecordingService::discontinuity:
        return "Input device sample/timestamp discontinuity";
    case InputRecordingService::invalidTimestamp:
        return "Input device acquisition timestamps unavailable";
    case InputRecordingService::invalidPcm:
        return "Input device supplied nonfinite PCM";
    case InputRecordingService::oversizedBlock:
        return "Input callback exceeds prepared channel/block bounds";
    default:
        return "Input device disconnected or interrupted";
    }
}
} // namespace
juce::var InputDeviceInfo::json() const
{
    auto v = obj();
    put(v, "uid", uid);
    put(v, "name", name);
    put(v, "rate", rate);
    put(v, "channels", channels);
    put(v, "latency", latency);
    put(v, "safety", safety);
    juce::Array<juce::var> a;
    for (auto &n : names)
        a.add(n);
    put(v, "names", a);
    if (reportedLayout.id.isNotEmpty())
        put(v, "layout", layoutJson(reportedLayout));
    return v;
}
juce::var inputPlanJson(const InputRecordingPlan &p)
{
    auto v = obj();
    put(v, "id", p.id);
    put(v, "document", p.document);
    put(v, "variation", p.variation);
    put(v, "deviceUID", p.deviceUID);
    put(v, "root", p.root.getFullPathName());
    put(v, "projectRate", p.projectRate);
    put(v, "start", (juce::int64)p.start);
    put(v, "generation", (juce::int64)p.generation);
    juce::Array<juce::var> tracks;
    for (auto &t : p.tracks)
    {
        auto x = obj();
        put(x, "owner", t.owner);
        put(x, "name", t.name);
        put(x, "layout", layoutJson(t.layout));
        juce::Array<juce::var> c;
        for (int n : t.channels)
            c.add(n);
        put(x, "channels", c);
        tracks.add(x);
    }
    put(v, "tracks", tracks);
    return v;
}
InputRecordingPlan inputPlanFromJson(const juce::var &v)
{
    InputRecordingPlan p;
    p.id = v["id"].toString();
    p.document = v["document"].toString();
    p.variation = v["variation"].toString();
    p.deviceUID = v["deviceUID"].toString();
    require(juce::File::isAbsolutePath(v["root"].toString()), "Recording root must be absolute");
    p.root = juce::File(v["root"].toString());
    p.projectRate = (double)v["projectRate"];
    p.start = (int64_t)v["start"];
    p.generation = (uint64_t)(int64_t)v["generation"];
    auto *a = v["tracks"].getArray();
    require(a && a->size() > 0 && a->size() <= 8, "Arm one to eight Audio Tracks");
    for (auto &x : *a)
    {
        InputTrackPlan t;
        t.owner = x["owner"].toString();
        t.name = x["name"].toString();
        t.layout = layoutFromJson(x["layout"]);
        auto *c = x["channels"].getArray();
        require(c && c->size() <= 16, "Invalid physical input selection");
        for (auto &n : *c)
            t.channels.push_back((int)n);
        p.tracks.push_back(std::move(t));
    }
    return p;
}
void validateInputPlan(const InputRecordingPlan &p, const InputDeviceInfo &d)
{
    require(p.deviceUID == d.uid, "Recording input device identity changed");
    require(operationId(p.id) && p.document.isNotEmpty(), "Invalid recording operation/document identity");
    require(std::isfinite(p.projectRate) && p.projectRate >= 8000 && p.projectRate <= 384000 &&
                p.start >= 0 && p.start < p.projectRate * 3600,
            "Invalid recording project clock");
    require(std::isfinite(d.rate) && d.rate >= 8000 && d.rate <= 384000 && d.channels > 0 &&
                d.channels <= 16 && d.uid.isNotEmpty(),
            "Input device unavailable or unsupported");
    require(!p.tracks.empty() && p.tracks.size() <= 8, "Arm one to eight Audio Tracks");
    std::set<juce::String> owners;
    for (auto &t : p.tracks)
    {
        validateLayout(t.layout);
        require(t.owner.isNotEmpty() && owners.insert(t.owner).second &&
                    t.channels.size() == (size_t)t.layout.size(),
                "Invalid or duplicate recording owner");
        std::set<int> selected;
        for (int c : t.channels)
            require(c >= 0 && c < d.channels && selected.insert(c).second,
                    "Input channel unavailable or repeated");
        if (t.layout.size() > 2)
        {
            require(t.layout == d.reportedLayout,
                    "Named multichannel input requires the device's reported ordered roles");
            for (int i = 0; i < t.layout.size(); ++i)
                require(t.channels[(size_t)i] == i, "Named input role order must match the device");
        }
        else
            require(t.layout == channelLayout(t.layout.size() == 1 ? "mono-v1" : "stereo-v1"),
                    "Choose an explicit Mono or Stereo input");
    }
}
void InputTimelineClock::publish(Value v) noexcept
{
    sequence.fetch_add(1, std::memory_order_acq_rel);
    host.store(v.hostNs, std::memory_order_relaxed);
    generation.store(v.generation, std::memory_order_relaxed);
    frame.store(v.frame, std::memory_order_relaxed);
    rate.store(v.rate, std::memory_order_relaxed);
    sequence.fetch_add(1, std::memory_order_release);
}
InputTimelineClock::Value InputTimelineClock::read() const noexcept
{
    for (int n = 0; n < 3; ++n)
    {
        auto s = sequence.load(std::memory_order_acquire);
        if (s & 1)
            continue;
        Value v{host.load(std::memory_order_relaxed), generation.load(std::memory_order_relaxed),
                frame.load(std::memory_order_relaxed), rate.load(std::memory_order_relaxed)};
        if (s == sequence.load(std::memory_order_acquire))
            return v;
    }
    return {};
}
InputRecordingStore::InputRecordingStore(InputRecordingPlan p, InputDeviceInfo d)
    : plan(std::move(p)), device(std::move(d))
{
    validateInputPlan(plan, device);
    partial = plan.root.getChildFile("." + plan.id + ".partial");
    require(!partial.exists() && !plan.root.getChildFile(plan.id).exists(),
            "Recording operation already exists; originals retained");
    require(partial.createDirectory().wasOk(), "Cannot create durable recording folder");
    selected.resize((size_t)InputRecordingService::maxFrames * 16);
    peaks.resize(plan.tracks.size());
    prefixHashes.resize(plan.tracks.size());
    for (auto &hash : prefixHashes)
        CC_SHA256_Init(&hash);
    try
    {
        for (size_t i = 0; i < plan.tracks.size(); ++i)
        {
            auto f = partial.getChildFile("source-" + juce::String((int)i) + ".f32");
            int fd = ::open(f.getFullPathName().toRawUTF8(), O_WRONLY | O_CREAT | O_EXCL, 0600);
            require(fd >= 0, "Cannot create source recording");
            handles.push_back(fd);
        }
        checkpoint();
    }
    catch (...)
    {
        for (int fd : handles)
            ::close(fd);
        handles.clear();
        throw;
    }
}
InputRecordingStore::~InputRecordingStore()
{
    for (int fd : handles)
        ::close(fd);
}
juce::var InputRecordingStore::journal(const juce::String &state, const juce::String &reason) const
{
    auto v = obj();
    put(v, "version", 2);
    put(v, "prefixAlgorithm", "sha256-raw-prefix-v1");
    put(v, "state", state);
    put(v, "reason", reason);
    put(v, "plan", inputPlanJson(plan));
    put(v, "device", device.json());
    put(v, "frames", (juce::int64)written);
    put(v, "placement", (juce::int64)placement);
    put(v, "firstDeviceFrame", (juce::int64)firstDeviceFrame);
    put(v, "firstAcquisitionHostNs", (juce::int64)firstHostNs);
    put(v, "clockPolicy", "input-acquisition-host-time-v1");
    put(v, "latencyPolicy",
        "HAL input timestamps already identify acquisition time; no duplicate latency subtraction");
    juce::Array<juce::var> a;
    for (size_t i = 0; i < plan.tracks.size(); ++i)
    {
        auto x = obj();
        put(x, "raw", "source-" + juce::String((int)i) + ".f32");
        put(x, "peak", peaks[i]);
        put(x, "owner", plan.tracks[i].owner);
        put(x, "bytes", (juce::int64)(written * plan.tracks[i].channels.size() * 4));
        put(x, "sha256", prefixDigest(prefixHashes[i]));
        a.add(x);
    }
    put(v, "sources", a);
    if (!diagnostics.isVoid())
        put(v, "diagnostics", diagnostics);
    return v;
}
void InputRecordingStore::append(const float *pcm, int frames, int channels, int64_t deviceFrame,
                                 uint64_t hostNs, int64_t projectFrame)
{
    require(!closed && frames > 0 && frames <= InputRecordingService::maxFrames &&
                channels == device.channels,
            "Invalid input writer block");
    int64_t bytes = 0;
    for (auto &t : plan.tracks)
        bytes += (int64_t)frames * t.channels.size() * 4;
    require(writeBudget < 0 || bytes <= writeBudget, "Controlled recording disk-full/write failure");
    auto firstProjectFrame = written ? placement : std::max<int64_t>(0, projectFrame);
    require(firstProjectFrame + std::ceil((long double)(written + frames) * plan.projectRate / device.rate) <=
                std::llround(plan.projectRate * 3600),
            "Project recording limit reached; valid source prefix retained");
    for (auto &t : plan.tracks)
        require((written + frames) * (int64_t)t.channels.size() * 4 < 0x70000000 &&
                    written + frames < device.rate * 3600,
                "Recording safety limit reached; prefix retained");
    if (!written)
    {
        firstDeviceFrame = deviceFrame;
        firstHostNs = hostNs;
        placement = std::max<int64_t>(0, projectFrame);
    }
    auto nextHashes = prefixHashes;
    auto nextPeaks = peaks;
    for (size_t i = 0; i < plan.tracks.size(); ++i)
    {
        auto &t = plan.tracks[i];
        for (int n = 0; n < frames; ++n)
            for (size_t c = 0; c < t.channels.size(); ++c)
            {
                float s = pcm[(size_t)n * channels + t.channels[c]];
                require(std::isfinite(s), "Nonfinite recorded sample");
                selected[(size_t)n * t.channels.size() + c] = s;
                nextPeaks[i] = std::max(nextPeaks[i], std::abs(s));
            }
        writeAll(handles[i], selected.data(), (size_t)frames * t.channels.size() * 4);
        CC_SHA256_Update(&nextHashes[i], selected.data(), (CC_LONG)(frames * t.channels.size() * 4));
    }
    prefixHashes = std::move(nextHashes);
    peaks = std::move(nextPeaks);
    if (writeBudget >= 0)
        writeBudget -= bytes;
    written += frames;
}
void InputRecordingStore::checkpoint()
{
    for (int fd : handles)
        require(::fsync(fd) == 0, "Recording source sync failed");
    atomicJson(partial.getChildFile("journal.json"),
               journal("recording", "Interrupted recordings require deliberate recovery"));
}
juce::var InputRecordingStore::finish(bool complete, const juce::String &reason, bool cancelled)
{
    require(!closed, "Recording already closed");
    checkpoint();
    closed = true;
    for (int fd : handles)
        ::close(fd);
    handles.clear();
    auto j = journal(cancelled ? "cancelled" : "incomplete",
                     written == 0 && reason.isEmpty() ? "No input frames were captured" : reason);
    atomicJson(partial.getChildFile("journal.json"), j);
    if (complete && written > 0)
        return publish(partial, j, false);
    put(j, "partial", partial.getFullPathName());
    return j;
}
juce::var InputRecordingStore::publish(const juce::File &partial, juce::var j, bool recovered,
                                       bool allowUnverifiedLegacy)
{
    auto plan = inputPlanFromJson(j["plan"]);
    plan.root = partial.getParentDirectory();
    auto device = deviceFrom(j["device"]);
    validateInputPlan(plan, device);
    int64_t frames = (int64_t)j["frames"];
    require(frames > 0 && frames < device.rate * 3600, "No durable recording frames to recover");
    require(j["sources"].isArray() && j["sources"].size() == (int)plan.tracks.size(),
            "Invalid recording sources");
    auto final = plan.root.getChildFile(plan.id);
    require(!final.exists(), "Recording already published; existing audio retained");
    bool verified = (int)j["version"] == 2;
    require(verified || allowUnverifiedLegacy,
            "Legacy recording has no historical prefix checksum; choose explicit unverified recovery");
    if (verified)
        require(j["prefixAlgorithm"].toString() == "sha256-raw-prefix-v1",
                "Recording prefix integrity: unsupported algorithm");
    // Validate every owner before touching any derived WAV or receipt. Only acknowledged bytes count.
    std::vector<juce::String> digests;
    for (size_t i = 0; i < plan.tracks.size(); ++i)
    {
        auto rawName = "source-" + juce::String((int)i) + ".f32";
        auto entry = j["sources"][(int)i];
        int64_t bytes = frames * plan.tracks[i].channels.size() * 4;
        require(entry["raw"].toString() == rawName, "Recording prefix integrity: invalid raw source");
        if (verified)
            require(entry["owner"].toString() == plan.tracks[i].owner && (int64_t)entry["bytes"] == bytes &&
                        entry["sha256"].toString().length() == 64,
                    "Recording prefix integrity: owner or acknowledged byte count mismatch");
        digests.push_back(verifyPrefix(partial.getChildFile(rawName), bytes,
                                       verified ? entry["sha256"].toString() : juce::String()));
    }
    put(j, "prefixIntegrity", verified ? "checkpoint-verified" : "legacy-unverified");
    juce::Array<juce::var> outputs;
    for (size_t i = 0; i < plan.tracks.size(); ++i)
    {
        auto &track = plan.tracks[i];
        int channels = track.layout.size();
        auto rawName = "source-" + juce::String((int)i) + ".f32";
        require(j["sources"][(int)i]["raw"].toString() == rawName, "Invalid raw recording path");
        auto raw = partial.getChildFile(rawName);
        require(raw.getSize() >= frames * channels * 4, "Recording prefix is truncated");
        auto wavName = "Recording-" + juce::String((int)i + 1) + ".wav";
        auto wav = partial.getChildFile(wavName);
        juce::TemporaryFile staged(wav);
        auto stream = staged.getFile().createOutputStream();
        require(stream != nullptr, "Cannot stage source WAV");
        juce::WavAudioFormat format;
        juce::StringPairArray metadata;
        metadata.set(juce::WavAudioFormat::bwavDescription,
                     "SoundPlus layout:" + track.layout.id +
                         "; order:" + track.layout.roles.joinIntoString(","));
        std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(
            stream.release(), device.rate, juceChannelSet(track.layout), 32, metadata, 0));
        require(writer != nullptr, "Cannot create source WAV");
        auto input = raw.createInputStream();
        require(input != nullptr, "Cannot read retained source PCM");
        std::vector<float> interleaved((size_t)4096 * channels);
        juce::AudioBuffer<float> b(channels, 4096);
        auto order = wavOrder(track.layout);
        CC_SHA256_CTX copied;
        CC_SHA256_Init(&copied);
        for (int64_t at = 0; at < frames; at += 4096)
        {
            int n = (int)std::min<int64_t>(4096, frames - at);
            int bytes = n * channels * 4;
            require(input->read(interleaved.data(), bytes) == bytes, "Truncated retained PCM");
            for (int k = 0; k < n * channels; ++k)
                require(std::isfinite(interleaved[(size_t)k]), "Recording prefix integrity: nonfinite PCM");
            CC_SHA256_Update(&copied, interleaved.data(), (CC_LONG)bytes);
            for (int c = 0; c < channels; ++c)
                for (int k = 0; k < n; ++k)
                    b.setSample(c, k, interleaved[(size_t)k * channels + order[(size_t)c]]);
            require(writer->writeFromAudioSampleBuffer(b, 0, n), "Recording WAV write failed");
        }
        writer.reset();
        require(prefixDigest(copied) == digests[i], "Recording prefix integrity: changed during publication");
        syncPath(staged.getFile());
        require(staged.overwriteTargetFileWithTemporary(), "Cannot finalize source WAV");
        auto item = obj();
        put(item, "owner", track.owner);
        put(item, "name", track.name);
        put(item, "path", final.getChildFile(wavName).getFullPathName());
        put(item, "sha256", juce::SHA256(wav).toHexString());
        put(item, "frames", (juce::int64)frames);
        put(item, "rate", device.rate);
        put(item, "layout", layoutJson(track.layout));
        put(item, "peak", j["sources"][(int)i]["peak"]);
        outputs.add(item);
    }
    if (recovered)
        put(j, "originalState", j["state"]);
    put(j, "state", recovered ? "recovered" : "complete");
    put(j, "recovered", recovered);
    put(j, "outputs", outputs);
    atomicJson(partial.getChildFile("receipt.json"), j);
    syncPath(partial);
    require(::rename(partial.getFullPathName().toRawUTF8(), final.getFullPathName().toRawUTF8()) == 0,
            "Cannot atomically publish recording set; partial retained");
    syncPath(plan.root);
    put(j, "receipt", final.getChildFile("receipt.json").getFullPathName());
    return j;
}
juce::var InputRecordingStore::recover(const juce::File &partial, bool allowUnverifiedLegacy)
{
    if (operationId(partial.getFileName()))
    {
        auto j = readJson(partial.getChildFile("receipt.json"));
        require(j["plan"]["id"].toString() == partial.getFileName() &&
                    (j["state"].toString() == "complete" || j["state"].toString() == "recovered"),
                "Retained source recording is not finalized");
        put(j, "receipt", partial.getChildFile("receipt.json").getFullPathName());
        return j;
    }
    require(partial.getFileName().startsWithChar('.') && partial.getFileName().endsWith(".partial"),
            "Choose a retained partial recording folder");
    auto j = readJson(partial.getChildFile("journal.json"));
    require(partial.getFileName() == "." + j["plan"]["id"].toString() + ".partial",
            "Recording operation/folder mismatch");
    return publish(partial, j, true, allowUnverifiedLegacy);
}
juce::Array<juce::var> InputRecordingStore::recoverable(const juce::File &root)
{
    juce::Array<juce::var> a;
    for (auto f : root.findChildFiles(juce::File::findDirectories, false))
        try
        {
            bool complete = operationId(f.getFileName()) && f.getChildFile("receipt.json").existsAsFile();
            if (!complete && !(f.getFileName().startsWithChar('.') && f.getFileName().endsWith(".partial")))
                continue;
            auto j = readJson(f.getChildFile(complete ? "receipt.json" : "journal.json"));
            put(j, "partial", f.getFullPathName());
            if (complete)
                put(j, "receipt", f.getChildFile("receipt.json").getFullPathName());
            a.add(j);
        }
        catch (...)
        {
        }
    return a;
}

InputRecordingService::InputRecordingService(InputTimelineClock &c, const std::atomic<int64_t> *counter)
    : clock(c), outputUnderrunCounter(counter), ring(std::make_unique<std::array<Block, slots>>())
{
    worker = std::thread([this] { run(); });
}
InputRecordingService::~InputRecordingService()
{
    quitting = true;
    accepting = false;
    wake.notify_one();
    if (worker.joinable())
        worker.join();
}
void InputRecordingService::configure(const InputDeviceInfo &d)
{
    require(!busy(), "Stop recording before changing input devices");
    std::lock_guard lock(mutex);
    device = d;
    disableMonitoring();
    failure = none;
    for (auto &peak : peaks)
        peak = 0;
    maxCallbackNs = 0;
    nativeCallbackNs = 0;
}
void InputRecordingService::start(InputRecordingPlan p)
{
    std::lock_guard lock(mutex);
    require(!owned.exchange(true), "An input recording already owns transport");
    try
    {
        validateInputPlan(p, device);
    }
    catch (...)
    {
        owned = false;
        throw;
    }
    failure = none;
    finishId.clear();
    interrupted.clear();
    cancel = false;
    pending = std::move(p);
    wake.notify_one();
}
void InputRecordingService::finish(const juce::String &id, bool c)
{
    std::lock_guard lock(mutex);
    finishId = id;
    cancel = c;
}
void InputRecordingService::interrupt(const juce::String &s)
{
    std::lock_guard lock(mutex);
    interrupted = s;
    fault(deviceLost);
}
void InputRecordingService::fault(Fault f) noexcept
{
    disableMonitoring();
    int expected = none;
    failure.compare_exchange_strong(expected, f);
    accepting = false;
}
void InputRecordingService::push(const float *const *in, int channels, int frames, int64_t sourceFrame,
                                 uint64_t hostNs) noexcept
{
    callbackUsers.fetch_add(1, std::memory_order_acq_rel);
    struct CallbackScope
    {
        std::atomic<int> &users;
        std::atomic<uint64_t> &maximum;
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        ~CallbackScope()
        {
            auto ns = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now() - start)
                          .count();
            if (ns > maximum.load(std::memory_order_relaxed))
                maximum.store(ns, std::memory_order_relaxed);
            users.fetch_sub(1, std::memory_order_release);
        }
    } scope{callbackUsers, maxCallbackNs};
    ++callbackCount;
    if (!in || channels < 1 || channels > maxChannels || frames < 1 || frames > maxFrames)
    {
        if (accepting)
            fault(oversizedBlock);
        return;
    }
    bool valid = hostNs != 0 && sourceFrame >= 0;
    for (int c = 0; c < channels; ++c)
    {
        valid = valid && in[c] != nullptr;
        float peak = 0;
        for (int n = 0; n < frames; ++n)
        {
            auto s = in[c] ? in[c][n] : 0.f;
            valid = valid && std::isfinite(s);
            peak = std::max(peak, std::abs(s));
        }
        peaks[(size_t)c].store(peak, std::memory_order_relaxed);
    }
    if (!valid)
    {
        if (accepting)
            fault(hostNs ? invalidPcm : invalidTimestamp);
        return;
    }
    if (accepting.load(std::memory_order_acquire))
    {
        auto w = write.load(std::memory_order_relaxed);
        if (w - read.load(std::memory_order_acquire) >= slots)
        {
            ++overruns;
            fault(overflow);
        }
        else
        {
            auto &b = (*ring)[(size_t)(w % slots)];
            b.channels = channels;
            b.frames = frames;
            b.deviceFrame = sourceFrame;
            b.hostNs = hostNs;
            for (int n = 0; n < frames; ++n)
                for (int c = 0; c < channels; ++c)
                    b.pcm[(size_t)n * channels + c] = in[c] ? in[c][n] : 0.f;
            write.store(w + 1, std::memory_order_release);
        }
    }
    if (monitoring.load(std::memory_order_acquire))
    {
        auto epoch = monitorEpoch.load(std::memory_order_acquire);
        auto w = monitorWrite.load(std::memory_order_relaxed),
             r = monitorRead.load(std::memory_order_acquire);
        if (w - r + (uint64_t)frames > monitorCapacity)
            ++monitorDrops;
        else
        {
            for (int n = 0; n < frames; ++n)
            {
                monitorEpochTags[(size_t)((w + n) % monitorCapacity)] = epoch;
                for (int c = 0; c < 2; ++c)
                {
                    float sum = 0;
                    for (int k = 0; k < channels; ++k)
                        sum += (in[k] ? in[k][n] : 0.f) *
                               monitorGains[(size_t)c][(size_t)k].load(std::memory_order_relaxed);
                    monitored[(size_t)c][(size_t)((w + n) % monitorCapacity)] = sum;
                }
            }
            monitorWrite.store(w + frames, std::memory_order_release);
        }
    }
}
void InputRecordingService::disableMonitoring() noexcept
{
    monitoring.store(false, std::memory_order_release);
    monitorEpoch.fetch_add(1, std::memory_order_acq_rel);
    monitorFloor.store(monitorWrite.load(std::memory_order_acquire), std::memory_order_release);
}
bool InputRecordingService::setMonitoring(const std::vector<InputTrackPlan> &tracks, double outputRate)
{
    disableMonitoring();
    for (auto &row : monitorGains)
        for (auto &gain : row)
            gain = 0;
    if (!tracks.empty() && (device.rate != outputRate || failure.load() != none))
        return false;
    std::array<std::array<float, maxChannels>, 2> matrix{};
    for (auto &t : tracks)
    {
        InputRecordingPlan p;
        p.id = juce::Uuid().toString();
        p.document = "monitor";
        p.deviceUID = device.uid;
        p.tracks = {t};
        validateInputPlan(p, device);
        auto conversion = conversionMatrix(t.layout, channelLayout("stereo-v1"));
        for (int c = 0; c < 2; ++c)
            for (size_t k = 0; k < t.channels.size(); ++k)
                matrix[(size_t)c][(size_t)t.channels[k]] += conversion[(size_t)c][k];
    }
    for (int c = 0; c < 2; ++c)
        for (int k = 0; k < maxChannels; ++k)
            monitorGains[(size_t)c][(size_t)k] = matrix[(size_t)c][(size_t)k];
    monitoring = !tracks.empty();
    if (!tracks.empty() && failure.load(std::memory_order_acquire) != none)
    {
        disableMonitoring();
        return false;
    }
    return true;
}
void InputRecordingService::monitor(float *const *out, int channels, int frames, float gain) noexcept
{
    auto r = monitorRead.load(std::memory_order_relaxed), w = monitorWrite.load(std::memory_order_acquire);
    if (!monitoring.load(std::memory_order_acquire))
    {
        monitorRead.store(w, std::memory_order_release);
        return;
    }
    auto epoch = monitorEpoch.load(std::memory_order_acquire);
    r = std::max(r, monitorFloor.load(std::memory_order_acquire));
    if (r > w)
        return; // A newer configuration's floor overtook this callback's old write snapshot.
    int count = (int)std::min<uint64_t>(w - r, (uint64_t)frames);
    for (int n = 0; n < count; ++n)
        for (int c = 0; c < std::min(2, channels); ++c)
            if (out[c] && monitorEpochTags[(size_t)((r + n) % monitorCapacity)] == epoch)
                out[c][n] += monitored[(size_t)c][(size_t)((r + n) % monitorCapacity)] * gain;
    monitorRead.store(r + count, std::memory_order_release);
}
juce::var InputRecordingService::status()
{
    std::lock_guard lock(mutex);
    auto v = observation.isObject() ? observation.clone() : obj();
    put(v, "device", device.json());
    put(v, "busy", busy());
    put(v, "monitoring", monitoring.load());
    juce::Array<juce::var> a;
    for (int c = 0; c < device.channels; ++c)
        a.add(peaks[(size_t)c].load());
    put(v, "peaks", a);
    put(v, "callbacks", (juce::int64)callbackCount.load());
    put(v, "overruns", (juce::int64)overruns.load());
    put(v, "monitorDrops", (juce::int64)monitorDrops.load());
    put(v, "maxInputPushUs", maxCallbackNs.load() / 1000.0);
    put(v, "maxNativeInputCallbackUs", nativeCallbackNs.load() / 1000.0);
    return v;
}
void InputRecordingService::run()
{
    std::unique_ptr<InputRecordingStore> store;
    InputRecordingPlan active;
    InputDeviceInfo info;
    int64_t nextFrame = -1;
    int previousFrames = 0;
    uint64_t lastHost = 0;
    auto publish = [&](juce::var v)
    {
        std::lock_guard lock(mutex);
        observation = std::move(v);
    };
    auto lastCheckpoint = std::chrono::steady_clock::now(), started = lastCheckpoint;
    juce::String reason;
    bool stopping = false, cancelled = false;
    InputTimelineClock::Value anchor;
    uint64_t callbackBase = 0, overrunBase = 0, monitorDropBase = 0;
    int64_t outputUnderrunBase = 0;
    auto diagnostics = [&]
    {
        auto v = obj();
        put(v, "inputCallbacks", (juce::int64)(callbackCount.load() - callbackBase));
        put(v, "inputOverruns", (juce::int64)(overruns.load() - overrunBase));
        put(v, "monitorDrops", (juce::int64)(monitorDrops.load() - monitorDropBase));
        put(v, "maxInputPushUsSinceDeviceOpen", maxCallbackNs.load() / 1000.0);
        put(v, "maxNativeInputCallbackUsSinceDeviceOpen", nativeCallbackNs.load() / 1000.0);
        put(v, "outputUnderruns",
            outputUnderrunCounter ? outputUnderrunCounter->load() - outputUnderrunBase : int64_t(-1));
        put(v, "ringSlots", slots);
        put(v, "maxCallbackFrames", maxFrames);
        return v;
    };
    for (;;)
    {
        std::optional<InputRecordingPlan> request;
        {
            std::unique_lock lock(mutex);
            if (!store)
                wake.wait(lock, [&] { return quitting || pending.has_value(); });
            if (pending)
            {
                request = std::move(pending);
                pending.reset();
                info = device;
            }
        }
        if (request)
        {
            active = std::move(*request);
            callbackBase = callbackCount.load();
            overrunBase = overruns.load();
            monitorDropBase = monitorDrops.load();
            outputUnderrunBase = outputUnderrunCounter ? outputUnderrunCounter->load() : 0;
            reason.clear();
            stopping = cancelled = false;
            nextFrame = -1;
            previousFrames = 0;
            lastHost = 0;
            anchor = {};
            auto v = obj();
            put(v, "id", active.id);
            put(v, "state", "preparing");
            publish(v);
            try
            {
                store = std::make_unique<InputRecordingStore>(active, info);
                read = write.load();
                accepting = true;
                started = lastCheckpoint = std::chrono::steady_clock::now();
            }
            catch (const std::exception &e)
            {
                put(v, "state", "incomplete");
                put(v, "reason", e.what());
                publish(v);
                owned = false;
                store.reset();
            }
        }
        if (store)
        {
            {
                std::lock_guard lock(mutex);
                if (finishId == active.id)
                {
                    stopping = true;
                    cancelled = cancel;
                    finishId.clear();
                }
                if (interrupted.isNotEmpty())
                    reason = interrupted;
            }
            if (quitting)
            {
                stopping = true;
                reason = "Application/helper interrupted; recover retained source PCM";
            }
            if (failure.load() != none)
            {
                stopping = true;
                if (reason.isEmpty())
                    reason = faultName(failure.load());
            }
            if (stopping)
                accepting = false;
            if (!anchor.hostNs)
            {
                auto c = clock.read();
                if (c.hostNs && c.generation == active.generation && c.rate == active.projectRate)
                    anchor = c;
            }
            if (!anchor.hostNs && std::chrono::steady_clock::now() - started > std::chrono::seconds(10))
            {
                stopping = true;
                reason = "Playback clock did not become available; input was not placed";
                accepting = false;
            }
            auto r = read.load(std::memory_order_relaxed), w = write.load(std::memory_order_acquire);
            if (!holdWriter.load() || quitting || stopping)
            {
                for (; r < w; ++r)
                {
                    auto &b = (*ring)[(size_t)(r % slots)];
                    try
                    {
                        if (nextFrame >= 0 && (b.deviceFrame != nextFrame || b.hostNs <= lastHost ||
                                               std::abs((double)(b.hostNs - lastHost) -
                                                        previousFrames * 1.e9 / info.rate) > 2000000))
                            throw std::runtime_error(faultName(discontinuity));
                        nextFrame = b.deviceFrame + b.frames;
                        previousFrames = b.frames;
                        lastHost = b.hostNs;
                        if (!anchor.hostNs)
                            continue;
                        long double project =
                            (long double)anchor.frame +
                            ((long double)b.hostNs - anchor.hostNs) * active.projectRate / 1.e9L;
                        int skip = (int)std::clamp<long double>(
                            std::ceil((active.start - project) * info.rate / active.projectRate), 0,
                            b.frames);
                        if (skip == b.frames)
                            continue;
                        if (reason.isEmpty() && !cancelled)
                        {
                            store->writeBudget = budget.load();
                            store->append(
                                b.pcm.data() + (size_t)skip * b.channels, b.frames - skip, b.channels,
                                b.deviceFrame + skip,
                                b.hostNs + (uint64_t)std::llround(skip * 1.e9 / info.rate),
                                (int64_t)std::llround(project + skip * active.projectRate / info.rate));
                        }
                    }
                    catch (const std::exception &e)
                    {
                        if (reason.isEmpty())
                            reason = e.what();
                        stopping = true;
                        accepting = false;
                    }
                }
                read.store(r, std::memory_order_release);
            }
            if (!stopping &&
                std::chrono::steady_clock::now() - lastCheckpoint > std::chrono::milliseconds(250))
                try
                {
                    store->setDiagnostics(diagnostics());
                    store->checkpoint();
                    lastCheckpoint = std::chrono::steady_clock::now();
                }
                catch (const std::exception &e)
                {
                    reason = e.what();
                    stopping = true;
                    accepting = false;
                }
            if (stopping && callbackUsers.load(std::memory_order_acquire) == 0 && read.load() == write.load())
            {
                auto v = obj();
                put(v, "id", active.id);
                put(v, "state", "finalizing");
                publish(v);
                try
                {
                    store->setDiagnostics(diagnostics());
                    v = store->finish(reason.isEmpty() && !cancelled, reason, cancelled);
                    put(v, "id", active.id);
                }
                catch (const std::exception &e)
                {
                    put(v, "state", "incomplete");
                    put(v, "reason", e.what());
                    put(v, "partial",
                        active.root.getChildFile("." + active.id + ".partial").getFullPathName());
                }
                publish(v);
                store.reset();
                owned = false;
            }
            else if (store)
            {
                auto v = obj();
                put(v, "id", active.id);
                put(v, "state", anchor.hostNs ? "recording" : "preparing");
                put(v, "frames", (juce::int64)store->frames());
                put(v, "rate", info.rate);
                publish(v);
            }
        }
        if (quitting && !store)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
} // namespace sp
