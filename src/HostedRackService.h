#pragma once
#include "HostedRack.h"
#include "HostedRackExchange.h"
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
// Included after the isolated worker's instance/layout/restore functions.
namespace
{
var serveRack(const var &job, AudioPluginFormatManager &formats)
{
    auto path = File(job["exchange"].toString());
    int fd = ::open(path.getFullPathName().toRawUTF8(), O_RDWR);
    check(fd >= 0, "Cannot open hosted rack exchange");
    auto memory = mmap(nullptr, sizeof(sp::HostedRackExchange), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    check(memory != MAP_FAILED, "Cannot map hosted rack exchange");
    auto *e = static_cast<sp::HostedRackExchange *>(memory);
    struct Unmap
    {
        void *p;
        ~Unmap()
        {
            munmap(p, sizeof(sp::HostedRackExchange));
        }
    } unmap{memory};
    try
    {
        check(e->protocol == sp::HostedRackExchange::magic, "Hosted exchange protocol mismatch");
        auto rack = sp::hostedRackFromJson(job["rack"]);
        auto bus = sp::layoutFromJson(job["layout"]);
        auto rate = (double)job["rate"];
        check(rate >= 8000 && rate <= 192000 && bus.size() == e->channels, "Invalid rack bus/rate");
        struct Slot
        {
            HostedInstance effect;
            int latency = 0, write = 0;
            bool bypass = false;
            AudioBuffer<float> dry;
            AudioProcessor::BusesLayout buses;
            StringArray parameterIds;
        };
        std::vector<Slot> slots;
        int totalLatency = 0;
        for (auto &slot : rack.slots)
        {
            auto settings = JSON::parse(slot.settings);
            auto p = instance(formats, settings["plugin"], rate, 512);
            onPluginMessageThread(
                [&]
                {
                    restore(*p, settings);
                    layout(*p, bus);
                    p->setNonRealtime(false);
                    p->setRateAndBufferSizeDetails(rate, 512);
                    p->prepareToPlay(rate, 512);
                    p->reset();
                });
            int latency = p->getLatencySamples();
            check(latency >= 0 && latency <= (int)rate && totalLatency + latency <= (int)rate,
                  "Hosted rack latency exceeds one-second bound");
            Slot prepared;
            prepared.latency = latency;
            prepared.bypass = rack.bypass || slot.bypass;
            prepared.buses = p->getBusesLayout();
            onPluginMessageThread(
                [&]
                {
                    for (int i = 0; i < p->getParameters().size(); ++i)
                        prepared.parameterIds.add(p->getParameterID(i));
                });
            prepared.effect = std::move(p);
            prepared.dry.setSize(bus.size(), std::max(1, latency + 1));
            prepared.dry.clear();
            slots.push_back(std::move(prepared));
            totalLatency += latency;
        }
        e->latency = totalLatency;
        AudioBuffer<float> audio(bus.size(), 512), dry(bus.size(), 512);
        MidiBuffer midi;
        e->ready.store(1, std::memory_order_release);
        uint32_t sequence = 0, controls = 0;
        while (!e->closing.load(std::memory_order_acquire))
        {
            auto requested = e->request.load(std::memory_order_acquire);
            if (requested == sequence)
            {
                Thread::sleep(1);
                continue;
            }
            if (e->controls != controls)
            {
                onPluginMessageThread(
                    [&]
                    {
                        for (size_t i = 0; i < slots.size(); ++i)
                        {
                            auto &slot = slots[i];
                            slot.bypass = (e->bypass & (1u << i)) != 0;
                            check(e->parameterCount[i] >= 0 && e->parameterCount[i] <= 512,
                                  "Hosted control bound exceeded");
                            for (int n = 0; n < e->parameterCount[i]; ++n)
                            {
                                int index = e->parameterIndex[i][n];
                                float x = e->parameterValue[i][n];
                                check(isPositiveAndBelow(index, slot.effect->getParameters().size()) &&
                                          std::isfinite(x) && x >= 0 && x <= 1,
                                      "Invalid hosted control");
                                check(index < slot.parameterIds.size() &&
                                          slot.effect->getParameterID(index) == slot.parameterIds[index],
                                      "Hosted parameter identity changed; reprepare graph");
                                slot.effect->getParameters()[index]->setValueNotifyingHost(x);
                            }
                        }
                    });
                controls = e->controls;
            }
            int frames = e->frames;
            check(frames > 0 && frames <= 512, "Invalid hosted block size");
            for (int c = 0; c < bus.size(); ++c)
                std::copy_n(e->input[c], frames, audio.getWritePointer(c));
            for (auto &slot : slots)
            {
                for (int n = 0; n < frames; ++n)
                {
                    int at = slot.write;
                    for (int c = 0; c < bus.size(); ++c)
                    {
                        slot.dry.setSample(c, at, audio.getSample(c, n));
                        dry.setSample(c, n, slot.dry.getSample(c, (at + 1) % slot.dry.getNumSamples()));
                    }
                    slot.write = (at + 1) % slot.dry.getNumSamples();
                }
                AudioBuffer<float> view(audio.getArrayOfWritePointers(), bus.size(), frames);
                onPluginMessageThread([&] { slot.effect->processBlock(view, midi); });
                midi.clear();
                check(slot.effect->getLatencySamples() == slot.latency &&
                          slot.effect->getBusesLayout() == slot.buses,
                      "Plugin changed bus/latency; stop and explicitly reprepare the graph");
                if (slot.bypass)
                    for (int c = 0; c < bus.size(); ++c)
                        audio.copyFrom(c, 0, dry, c, 0, frames);
            }
            for (int c = 0; c < bus.size(); ++c)
                for (int n = 0; n < frames; ++n)
                {
                    auto sample = audio.getSample(c, n);
                    check(std::isfinite(sample), "Hosted rack produced nonfinite output");
                    e->output[c][n] = sample;
                }
            sequence = requested;
            e->response.store(sequence, std::memory_order_release);
        }
        for (auto &slot : slots)
            onPluginMessageThread([&] { slot.effect->releaseResources(); });
        return obj();
    }
    catch (const std::exception &error)
    {
        std::snprintf(e->reason, sizeof(e->reason), "%s", error.what());
        e->failed.store(1, std::memory_order_release);
        throw;
    }
}
} // namespace
