#include "HostedRack.h"
#include "HostedRackExchange.h"
#include "PluginProcessing.h"
#include <sys/mman.h>
#include <sys/wait.h>
#include <spawn.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <cmath>
#include <set>
#include <libproc.h>
extern char **environ;
namespace sp
{
juce::var hostedRackJson(const HostedRack &rack)
{
    auto *o = new juce::DynamicObject();
    o->setProperty("version", 1);
    o->setProperty("id", rack.id);
    o->setProperty("bypass", rack.bypass);
    juce::Array<juce::var> slots;
    for (auto &slot : rack.slots)
    {
        auto *s = new juce::DynamicObject();
        s->setProperty("id", slot.id);
        s->setProperty("bypass", slot.bypass);
        s->setProperty("settings", juce::JSON::parse(slot.settings));
        slots.add(juce::var(s));
    }
    o->setProperty("slots", slots);
    return juce::var(o);
}
HostedRack hostedRackFromJson(const juce::var &v)
{
    HostedRack rack;
    if (v.isVoid())
        return rack;
    if (!v.isObject() || (int)v["version"] != 1 || !v["slots"].isArray())
        throw std::runtime_error("Unsupported hosted rack version");
    rack.id = v["id"].toString();
    rack.bypass = (bool)v["bypass"];
    for (auto &s : *v["slots"].getArray())
        rack.slots.push_back(
            {s["id"].toString(), juce::JSON::toString(s["settings"], true), (bool)s["bypass"]});
    validateHostedRack(rack);
    return rack;
}
void validateHostedRack(const HostedRack &r)
{
    if (r.id.isEmpty() || r.slots.size() > 8)
        throw std::runtime_error("Hosted rack requires an identity and at most eight slots");
    juce::StringArray ids;
    for (auto &s : r.slots)
    {
        auto v = juce::JSON::parse(s.settings);
        auto format = v["plugin"]["format"].toString();
        if (s.id.isEmpty() || ids.contains(s.id) || s.settings.getNumBytesAsUTF8() > 6 * 1024 * 1024 ||
            !v.isObject() || (format != "VST3" && format != "AudioUnit") ||
            v["plugin"]["id"].toString().isEmpty())
            throw std::runtime_error("Invalid hosted slot identity/format/state");
        if (!v["parameters"].isArray() || v["parameters"].size() > 512)
            throw std::runtime_error("Hosted parameter bound/description missing");
        std::set<int> indices;
        for (auto &parameter : *v["parameters"].getArray())
        {
            int index = (int)parameter["index"];
            double value = (double)parameter["value"];
            if (index < 0 || index >= 16384 || !indices.insert(index).second || !std::isfinite(value) ||
                value < 0 || value > 1)
                throw std::runtime_error("Invalid hosted parameter identity/value");
        }
        ids.add(s.id);
    }
}
struct HostedRackClient::Impl
{
    juce::File workspace;
    int fd = -1;
    pid_t pid = -1;
    HostedRackExchange *exchange = nullptr;
    uint32_t sequence = 0;
    bool valid = false, borrowed = false, liveOwner = false, composite = false;
    uint64_t lease = 0, commandSequence = 0, modelCommand = 0;
    bool leaseAcquired = false;
    juce::String instance;
    std::vector<std::unique_ptr<HostedRackClient>> children;
    int compositeLatency = 0;
    HostedRack configuration;
    std::vector<std::map<int, std::pair<int64_t, uint64_t>>> pendingIndices;
    std::vector<bool> pendingState;
    std::vector<uint64_t> restoreSequences;
    std::vector<int64_t> restoreOrigins;
    ~Impl()
    {
        if (exchange)
        {
            if (borrowed && leaseAcquired)
            {
                auto mine = lease;
                exchange->lease.compare_exchange_strong(mine, 0);
            }
            else if (!borrowed)
                exchange->closing.store(1, std::memory_order_release);
        }
        if (pid > 0 && !borrowed)
        {
            int status = 0;
            auto end = juce::Time::getMillisecondCounterHiRes() + 100;
            while (waitpid(pid, &status, WNOHANG) == 0 && juce::Time::getMillisecondCounterHiRes() < end)
                juce::Thread::sleep(1);
            if (waitpid(pid, &status, WNOHANG) == 0)
            {
                kill(pid, SIGKILL);
                while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
                {
                }
            }
        }
        if (liveOwner)
        {
            try
            {
                auto endpoint = workspace.getParentDirectory().getChildFile("endpoint.json");
                if (loadProcessingJson(endpoint)["generation"].toString() == instance)
                    endpoint.deleteFile();
            }
            catch (const std::exception &)
            {
            }
        }
        if (exchange)
            munmap(exchange, sizeof(HostedRackExchange));
        if (fd >= 0)
            close(fd);
        // Only ephemeral exchange/configuration files are removed; no PCM/state
        // authority or original source is ever stored in this private directory.
        if (workspace.getFileName().startsWith("SoundPlus-rack-"))
            workspace.deleteRecursively();
    }
    void await(uint32_t target, double milliseconds, const std::function<bool()> &cancelled)
    {
        auto end = juce::Time::getMillisecondCounterHiRes() + milliseconds;
        while ((target ? exchange->response.load(std::memory_order_acquire) != target
                       : !exchange->ready.load(std::memory_order_acquire)))
        {
            if (borrowed && exchange->closing.load(std::memory_order_acquire))
                throw std::runtime_error("Live insert owner closed; no output is certified");
            if (exchange->failed.load(std::memory_order_acquire))
                throw std::runtime_error(exchange->reason);
            if (cancelled())
                throw QueryCancelled();
            rusage_info_v4 usage{};
            if (proc_pid_rusage(pid, RUSAGE_INFO_V4, (rusage_info_t *)&usage) == 0 &&
                usage.ri_phys_footprint > 1024ULL * 1024 * 1024)
                throw std::runtime_error("Hosted rack exceeded the one-GiB resident-memory bound");
            int status = 0;
            if ((borrowed && ::kill(pid, 0) != 0 && errno == ESRCH) ||
                (!borrowed && waitpid(pid, &status, WNOHANG) == pid))
            {
                pid = -1;
                throw std::runtime_error("Hosted rack service exited; no output is certified");
            }
            if (juce::Time::getMillisecondCounterHiRes() > end)
                throw std::runtime_error(
                    "Hosted rack missed its bounded processing deadline; playback stopped");
            juce::Thread::sleep(1);
        }
        if (exchange->failed.load(std::memory_order_acquire))
            throw std::runtime_error(exchange->reason);
    }
};
HostedRackClient::HostedRackClient(const HostedRack &rack, const ChannelLayout &layout, double rate,
                                   const std::function<bool()> &cancelled, const juce::File &liveDirectory,
                                   const juce::var &binding)
    : impl(std::make_unique<Impl>())
{
    validateHostedRack(rack);
    validateLayout(layout);
    impl->configuration = rack;
    if (rack.runtimeEndpoint != juce::File())
    {
        if (rack.runtimeOwnerRack.isEmpty())
        {
            impl->composite = true;
            for (auto &slot : rack.slots)
            {
                HostedRack single;
                single.id = slot.id;
                single.bypass = rack.bypass;
                single.slots = {slot};
                single.runtimeOwnerRack = rack.id;
                single.runtimeEndpoint =
                    rack.runtimeEndpoint.getChildFile(slot.id).getChildFile("endpoint.json");
                auto child = std::make_unique<HostedRackClient>(single, layout, rate, cancelled);
                impl->compositeLatency += child->latency();
                impl->children.push_back(std::move(child));
                if (impl->compositeLatency > rate)
                    throw std::runtime_error("Live rack latency exceeds one second");
            }
            impl->valid = true;
            return;
        }
        auto end = juce::Time::getMillisecondCounterHiRes() + 20000;
        juce::var endpoint;
        while (!endpoint.isObject())
        {
            if (cancelled())
                throw QueryCancelled();
            try
            {
                auto v = loadProcessingJson(rack.runtimeEndpoint);
                bool matches = v["rack"].toString() == rack.id && (double)v["rate"] == rate &&
                               layoutFromJson(v["layout"]) == layout &&
                               v["binding"]["rack"].toString() == rack.runtimeOwnerRack &&
                               v["slots"].size() == (int)rack.slots.size();
                for (size_t i = 0; matches && i < rack.slots.size(); ++i)
                {
                    auto plugin = juce::JSON::parse(rack.slots[i].settings)["plugin"];
                    matches = v["slots"][(int)i]["slot"].toString() == rack.slots[i].id;
                    for (auto key : {"id", "path", "format", "fingerprint"})
                        matches = matches && v["slots"][(int)i]["plugin"][key] == plugin[key];
                }
                if (matches)
                    endpoint = v;
            }
            catch (const std::exception &)
            {
            }
            if (!endpoint.isObject())
            {
                if (juce::Time::getMillisecondCounterHiRes() > end)
                    throw std::runtime_error("Live insert unavailable or reconnecting");
                juce::Thread::sleep(5);
            }
        }
        impl->borrowed = true;
        impl->pid = (pid_t)(int)endpoint["pid"];
        impl->instance = endpoint["generation"].toString();
        impl->workspace = rack.runtimeEndpoint.getParentDirectory();
        auto path = juce::File(endpoint["exchange"].toString());
        impl->fd = ::open(path.getFullPathName().toRawUTF8(), O_RDWR);
        if (impl->fd < 0)
            throw std::runtime_error("Live insert exchange unavailable");
        auto memory =
            mmap(nullptr, sizeof(HostedRackExchange), PROT_READ | PROT_WRITE, MAP_SHARED, impl->fd, 0);
        if (memory == MAP_FAILED)
            throw std::runtime_error("Cannot attach live insert exchange");
        impl->exchange = static_cast<HostedRackExchange *>(memory);
        if (impl->exchange->protocol != HostedRackExchange::magic ||
            impl->exchange->channels != layout.size())
            throw std::runtime_error("Live insert protocol/layout mismatch");
        impl->lease = (uint64_t)juce::Uuid().toString().hashCode64();
        if (impl->lease == 0)
            impl->lease = 1;
        uint64_t empty = 0;
        while (!impl->exchange->lease.compare_exchange_weak(empty, impl->lease))
        {
            empty = 0;
            if (cancelled())
                throw QueryCancelled();
            if (juce::Time::getMillisecondCounterHiRes() > end)
                throw std::runtime_error("Previous live insert producer did not release ownership");
            juce::Thread::sleep(1);
        }
        impl->leaseAcquired = true;
        if (impl->exchange->closing.load() || impl->exchange->failed.load())
            throw std::runtime_error("Live insert unavailable; no output is certified");
        // A timed-out producer may have released its lease with a block still
        // executing. Drain that entire request before resetting/reusing its ring.
        impl->sequence = impl->exchange->request.load(std::memory_order_acquire);
        if (impl->exchange->response.load(std::memory_order_acquire) != impl->sequence)
            impl->await(impl->sequence, 20000, cancelled);
        impl->exchange->resetRequested.store(impl->lease, std::memory_order_release);
        while (impl->exchange->resetAcknowledged.load(std::memory_order_acquire) != impl->lease)
        {
            if (impl->exchange->closing.load() || impl->exchange->failed.load())
                throw std::runtime_error("Live insert failed during reset; no output is certified");
            if (cancelled())
                throw QueryCancelled();
            if (juce::Time::getMillisecondCounterHiRes() > end)
                throw std::runtime_error("Live insert reset timed out");
            juce::Thread::sleep(1);
        }
        impl->valid = true;
        return;
    }
    auto exe = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    auto helper = exe.getSiblingFile("SoundPlusPlugin");
    if (!helper.existsAsFile())
        helper = exe.getParentDirectory().getParentDirectory().getChildFile("Helpers/SoundPlusPlugin");
    auto override = juce::SystemStats::getEnvironmentVariable("SOUNDPLUS_PLUGIN_HELPER", {});
    if (override.isNotEmpty())
        helper = juce::File(override);
    if (!helper.existsAsFile())
        throw std::runtime_error("Persistent plugin helper unavailable");
    impl->liveOwner = liveDirectory != juce::File();
    impl->instance = juce::Uuid().toString();
    if (impl->liveOwner)
        helper = helper.getSiblingFile("SoundPlusPluginEditor.app/Contents/MacOS/SoundPlusPluginEditor");
    impl->workspace = impl->liveOwner ? liveDirectory
                                      : juce::File::getSpecialLocation(juce::File::tempDirectory)
                                            .getChildFile("SoundPlus-rack-" + juce::Uuid().toString());
    if (impl->workspace.createDirectory().failed())
        throw std::runtime_error("Cannot prepare hosted rack exchange");
    auto path = impl->workspace.getChildFile("exchange");
    impl->fd = ::open(path.getFullPathName().toRawUTF8(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (impl->fd < 0 || ftruncate(impl->fd, sizeof(HostedRackExchange)) != 0)
        throw std::runtime_error("Cannot allocate hosted exchange");
    auto memory = mmap(nullptr, sizeof(HostedRackExchange), PROT_READ | PROT_WRITE, MAP_SHARED, impl->fd, 0);
    if (memory == MAP_FAILED)
        throw std::runtime_error("Cannot map hosted exchange");
    impl->exchange = new (memory) HostedRackExchange();
    impl->exchange->channels = layout.size();
    auto *job = new juce::DynamicObject();
    job->setProperty("protocol", 1);
    job->setProperty("mode", "rack");
    job->setProperty("rack", hostedRackJson(rack));
    job->setProperty("layout", layoutJson(layout));
    job->setProperty("rate", rate);
    job->setProperty("block", 512);
    job->setProperty("exchange", path.getFullPathName());
    if (impl->liveOwner)
    {
        job->setProperty("liveDirectory", impl->workspace.getFullPathName());
        job->setProperty("generation", impl->instance);
        job->setProperty("binding", binding);
        job->setProperty(
            "endpoint", impl->workspace.getParentDirectory().getChildFile("endpoint.json").getFullPathName());
    }
    job->setProperty("reply", impl->workspace.getChildFile("reply.json").getFullPathName());
    auto config = impl->workspace.getChildFile("job.json");
    saveProcessingJson(config, juce::var(job));
    auto executable = helper.getFullPathName().toStdString(),
         argument = config.getFullPathName().toStdString();
    char *argv[]{executable.data(), argument.data(), nullptr};
    if (posix_spawn(&impl->pid, executable.c_str(), nullptr, nullptr, argv, environ) != 0)
        throw std::runtime_error("Cannot launch persistent rack service");
    impl->await(0, 20000, cancelled);
    if (impl->exchange->channels != layout.size() || impl->exchange->latency < 0 ||
        impl->exchange->latency > rate)
        throw std::runtime_error("Invalid hosted service latency/bus response");
    impl->valid = true;
}
HostedRackClient::~HostedRackClient() = default;
int HostedRackClient::latency() const
{
    return impl->composite ? impl->compositeLatency : impl->exchange->latency;
}
void HostedRackClient::process(juce::AudioBuffer<float> &buffer, int frames)
{
    if (impl->composite)
    {
        for (auto &child : impl->children)
            child->process(buffer, frames);
        return;
    }
    auto *e = impl->exchange;
    if (impl->liveOwner)
        throw std::runtime_error("UI live owner cannot drive PCM");
    if (!impl->valid || frames < 1 || frames > 512 || buffer.getNumChannels() != e->channels)
        throw std::runtime_error("Invalid hosted rack block");
    for (int c = 0; c < e->channels; ++c)
        std::copy_n(buffer.getReadPointer(c), frames, e->input[c]);
    e->frames = frames;
    e->lastRequestTicks.store(juce::Time::getHighResolutionTicks(), std::memory_order_release);
    e->request.store(++impl->sequence, std::memory_order_release);
    // Producer-thread wait only. The device callback never touches this mapping.
    try
    {
        impl->await(impl->sequence, 250, [] { return false; });
    }
    catch (...)
    {
        impl->valid = false;
        buffer.clear();
        throw;
    }
    for (int c = 0; c < e->channels; ++c)
        std::copy_n(e->output[c], frames, buffer.getWritePointer(c));
}
void HostedRackClient::update(const HostedRack &rack)
{
    validateHostedRack(rack);
    if (impl->composite)
    {
        if (rack.slots.size() != impl->children.size())
            throw std::runtime_error("Live rack topology changed; reprepare");
        for (size_t i = 0; i < rack.slots.size(); ++i)
        {
            HostedRack single;
            single.id = rack.slots[i].id;
            single.bypass = rack.bypass;
            single.slots = {rack.slots[i]};
            impl->children[i]->update(single);
        }
        impl->configuration = rack;
        return;
    }
    if (impl->borrowed)
    {
        impl->configuration = rack;
        return;
    }
    if (impl->liveOwner)
    {
        auto v = juce::var(new juce::DynamicObject());
        v.getDynamicObject()->setProperty("generation", impl->instance);
        v.getDynamicObject()->setProperty("sequence",
                                          (juce::int64)(impl->modelCommand = ++impl->commandSequence));
        v.getDynamicObject()->setProperty(
            "eventTicks", (juce::int64)(rack.runtimeControlTicks > 0 ? rack.runtimeControlTicks
                                                                     : juce::Time::getHighResolutionTicks()));
        if (impl->pendingIndices.size() != rack.slots.size())
        {
            impl->pendingIndices.resize(rack.slots.size());
            impl->pendingState.resize(rack.slots.size());
            impl->restoreSequences.resize(rack.slots.size());
            impl->restoreOrigins.resize(rack.slots.size());
        }
        juce::Array<juce::var> changes;
        for (size_t i = 0; i < rack.slots.size(); ++i)
        {
            auto before = juce::JSON::parse(impl->configuration.slots.at(i).settings);
            auto after = juce::JSON::parse(rack.slots[i].settings);
            auto row = juce::var(new juce::DynamicObject());
            const bool changedState =
                before["stateHash"] != after["stateHash"] || before["program"] != after["program"] ||
                rack.slots[i].runtimeRestoreOrigin > impl->configuration.slots[i].runtimeRestoreOrigin;
            if (changedState)
            {
                impl->pendingState[i] = true;
                impl->restoreSequences[i] = impl->modelCommand;
                impl->restoreOrigins[i] = rack.slots[i].runtimeRestoreOrigin > 0
                                              ? rack.slots[i].runtimeRestoreOrigin
                                              : (juce::int64)v["eventTicks"];
            }
            row.getDynamicObject()->setProperty("fullState", (bool)impl->pendingState[i]);
            row.getDynamicObject()->setProperty("restoreSequence", (juce::int64)impl->restoreSequences[i]);
            row.getDynamicObject()->setProperty("restoreTicks", (juce::int64)impl->restoreOrigins[i]);
            for (int n = 0; n < after["parameters"].size(); ++n)
            {
                const int nativeIndex = (int)after["parameters"][n]["index"];
                auto nextOrigin = rack.slots[i].runtimeParameterOrigins.find(nativeIndex);
                auto oldOrigin = impl->configuration.slots[i].runtimeParameterOrigins.find(nativeIndex);
                const bool newIntent =
                    nextOrigin != rack.slots[i].runtimeParameterOrigins.end() &&
                    nextOrigin->second >
                        (oldOrigin == impl->configuration.slots[i].runtimeParameterOrigins.end()
                             ? 0
                             : oldOrigin->second);
                if (newIntent || after["parameters"][n]["value"] != before["parameters"][n]["value"])
                {
                    const int index = (int)after["parameters"][n]["index"];
                    auto origin = rack.slots[i].runtimeParameterOrigins.find(index);
                    impl->pendingIndices[i][index] = {origin == rack.slots[i].runtimeParameterOrigins.end()
                                                          ? (juce::int64)v["eventTicks"]
                                                          : origin->second,
                                                      impl->modelCommand};
                }
            }
            juce::Array<juce::var> indices;
            auto origins = juce::var(new juce::DynamicObject());
            for (auto &[index, intent] : impl->pendingIndices[i])
            {
                indices.add(index);
                origins.getDynamicObject()->setProperty(juce::String(index), (juce::int64)intent.first);
            }
            row.getDynamicObject()->setProperty("origins", origins);
            row.getDynamicObject()->setProperty("indices", indices);
            changes.add(row);
        }
        v.getDynamicObject()->setProperty("changes", changes);
        v.getDynamicObject()->setProperty("rack", hostedRackJson(rack));
        saveProcessingJson(impl->workspace.getChildFile("model.json"), v);
        impl->configuration = rack;
        return;
    }
    auto &before = impl->configuration;
    if (rack.id != before.id || rack.slots.size() != before.slots.size())
        throw std::runtime_error("Hosted rack topology changed");
    auto *e = impl->exchange;
    e->bypass = 0;
    for (size_t i = 0; i < rack.slots.size(); ++i)
    {
        auto old = juce::JSON::parse(before.slots[i].settings),
             next = juce::JSON::parse(rack.slots[i].settings);
        auto parameters = next["parameters"];
        auto oldParameters = old["parameters"];
        old.getDynamicObject()->removeProperty("parameters");
        next.getDynamicObject()->removeProperty("parameters");
        if (rack.slots[i].id != before.slots[i].id ||
            juce::JSON::toString(old, true) != juce::JSON::toString(next, true) || !parameters.isArray() ||
            parameters.size() > 512)
            throw std::runtime_error("Hosted state/topology changed; reprepare graph");
        e->parameterCount[i] = parameters.size();
        for (int n = 0; n < parameters.size(); ++n)
        {
            auto value = parameters[n];
            if (n >= oldParameters.size() || value["index"] != oldParameters[n]["index"] ||
                value["id"] != oldParameters[n]["id"] || value["stableId"] != oldParameters[n]["stableId"])
                throw std::runtime_error("Hosted parameter ID changed; reprepare graph");
            double x = (double)value["value"];
            if (!std::isfinite(x) || x < 0 || x > 1)
                throw std::runtime_error("Invalid live hosted parameter");
            e->parameterIndex[i][n] = (int)value["index"];
            e->parameterValue[i][n] = (float)x;
        }
        if (rack.bypass || rack.slots[i].bypass)
            e->bypass |= 1u << i;
    }
    ++e->controls;
    before = rack;
}

void HostedRackClient::acceptCapture(const HostedRack &rack)
{
    impl->configuration = rack;
    if (impl->liveOwner)
    {
        auto record = juce::var(new juce::DynamicObject());
        auto *o = record.getDynamicObject();
        o->setProperty("generation", impl->instance);
        o->setProperty("sequence", (juce::int64)++impl->commandSequence);
        o->setProperty("rack", hostedRackJson(rack));
        saveProcessingJson(impl->workspace.getChildFile("baseline.json"), record);
    }
}
uint64_t HostedRackClient::modelSequence() const
{
    return impl->modelCommand;
}
void HostedRackClient::checkHealth() const
{
    if (!impl->exchange || impl->exchange->failed.load() || impl->pid <= 0 ||
        (::kill(impl->pid, 0) != 0 && errno == ESRCH))
        throw std::runtime_error("Live insert unavailable; last coherent checkpoint retained");
    const auto tick = impl->exchange->heartbeatTicks.load();
    if (tick > 0 && juce::Time::highResolutionTicksToSeconds(juce::Time::getHighResolutionTicks() - tick) > 5)
        throw std::runtime_error("Live insert unresponsive; last coherent checkpoint retained");
}
juce::var HostedRackClient::liveState() const
{
    auto state = loadProcessingJson(impl->workspace.getChildFile("state.json"));
    const auto acknowledged = (uint64_t)(juce::int64)state["modelSequence"];
    for (size_t i = 0; i < impl->pendingIndices.size(); ++i)
    {
        std::erase_if(impl->pendingIndices[i],
                      [&](auto &intent) { return intent.second.second <= acknowledged; });
        if (impl->restoreSequences[i] <= acknowledged)
            impl->pendingState[i] = false;
    }
    return state;
}
juce::String HostedRackClient::instanceGeneration() const
{
    return impl->instance;
}
void HostedRackClient::editor(const juce::String &slot, bool open)
{
    auto v = juce::var(new juce::DynamicObject());
    auto *o = v.getDynamicObject();
    o->setProperty("generation", impl->instance);
    o->setProperty("sequence", (juce::int64)++impl->commandSequence);
    o->setProperty("slot", slot);
    o->setProperty("open", open);
    saveProcessingJson(impl->workspace.getChildFile("editor.json"), v);
}
void HostedRackClient::capture(uint64_t sequence)
{
    auto v = juce::var(new juce::DynamicObject());
    auto *o = v.getDynamicObject();
    o->setProperty("generation", impl->instance);
    o->setProperty("sequence", (juce::int64)sequence);
    saveProcessingJson(impl->workspace.getChildFile("capture.json"), v);
}
} // namespace sp
