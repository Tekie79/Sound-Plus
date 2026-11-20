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
    bool valid = false;
    HostedRack configuration;
    ~Impl()
    {
        if (exchange)
            exchange->closing.store(1, std::memory_order_release);
        if (pid > 0)
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
            if (exchange->failed.load(std::memory_order_acquire))
                throw std::runtime_error(exchange->reason);
            if (cancelled())
                throw QueryCancelled();
            rusage_info_v4 usage{};
            if (proc_pid_rusage(pid, RUSAGE_INFO_V4, (rusage_info_t *)&usage) == 0 &&
                usage.ri_phys_footprint > 1024ULL * 1024 * 1024)
                throw std::runtime_error("Hosted rack exceeded the one-GiB resident-memory bound");
            int status = 0;
            if (waitpid(pid, &status, WNOHANG) == pid)
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
                                   const std::function<bool()> &cancelled)
    : impl(std::make_unique<Impl>())
{
    validateHostedRack(rack);
    validateLayout(layout);
    impl->configuration = rack;
    auto exe = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    auto helper = exe.getSiblingFile("SoundPlusPlugin");
    if (!helper.existsAsFile())
        helper = exe.getParentDirectory().getParentDirectory().getChildFile("Helpers/SoundPlusPlugin");
    auto override = juce::SystemStats::getEnvironmentVariable("SOUNDPLUS_PLUGIN_HELPER", {});
    if (override.isNotEmpty())
        helper = juce::File(override);
    if (!helper.existsAsFile())
        throw std::runtime_error("Persistent plugin helper unavailable");
    impl->workspace = juce::File::getSpecialLocation(juce::File::tempDirectory)
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
    return impl->exchange->latency;
}
void HostedRackClient::process(juce::AudioBuffer<float> &buffer, int frames)
{
    auto *e = impl->exchange;
    if (!impl->valid || frames < 1 || frames > 512 || buffer.getNumChannels() != e->channels)
        throw std::runtime_error("Invalid hosted rack block");
    for (int c = 0; c < e->channels; ++c)
        std::copy_n(buffer.getReadPointer(c), frames, e->input[c]);
    e->frames = frames;
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
                value["id"] != oldParameters[n]["id"])
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

} // namespace sp
