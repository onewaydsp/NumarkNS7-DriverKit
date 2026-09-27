// midi_io.cpp
// CoreMIDI, replay and dry-run implementations of MidiInput / MidiOutput,
// plus the list / monitor / send commands.

#include "midi_io.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

const char * const kDeviceName = "Numark USB Audio Device";

void SleepMs(int ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// ------------------------------------------------------------------ helpers

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

static void PrintEndpoint(const char * kind, MIDIEndpointRef endpoint)
{
    std::printf("      %s \"%s\"  uniqueID %d  maxSysExSpeed %d\n", kind,
                StringProperty(endpoint, kMIDIPropertyName).c_str(),
                IntProperty(endpoint, kMIDIPropertyUniqueID),
                IntProperty(endpoint, kMIDIPropertyMaxSysExSpeed));
}

// --------------------------------------------------------------- replay in

namespace {

class ReplayInput : public MidiInput {
public:
    explicit ReplayInput(std::vector<std::vector<MidiMsg>> blocks) : blocks_(std::move(blocks)) {}

    bool BeginStep() override
    {
        if (next_ >= blocks_.size()) { pending_.clear(); return false; }
        pending_ = blocks_[next_++];
        for (size_t i = 0; i < pending_.size(); i++) pending_[i].time = 0.001 * double(i);
        return true;
    }

    std::vector<MidiMsg> Drain() override
    {
        std::vector<MidiMsg> out;
        out.swap(pending_);
        return out;
    }

    bool StepComplete() const override { return pending_.empty(); }
    bool IsReplay() const override { return true; }

private:
    std::vector<std::vector<MidiMsg>> blocks_;
    size_t next_ = 0;
    std::vector<MidiMsg> pending_;
};

class DryRunOutput : public MidiOutput {
public:
    bool Send(const MidiMsg & msg) override
    {
        std::printf("    [dry-run] %s (not sent)\n", FormatMsg(msg).c_str());
        std::fflush(stdout);
        return true;
    }
    bool IsDryRun() const override { return true; }
};

// ---------------------------------------------------------------- live in

using Clock = std::chrono::steady_clock;

class LiveInput : public MidiInput {
public:
    bool BeginStep() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        start_ = Clock::now();
        return true;
    }

    std::vector<MidiMsg> Drain() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<MidiMsg> out;
        out.swap(queue_);
        return out;
    }

    bool StepComplete() const override { return false; }
    bool IsReplay() const override { return false; }

    // Called on the CoreMIDI thread with MIDI 1.0 UMP words (type 2).
    void Receive(const MIDIEventList * list)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const double now = std::chrono::duration<double>(Clock::now() - start_).count();
        // UMP sizes by message type: 0-2 one word, 3-4 two words, 5 four.
        static const UInt32 kWords[16] = { 1, 1, 1, 2, 2, 4, 1, 1, 2, 2, 2, 3, 3, 4, 4, 4 };
        const MIDIEventPacket * packet = &list->packet[0];
        for (UInt32 p = 0; p < list->numPackets; p++) {
            for (UInt32 w = 0; w < packet->wordCount; w += kWords[packet->words[w] >> 28]) {
                const UInt32 word = packet->words[w];
                if ((word >> 28) == 0x2) {
                    MidiMsg msg;
                    msg.status = uint8_t(word >> 16);
                    msg.data1 = uint8_t(word >> 8) & 0x7F;
                    msg.data2 = uint8_t(word) & 0x7F;
                    msg.time = now;
                    if (ChannelVoiceDataBytes(msg.status) > 0 && queue_.size() < 200000) queue_.push_back(msg);
                }
            }
            packet = MIDIEventPacketNext(packet);
        }
    }

private:
    std::mutex mutex_;
    std::vector<MidiMsg> queue_;
    Clock::time_point start_ = Clock::now();
};

class LiveOutput : public MidiOutput {
public:
    LiveOutput(MIDIPortRef port, MIDIEndpointRef destination) : port_(port), destination_(destination) {}

    bool Send(const MidiMsg & msg) override
    {
        const int data = ChannelVoiceDataBytes(msg.status);
        if (data < 0) {
            std::printf("    [send] refused: not a channel voice status\n");
            return false;
        }
        UInt32 word = 0x20000000u | UInt32(msg.status) << 16 | UInt32(msg.data1 & 0x7F) << 8 |
                      (data == 2 ? UInt32(msg.data2 & 0x7F) : 0);
        Byte storage[256];
        auto * list = reinterpret_cast<MIDIEventList *>(storage);
        MIDIEventPacket * packet = MIDIEventListInit(list, kMIDIProtocol_1_0);
        packet = MIDIEventListAdd(list, sizeof storage, packet, 0, 1, &word);
        const OSStatus err = packet ? MIDISendEventList(port_, destination_, list) : OSStatus(-1);
        std::printf("    [send] %s%s\n", FormatMsg(msg).c_str(),
                    err == noErr ? "" : ("  FAILED " + std::to_string(int(err))).c_str());
        std::fflush(stdout);
        return err == noErr;
    }
    bool IsDryRun() const override { return false; }

private:
    MIDIPortRef port_;
    MIDIEndpointRef destination_;
};

}   // namespace

std::unique_ptr<MidiInput> OpenReplayInput(const std::string & path, std::string * error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) { *error = path + ": cannot open"; return nullptr; }
    std::stringstream text;
    text << in.rdbuf();
    std::vector<std::vector<MidiMsg>> blocks;
    if (!ParseReplay(text.str(), &blocks, error)) { *error = path + ": " + *error; return nullptr; }
    return std::make_unique<ReplayInput>(std::move(blocks));
}

std::unique_ptr<MidiOutput> MakeDryRunOutput()
{
    return std::make_unique<DryRunOutput>();
}

// ------------------------------------------------------------ the session

CoreMidiSession::~CoreMidiSession()
{
    // Give CoreMIDI a moment to deliver the last sends before the client goes.
    if (output_) SleepMs(200);
    output_.reset();
    if (client_) MIDIClientDispose(client_);
    input_.reset();
}

bool CoreMidiSession::Open(bool wantInput, bool wantOutput, std::string * error)
{
    const MIDIEndpointRef source = FindSource();
    const MIDIEndpointRef destination = FindDestination();
    // An input connection keeps MIDIServer and the driver IO running even
    // when only sending, so connect the source whenever it exists.
    if (wantInput && source == 0) { *error = std::string("no source on a device named \"") + kDeviceName + "\""; return false; }
    if (wantOutput && destination == 0) { *error = std::string("no destination on a device named \"") + kDeviceName + "\""; return false; }

    MIDIClientRef client = 0;
    OSStatus err = MIDIClientCreate(CFSTR("ns7midi"), nullptr, nullptr, &client);
    if (err != noErr) { *error = "MIDIClientCreate failed: " + std::to_string(int(err)); return false; }
    client_ = client;

    if (source != 0) {
        auto input = std::make_unique<LiveInput>();
        LiveInput * raw = input.get();
        MIDIPortRef inPort = 0;
        err = MIDIInputPortCreateWithProtocol(client, CFSTR("ns7midi in"), kMIDIProtocol_1_0, &inPort,
            ^(const MIDIEventList * list, void *) { raw->Receive(list); });
        if (err == noErr) err = MIDIPortConnectSource(inPort, source, nullptr);
        if (err != noErr) { *error = "input setup failed: " + std::to_string(int(err)); return false; }
        input_ = std::move(input);
    }
    if (wantOutput) {
        MIDIPortRef outPort = 0;
        err = MIDIOutputPortCreate(client, CFSTR("ns7midi out"), &outPort);
        if (err != noErr) { *error = "output setup failed: " + std::to_string(int(err)); return false; }
        output_ = std::make_unique<LiveOutput>(outPort, destination);
        SleepMs(500);   // let MIDIServer start the device IO before the first send
    }
    return true;
}

// ---------------------------------------------------------------- commands

int ListDevices()
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

int MonitorCommand(double seconds, const std::string & recordPath)
{
    CoreMidiSession session;
    std::string error;
    if (!session.Open(true, false, &error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
    std::ofstream record;
    if (!recordPath.empty()) {
        record.open(recordPath, std::ios::trunc);
        if (!record) { std::fprintf(stderr, "%s: cannot create\n", recordPath.c_str()); return 1; }
        record << "# ns7midi monitor recording (replay format: one message per line, --- ends a block)\n";
    }
    MidiInput * input = session.input();
    input->BeginStep();
    std::printf("Monitoring \"%s\" for %.0f s. Move controls on the NS7.\n", kDeviceName, seconds);
    std::fflush(stdout);
    size_t count = 0;
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < end) {
        SleepMs(20);
        for (const MidiMsg & msg : input->Drain()) {
            count++;
            std::printf("%9.3f  %s\n", msg.time, FormatMsg(msg).c_str());
            if (record) record << FormatMsg(msg) << "\n";
        }
        std::fflush(stdout);
    }
    std::printf("%zu message(s)\n", count);
    return 0;
}

int SendCommand(const std::vector<std::string> & hexTokens)
{
    std::vector<MidiMsg> msgs;
    if (!ParseChannelVoiceBytes(hexTokens, &msgs)) {
        std::fprintf(stderr, "expected channel voice messages in hex, e.g. 90 11 7F\n");
        return 2;
    }
    CoreMidiSession session;
    std::string error;
    if (!session.Open(false, true, &error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
    bool ok = true;
    for (const MidiMsg & msg : msgs) ok = session.output()->Send(msg) && ok;
    std::printf("sent %zu message(s): %s\n", msgs.size(), ok ? "ok" : "failed");
    return ok ? 0 : 1;
}
