// ns7midi.cpp
// CoreMIDI-side check for the NS7 dext.
//
//   ns7midi list                  list every MIDI device, entity and endpoint
//   ns7midi monitor [seconds]     print MIDI arriving from the NS7 (default 20 s)
//   ns7midi send <hex bytes...>   send channel voice messages to the NS7,
//                                 e.g.  ns7midi send 90 11 7F
//
// The NS7 is found by its CoreMIDI device name, which the dext copies from
// the USB product string.

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

static const char * const kDeviceName = "Numark USB Audio Device";

static std::string StringProperty(MIDIObjectRef object, CFStringRef property)
{
    CFStringRef value = nullptr;
    if (MIDIObjectGetStringProperty(object, property, &value) != noErr || value == nullptr) return "";
    char buf[256] = {};
    CFStringGetCString(value, buf, sizeof buf, kCFStringEncodingUTF8);
    CFRelease(value);
    return buf;
}

static SInt32 IntProperty(MIDIObjectRef object, CFStringRef property)
{
    SInt32 value = 0;
    MIDIObjectGetIntegerProperty(object, property, &value);
    return value;
}

static void PrintEndpoint(const char * kind, MIDIEndpointRef endpoint)
{
    std::printf("      %s \"%s\"  uniqueID %d  maxSysExSpeed %d\n", kind,
                StringProperty(endpoint, kMIDIPropertyName).c_str(),
                IntProperty(endpoint, kMIDIPropertyUniqueID),
                IntProperty(endpoint, kMIDIPropertyMaxSysExSpeed));
}

static int List()
{
    const ItemCount devices = MIDIGetNumberOfDevices();
    for (ItemCount d = 0; d < devices; d++) {
        MIDIDeviceRef device = MIDIGetDevice(d);
        std::printf("device \"%s\"  manufacturer \"%s\"  model \"%s\"  uniqueID %d%s\n",
                    StringProperty(device, kMIDIPropertyName).c_str(),
                    StringProperty(device, kMIDIPropertyManufacturer).c_str(),
                    StringProperty(device, kMIDIPropertyModel).c_str(),
                    IntProperty(device, kMIDIPropertyUniqueID),
                    IntProperty(device, kMIDIPropertyOffline) ? "  (offline)" : "");
        const ItemCount entities = MIDIDeviceGetNumberOfEntities(device);
        for (ItemCount e = 0; e < entities; e++) {
            MIDIEntityRef entity = MIDIDeviceGetEntity(device, e);
            std::printf("    entity \"%s\"\n", StringProperty(entity, kMIDIPropertyName).c_str());
            for (ItemCount s = 0; s < MIDIEntityGetNumberOfSources(entity); s++)
                PrintEndpoint("source     ", MIDIEntityGetSource(entity, s));
            for (ItemCount s = 0; s < MIDIEntityGetNumberOfDestinations(entity); s++)
                PrintEndpoint("destination", MIDIEntityGetDestination(entity, s));
        }
    }
    return 0;
}

static bool OnNS7(MIDIEndpointRef endpoint)
{
    MIDIEntityRef entity = 0;
    MIDIDeviceRef device = 0;
    if (MIDIEndpointGetEntity(endpoint, &entity) != noErr || entity == 0) return false;
    if (MIDIEntityGetDevice(entity, &device) != noErr || device == 0) return false;
    return StringProperty(device, kMIDIPropertyName) == kDeviceName;
}

static MIDIEndpointRef FindSource()
{
    for (ItemCount i = 0; i < MIDIGetNumberOfSources(); i++)
        if (OnNS7(MIDIGetSource(i))) return MIDIGetSource(i);
    return 0;
}

static MIDIEndpointRef FindDestination()
{
    for (ItemCount i = 0; i < MIDIGetNumberOfDestinations(); i++)
        if (OnNS7(MIDIGetDestination(i))) return MIDIGetDestination(i);
    return 0;
}

static int Monitor(double seconds)
{
    MIDIEndpointRef source = FindSource();
    if (source == 0) {
        std::fprintf(stderr, "no source on a device named \"%s\"\n", kDeviceName);
        return 1;
    }
    MIDIClientRef client = 0;
    MIDIPortRef port = 0;
    MIDIClientCreate(CFSTR("ns7midi"), nullptr, nullptr, &client);
    static std::atomic<unsigned> messages { 0 };
    OSStatus err = MIDIInputPortCreateWithProtocol(client, CFSTR("ns7midi in"), kMIDIProtocol_1_0, &port,
        ^(const MIDIEventList * list, void *) {
            const MIDIEventPacket * packet = &list->packet[0];
            for (UInt32 p = 0; p < list->numPackets; p++) {
                std::printf("UMP:");
                for (UInt32 w = 0; w < packet->wordCount; w++) std::printf(" %08X", packet->words[w]);
                std::printf("\n");
                messages++;
                packet = MIDIEventPacketNext(packet);
            }
        });
    if (err == noErr) err = MIDIPortConnectSource(port, source, nullptr);
    if (err != noErr) {
        std::fprintf(stderr, "input setup failed: %d\n", int(err));
        MIDIClientDispose(client);
        return 1;
    }
    std::printf("Monitoring \"%s\" for %.0f s. Move controls on the NS7.\n",
                StringProperty(source, kMIDIPropertyName).c_str(), seconds);
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, false);
    std::printf("%u packets\n", messages.load());
    MIDIClientDispose(client);
    return 0;
}

// Parses one hex byte token strictly: no empty token, no leading sign, no
// trailing garbage, and no value over 0xFF.
static bool ParseHexByte(const char * token, uint8_t * out)
{
    if (token[0] == '\0' || token[0] == '-' || token[0] == '+') return false;
    char * end = nullptr;
    const unsigned long value = std::strtoul(token, &end, 16);
    if (end == token || *end != '\0' || value > 0xFF) return false;
    *out = uint8_t(value);
    return true;
}

// Parses hex bytes into MIDI 1.0 channel voice messages as UMP type-2 words.
static bool ParseChannelVoice(int argc, const char * argv[], std::vector<UInt32> * words)
{
    std::vector<uint8_t> bytes;
    for (int i = 0; i < argc; i++) {
        uint8_t byte = 0;
        if (!ParseHexByte(argv[i], &byte)) return false;
        bytes.push_back(byte);
    }
    for (size_t i = 0; i < bytes.size(); ) {
        const uint8_t status = bytes[i];
        if (status < 0x80 || status >= 0xF0) return false;
        const size_t data = ((status & 0xE0) == 0xC0) ? 1 : 2;
        if (i + 1 + data > bytes.size()) return false;
        const uint8_t d0 = bytes[i + 1], d1 = data == 2 ? bytes[i + 2] : 0;
        words->push_back(0x20000000u | UInt32(status) << 16 | UInt32(d0) << 8 | d1);
        i += 1 + data;
    }
    return !words->empty();
}

static int Send(int argc, const char * argv[])
{
    std::vector<UInt32> words;
    if (!ParseChannelVoice(argc, argv, &words)) {
        std::fprintf(stderr, "expected channel voice messages in hex, e.g. 90 11 7F\n");
        return 2;
    }
    MIDIEndpointRef destination = FindDestination();
    if (destination == 0) {
        std::fprintf(stderr, "no destination on a device named \"%s\"\n", kDeviceName);
        return 1;
    }
    MIDIClientRef client = 0;
    MIDIPortRef port = 0;
    MIDIClientCreate(CFSTR("ns7midi"), nullptr, nullptr, &client);
    MIDIOutputPortCreate(client, CFSTR("ns7midi out"), &port);

    Byte storage[1024];
    auto * list = reinterpret_cast<MIDIEventList *>(storage);
    MIDIEventPacket * packet = MIDIEventListInit(list, kMIDIProtocol_1_0);
    for (UInt32 w : words) {
        MIDIEventPacket * next = MIDIEventListAdd(list, sizeof storage, packet, 0, 1, &w);
        if (next == nullptr) {
            std::fprintf(stderr, "too many messages for one event list\n");
            MIDIClientDispose(client);
            return 1;
        }
        packet = next;
    }
    const OSStatus err = MIDISendEventList(port, destination, list);
    std::printf("sent %zu message(s): %s\n", words.size(), err == noErr ? "ok" : "failed");
    usleep(200 * 1000);   // let CoreMIDI deliver before the client goes away
    MIDIClientDispose(client);
    return err == noErr ? 0 : 1;
}

int main(int argc, const char * argv[])
{
    if (argc >= 2 && std::strcmp(argv[1], "list") == 0) return List();
    if (argc >= 2 && std::strcmp(argv[1], "monitor") == 0)
        return Monitor(argc >= 3 ? std::atof(argv[2]) : 20.0);
    if (argc >= 3 && std::strcmp(argv[1], "send") == 0) return Send(argc - 2, argv + 2);
    std::fprintf(stderr, "usage: %s list | monitor [seconds] | send <hex bytes...>\n", argv[0]);
    return 2;
}
