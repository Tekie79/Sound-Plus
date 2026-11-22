#include <juce_core/juce_core.h>
#include <CoreMIDI/CoreMIDI.h>
#include <mach/mach_time.h>
#include <iostream>
int main(int argc, char **argv)
{
    if (argc != 3)
        return 2;
    juce::File control(argv[1]), report(argv[2]);
    MIDIClientRef client = 0;
    MIDIEndpointRef source = 0;
    if (MIDIClientCreate(CFSTR("SoundPlus owned QA MIDI sender"), nullptr, nullptr, &client) != noErr ||
        MIDISourceCreate(client, CFSTR("SoundPlus QA MIDI"), &source) != noErr)
        return 3;
    SInt32 uid = 0;
    MIDIObjectGetIntegerProperty(source, kMIDIPropertyUniqueID, &uid);
    uint64_t sent = 0;
    auto publish = [&](bool active)
    {
        auto v = juce::var(new juce::DynamicObject);
        v.getDynamicObject()->setProperty("uid", juce::String(uid));
        v.getDynamicObject()->setProperty("sent", (juce::int64)sent);
        v.getDynamicObject()->setProperty("active", active);
        report.replaceWithText(juce::JSON::toString(v, true));
    };
    auto send = [&](uint8_t status, uint8_t a, uint8_t b)
    {
        alignas(MIDIPacketList) std::array<uint8_t, 256> bytes{};
        auto *list = reinterpret_cast<MIDIPacketList *>(bytes.data());
        auto *packet = MIDIPacketListInit(list);
        uint8_t data[]{status, a, b};
        packet = MIDIPacketListAdd(list, bytes.size(), packet, mach_absolute_time(), 3, data);
        if (packet && MIDIReceived(source, list) == noErr)
            ++sent;
    };
    bool held = false;
    double began = juce::Time::getMillisecondCounterHiRes(), next = began;
    publish(true);
    for (;;)
    {
        auto c = juce::JSON::parse(control);
        auto now = juce::Time::getMillisecondCounterHiRes();
        if ((bool)c["quit"] || now - began > 600000)
            break;
        bool run = (bool)c["run"];
        if ((run && now >= next) || (!run && held))
        {
            for (int p : {60, 64, 67})
                send(held ? 0x80 : 0x90, (uint8_t)p, held ? 64 : 90);
            held = !held;
            next = now + 250;
            publish(true);
        }
        juce::Thread::sleep(10);
    }
    for (int ch = 0; ch < 16; ++ch)
    {
        send(0xb0 | ch, 64, 0);
        send(0xb0 | ch, 123, 0);
    }
    publish(false);
    MIDIEndpointDispose(source);
    MIDIClientDispose(client);
    std::cout << "Owned CoreMIDI virtual sender completed " << sent << " packets\n";
}
