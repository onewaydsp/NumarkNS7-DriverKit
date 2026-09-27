// NumarkNS7Device.cpp
// USB root service for the Numark NS7. See NumarkNS7Device.iig.
//
// Startup follows the Ploytec kext (docs/PROTOCOL.md): select alt 1 on both
// interfaces, wait kSettleMs, run the control handshake, then start the audio
// pipes and the MIDI reads. This sequence was first proven on hardware with
// Tools/ns7probe (`init --send --stream`), which this file mirrors.

#include <os/log.h>
#include <stdio.h>
#include <string.h>

#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/OSAction.h>
#include <DriverKit/OSCollections.h>
#include <USBDriverKit/IOUSBHostDevice.h>
#include <USBDriverKit/IOUSBHostFamilyDefinitions.h>
#include <USBDriverKit/IOUSBHostInterface.h>
#include <USBDriverKit/IOUSBHostPipe.h>
#include <USBDriverKit/USBDriverKitDefs.h>

#include "NS7MIDIDriver.h"
#include "NS7Protocol.h"
#include "NumarkNS7Device.h"

#define Log(fmt, ...) os_log(OS_LOG_DEFAULT, "NumarkNS7: " fmt, ##__VA_ARGS__)

namespace {

constexpr uint8_t kStreamingAltSetting = 1;
constexpr uint8_t kNumInterfaces       = 2;

// Every endpoint on alt setting 1 of IF0 and IF1.
constexpr uint8_t kEndpoints[] = {
    NS7::kEPPlaybackOut, NS7::kEPMidiIn, NS7::kEPMidiOut, NS7::kEPFeedbackIn, NS7::kEPCaptureIn,
};
constexpr size_t  kNumEndpoints = sizeof(kEndpoints) / sizeof(kEndpoints[0]);
enum { kPipePlayback, kPipeMidiIn, kPipeMidiOut, kPipeFeedback, kPipeCapture };

// Requests kept in flight. The kext uses 3 playback and 2 feedback requests;
// on hardware, 2 feedback requests were not enough from user space: once a
// feedback request came back too old, the NS7 stalled its bulk IN endpoints.
constexpr uint32_t kPlaybackInFlight    = 4;
constexpr uint32_t kPlaybackMicroframes = 32;                 // 4 ms per request
constexpr uint32_t kPlaybackMaxPacket   = 6 * NS7::kPlaybackFrameBytes;
constexpr uint32_t kFeedbackInFlight    = 8;
constexpr uint32_t kFeedbackFrames      = 4;                  // one packet per ms
constexpr uint32_t kFeedbackMaxPacket   = 64;
constexpr uint32_t kCaptureInFlight     = 3;
constexpr uint32_t kCaptureBytes        = 20 * 512;           // 160 frames
constexpr uint32_t kMidiInFlight        = 16;
constexpr uint64_t kIsoLeadFrames       = 10;                 // ms ahead of the bus
constexpr uint32_t kControlTimeoutMs    = 5000;
constexpr uint64_t kStatsEveryFeedback  = 10000;              // ~10 s
constexpr uint32_t kLoggedErrorsPerPipe = 5;
constexpr uint32_t kLoggedMidiPerStats  = 100;                // MIDI lines per stats period

struct SlotRef { uint32_t index; };

struct IsoSlot {
    IOBufferMemoryDescriptor * data;
    IOBufferMemoryDescriptor * frames;
    uint8_t                  * dataPtr;
    IOUSBIsochronousFrame    * framePtr;
    OSAction                 * action;
    uint64_t                   firstFrame;
};

struct BulkSlot {
    IOBufferMemoryDescriptor * buffer;
    uint8_t                  * ptr;
    OSAction                 * action;
};

struct PipeStats { uint64_t done, errors, resyncs, stalls; };

} // namespace

struct NumarkNS7Device_IVars
{
    IOUSBHostDevice    * device = nullptr;
    IOUSBHostInterface * interfaces[kNumInterfaces] = {};
    IOUSBHostPipe      * pipes[kNumEndpoints] = {};

    IOBufferMemoryDescriptor * control = nullptr;
    uint8_t                  * controlPtr = nullptr;

    IsoSlot  playback[kPlaybackInFlight] = {};
    IsoSlot  feedback[kFeedbackInFlight] = {};
    BulkSlot capture[kCaptureInFlight] = {};
    BulkSlot midiIn[kMidiInFlight] = {};

    uint64_t playbackFrame = 0;     // next frame for a playback request
    uint64_t feedbackFrame = 0;
    uint32_t playbackSlot  = 0;     // running microframe counter for the 5/6 pattern
    int      feedbackFrames = -1;   // latest frames/ms from EP 0x81

    bool     stopping = false;

    NS7::RawMidiToUmp midiParser;

    PipeStats stats[kNumEndpoints] = {};
    uint64_t  feedbackPackets = 0;
    uint64_t  captureBytes = 0;
    uint64_t  midiMessages = 0;
    uint32_t  midiLogged = 0;       // reset with each stats line
    IOLock        * midiClientLock = nullptr;
    NS7MIDIDriver * midiClient = nullptr;       // guarded by midiClientLock, retained
    NS7MIDIDriver * midiInClient = nullptr;     // set only while MidiInComplete parses
};

bool
NumarkNS7Device::init()
{
    if (!super::init()) return false;
    // IONewZero zero-fills without running constructors: every member must be
    // valid as zero except the ones set here.
    ivars = IONewZero(NumarkNS7Device_IVars, 1);
    if (ivars == nullptr) return false;
    ivars->feedbackFrames = -1;
    ivars->midiClientLock = IOLockAlloc();
    if (ivars->midiClientLock == nullptr) return false;
    return true;
}

static void
ReleaseIsoSlots(IsoSlot * slots, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        OSSafeReleaseNULL(slots[i].data);
        OSSafeReleaseNULL(slots[i].frames);
    }
}

static void
ReleaseBulkSlots(BulkSlot * slots, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) OSSafeReleaseNULL(slots[i].buffer);
}

void
NumarkNS7Device::free()
{
    // Buffers outlive Stop() so an aborted transfer never lands in freed memory.
    if (ivars) {
        ReleaseIsoSlots(ivars->playback, kPlaybackInFlight);
        ReleaseIsoSlots(ivars->feedback, kFeedbackInFlight);
        ReleaseBulkSlots(ivars->capture, kCaptureInFlight);
        ReleaseBulkSlots(ivars->midiIn, kMidiInFlight);
        OSSafeReleaseNULL(ivars->control);
        OSSafeReleaseNULL(ivars->midiClient);
        if (ivars->midiClientLock) IOLockFree(ivars->midiClientLock);
    }
    IOSafeDeleteNULL(ivars, NumarkNS7Device_IVars, 1);
    super::free();
}

static void
LogConfigurationDescriptor(IOUSBHostDevice * device)
{
    const IOUSBConfigurationDescriptor * config = device->CopyConfigurationDescriptor(static_cast<uint8_t>(0));
    if (config == nullptr) return;

    const uint8_t * bytes = reinterpret_cast<const uint8_t *>(config);
    const uint16_t total  = USBToHost16(config->wTotalLength);
    for (uint16_t i = 0; i + 1 < total && bytes[i] != 0; i += bytes[i]) {
        const uint8_t len = bytes[i];
        if (i + len > total) break;
        char line[3 * 32 + 1] = {};
        for (uint8_t j = 0; j < len && j < 32; j++) {
            snprintf(line + 3 * j, 4, "%02x ", bytes[i + j]);
        }
        Log("desc[%02x] %{public}s", bytes[i + 1], line);
    }
    IOUSBHostFreeDescriptor(config);
}

// ── Buffers ──────────────────────────────────────────────────────────────────

static kern_return_t
CreateBuffer(IOUSBHostInterface * interface, uint32_t size,
             IOBufferMemoryDescriptor ** buffer, uint8_t ** ptr)
{
    kern_return_t ret = interface->CreateIOBuffer(kIOMemoryDirectionInOut, size, buffer);
    if (ret != kIOReturnSuccess) return ret;
    (*buffer)->SetLength(size);
    IOAddressSegment range = {};
    ret = (*buffer)->GetAddressRange(&range);
    if (ret != kIOReturnSuccess) return ret;
    *ptr = reinterpret_cast<uint8_t *>(range.address);
    memset(*ptr, 0, size);
    return kIOReturnSuccess;
}

static kern_return_t
CreateIsoSlot(IOUSBHostInterface * interface, uint32_t dataBytes, uint32_t frames, IsoSlot * slot)
{
    uint8_t * framePtr = nullptr;
    kern_return_t ret = CreateBuffer(interface, dataBytes, &slot->data, &slot->dataPtr);
    if (ret == kIOReturnSuccess)
        ret = CreateBuffer(interface, frames * sizeof(IOUSBIsochronousFrame), &slot->frames, &framePtr);
    slot->framePtr = reinterpret_cast<IOUSBIsochronousFrame *>(framePtr);
    return ret;
}

static void
SetSlotIndex(OSAction * action, uint32_t index)
{
    static_cast<SlotRef *>(action->GetReference())->index = index;
}

static uint32_t
SlotIndex(OSAction * action)
{
    return static_cast<const SlotRef *>(action->GetReference())->index;
}

// ── Control handshake ────────────────────────────────────────────────────────

// Sends one control request through the device. `data` is copied out for OUT
// requests and filled in for IN requests; it may be null when wLength is 0.
static kern_return_t
ControlRequest(NumarkNS7Device_IVars * iv, IOService * client, uint8_t bmRequestType,
               uint8_t bRequest, uint16_t wValue, uint16_t wIndex, uint16_t wLength,
               uint8_t * data, uint16_t * received = nullptr)
{
    const bool in = bmRequestType & 0x80;
    if (wLength > 8) return kIOReturnBadArgument;
    if (!in && wLength) memcpy(iv->controlPtr, data, wLength);
    if (in && wLength) memset(iv->controlPtr, 0, wLength);

    uint16_t got = 0;
    kern_return_t ret = iv->device->DeviceRequest(client, bmRequestType, bRequest, wValue, wIndex,
                                                  wLength, wLength ? iv->control : nullptr, &got,
                                                  kControlTimeoutMs);
    if (ret != kIOReturnSuccess) {
        Log("control %02x %02x %04x %04x %u failed: 0x%08x",
            bmRequestType, bRequest, wValue, wIndex, wLength, ret);
        return ret;
    }
    if (in && wLength) memcpy(data, iv->controlPtr, got < wLength ? got : wLength);
    if (received) *received = got;
    return kIOReturnSuccess;
}

static kern_return_t
ReadReg0(NumarkNS7Device_IVars * iv, IOService * client, uint8_t * value)
{
    return ControlRequest(iv, client, 0xC0, NS7::kReqRegister, 0, 0, 1, value);
}

static kern_return_t
WriteReg0(NumarkNS7Device_IVars * iv, IOService * client, uint8_t value)
{
    return ControlRequest(iv, client, 0x40, NS7::kReqRegister, value, 0, 0, nullptr);
}

static kern_return_t
GetRate(NumarkNS7Device_IVars * iv, IOService * client, uint16_t wIndex, uint32_t * hz)
{
    uint8_t b[3] = {};
    kern_return_t ret = ControlRequest(iv, client, 0xA2, NS7::kUacGetCur, NS7::kUacSamplingFreq,
                                       wIndex, 3, b);
    *hz = NS7::DecodeSampleRate(b);
    return ret;
}

static kern_return_t
SetRate(NumarkNS7Device_IVars * iv, IOService * client, uint8_t endpoint)
{
    uint8_t b[3];
    NS7::EncodeSampleRate(NS7::kSampleRate, b);
    return ControlRequest(iv, client, 0x22, NS7::kUacSetCur, NS7::kUacSamplingFreq, endpoint, 3, b);
}

// The kext's startup (AJ::updateAjInputSelector, then startStreaming /
// bulkAudioRun): select the internal clock, set 44.1 kHz on EP 0x86 and 0x02,
// then set the stream-enable bits in register 0.
static kern_return_t
Handshake(NumarkNS7Device_IVars * iv, IOService * client)
{
    uint8_t info[8] = {};
    uint16_t got = 0;
    kern_return_t ret = ControlRequest(iv, client, 0xC0, NS7::kReqFirmwareInfo, 0, 0, 8, info, &got);
    if (ret != kIOReturnSuccess) return ret;
    Log("firmware info (%u bytes): %02x %02x %02x %02x %02x", got,
        info[0], info[1], info[2], info[3], info[4]);
    ret = ControlRequest(iv, client, 0xC0, NS7::kReqFirmwareInfo, 0, 0, 5, info);
    if (ret != kIOReturnSuccess) return ret;

    uint8_t reg0 = 0;
    if ((ret = ReadReg0(iv, client, &reg0)) != kIOReturnSuccess) return ret;
    if (reg0 & NS7::kReg0Busy) {
        IOSleep(20);
        if ((ret = ReadReg0(iv, client, &reg0)) != kIOReturnSuccess) return ret;
    }
    uint32_t hz = 0;
    const uint8_t withClock = NS7::Reg0WithInternalClock(reg0);
    if (withClock != reg0) {
        if ((ret = WriteReg0(iv, client, withClock)) != kIOReturnSuccess) return ret;
        IOSleep(40);
        GetRate(iv, client, 0, &hz);
        if ((ret = ReadReg0(iv, client, &reg0)) != kIOReturnSuccess) return ret;
    }
    if ((ret = GetRate(iv, client, 0, &hz)) != kIOReturnSuccess) return ret;
    if (hz != NS7::kSampleRate) Log("device rate %u Hz, expected %u", hz, NS7::kSampleRate);

    if ((ret = SetRate(iv, client, NS7::kEPCaptureIn)) != kIOReturnSuccess) return ret;
    if ((ret = SetRate(iv, client, NS7::kEPPlaybackOut)) != kIOReturnSuccess) return ret;
    if ((ret = GetRate(iv, client, NS7::kEPCaptureIn, &hz)) != kIOReturnSuccess) return ret;

    IOSleep(50);
    if ((ret = ReadReg0(iv, client, &reg0)) != kIOReturnSuccess) return ret;
    const uint8_t enabled = NS7::Reg0WithStreamEnable(reg0, NS7::kChannels);
    if ((ret = WriteReg0(iv, client, enabled)) != kIOReturnSuccess) return ret;

    Log("handshake done: rate %u Hz, reg0 0x%02x -> 0x%02x", hz, reg0, enabled);
    return kIOReturnSuccess;
}

// ── Streaming ────────────────────────────────────────────────────────────────

static void
NoteError(NumarkNS7Device_IVars * iv, int pipe, IOReturn status)
{
    if (iv->stats[pipe].errors++ < kLoggedErrorsPerPipe)
        Log("EP 0x%02x error 0x%08x", kEndpoints[pipe], status);
}

static bool
TransferOk(IOReturn status)
{
    return status == kIOReturnSuccess || status == kIOReturnUnderrun;
}

static uint64_t
NextFrame(NumarkNS7Device_IVars * iv, int pipe, uint64_t planned)
{
    uint64_t now = 0;
    if (iv->interfaces[0]->GetFrameNumber(&now, nullptr) != kIOReturnSuccess) return planned;
    const uint64_t frame = NS7::NextIsoFrame(planned, now, kIsoLeadFrames);
    if (frame != planned && planned != 0) iv->stats[pipe].resyncs++;
    return frame;
}

static kern_return_t
SubmitPlayback(NumarkNS7Device_IVars * iv, uint32_t i)
{
    IsoSlot & slot = iv->playback[i];
    uint32_t sizes[kPlaybackMicroframes];
    NS7::FillPlaybackPacketSizes(iv->feedbackFrames, &iv->playbackSlot, kPlaybackMicroframes, sizes);
    for (uint32_t k = 0; k < kPlaybackMicroframes; k++)
        slot.framePtr[k] = { kIOReturnInvalid, sizes[k], 0, 0, 0 };
    // The data is silence: nothing writes the buffers yet.

    slot.firstFrame = NextFrame(iv, kPipePlayback, iv->playbackFrame);
    iv->playbackFrame = slot.firstFrame + kPlaybackMicroframes / 8;
    kern_return_t ret = iv->pipes[kPipePlayback]->IsochIO(slot.data, slot.frames, slot.firstFrame,
                                                         slot.action);
    if (ret != kIOReturnSuccess) NoteError(iv, kPipePlayback, ret);
    return ret;
}

static kern_return_t
SubmitFeedback(NumarkNS7Device_IVars * iv, uint32_t i)
{
    IsoSlot & slot = iv->feedback[i];
    for (uint32_t k = 0; k < kFeedbackFrames; k++)
        slot.framePtr[k] = { kIOReturnInvalid, kFeedbackMaxPacket, 0, 0, 0 };

    slot.firstFrame = NextFrame(iv, kPipeFeedback, iv->feedbackFrame);
    iv->feedbackFrame = slot.firstFrame + kFeedbackFrames;
    kern_return_t ret = iv->pipes[kPipeFeedback]->IsochIO(slot.data, slot.frames, slot.firstFrame,
                                                         slot.action);
    if (ret != kIOReturnSuccess) NoteError(iv, kPipeFeedback, ret);
    return ret;
}

static kern_return_t
SubmitBulk(NumarkNS7Device_IVars * iv, int pipe, BulkSlot & slot, uint32_t length)
{
    kern_return_t ret = iv->pipes[pipe]->AsyncIO(slot.buffer, length, slot.action, 0);
    if (ret != kIOReturnSuccess) NoteError(iv, pipe, ret);
    return ret;
}

// A STALL on a bulk IN is recoverable: clear it and queue the read again.
static void
RecoverBulk(NumarkNS7Device_IVars * iv, int pipe, BulkSlot & slot, uint32_t length)
{
    iv->stats[pipe].stalls++;
    kern_return_t ret = iv->pipes[pipe]->ClearStall(true);
    if (ret != kIOReturnSuccess) {
        Log("EP 0x%02x ClearStall failed: 0x%08x", kEndpoints[pipe], ret);
        return;
    }
    SubmitBulk(iv, pipe, slot, length);
}

static void
LogStats(NumarkNS7Device_IVars * iv)
{
    const PipeStats & p = iv->stats[kPipePlayback], & f = iv->stats[kPipeFeedback],
                    & c = iv->stats[kPipeCapture],  & m = iv->stats[kPipeMidiIn];
    Log("stats: playback %llu (%llu err, %llu resync) | feedback %llu pkts, %d frames/ms "
        "(%llu err, %llu resync) | capture %llu B (%llu err, %llu stall) | "
        "MIDI %llu xfers, %llu msgs (%llu err, %llu stall)",
        p.done, p.errors, p.resyncs, iv->feedbackPackets, iv->feedbackFrames, f.errors, f.resyncs,
        iv->captureBytes, c.errors, c.stalls, m.done, iv->midiMessages, m.errors, m.stalls);
    if (iv->midiLogged > kLoggedMidiPerStats)
        Log("%u MIDI messages not logged", iv->midiLogged - kLoggedMidiPerStats);
    iv->midiLogged = 0;
}

// ── MIDI client ──────────────────────────────────────────────────────────────

// Retained copy of the registered MIDI client, or nullptr.
static NS7MIDIDriver *
CopyMidiClient(NumarkNS7Device_IVars * iv)
{
    IOLockLock(iv->midiClientLock);
    NS7MIDIDriver * client = iv->midiClient;
    if (client) client->retain();
    IOLockUnlock(iv->midiClientLock);
    return client;
}

// Called by the raw MIDI parser for each complete UMP.
static void
OnMidiInUmp(void * ctx, const uint32_t * words, size_t count)
{
    auto * iv = static_cast<NumarkNS7Device_IVars *>(ctx);
    iv->midiMessages++;
    if (iv->midiInClient) iv->midiInClient->DeliverMidiIn(words, uint32_t(count));
    if (iv->midiLogged++ >= kLoggedMidiPerStats) return;
    if (count == 1) Log("MIDI in %08x", words[0]);
    else            Log("MIDI in %08x %08x", words[0], words[1]);
}

static kern_return_t
StartStreaming(NumarkNS7Device * self, NumarkNS7Device_IVars * iv)
{
    IOUSBHostInterface * if0 = iv->interfaces[0], * if1 = iv->interfaces[1];
    kern_return_t ret = kIOReturnSuccess;

    for (uint32_t i = 0; i < kPlaybackInFlight && ret == kIOReturnSuccess; i++) {
        ret = CreateIsoSlot(if0, kPlaybackMicroframes * kPlaybackMaxPacket, kPlaybackMicroframes,
                            &iv->playback[i]);
        if (ret == kIOReturnSuccess)
            ret = self->CreateActionPlaybackComplete(sizeof(SlotRef), &iv->playback[i].action);
        if (ret == kIOReturnSuccess) SetSlotIndex(iv->playback[i].action, i);
    }
    for (uint32_t i = 0; i < kFeedbackInFlight && ret == kIOReturnSuccess; i++) {
        ret = CreateIsoSlot(if1, kFeedbackFrames * kFeedbackMaxPacket, kFeedbackFrames,
                            &iv->feedback[i]);
        if (ret == kIOReturnSuccess)
            ret = self->CreateActionFeedbackComplete(sizeof(SlotRef), &iv->feedback[i].action);
        if (ret == kIOReturnSuccess) SetSlotIndex(iv->feedback[i].action, i);
    }
    for (uint32_t i = 0; i < kCaptureInFlight && ret == kIOReturnSuccess; i++) {
        ret = CreateBuffer(if1, kCaptureBytes, &iv->capture[i].buffer, &iv->capture[i].ptr);
        if (ret == kIOReturnSuccess)
            ret = self->CreateActionCaptureComplete(sizeof(SlotRef), &iv->capture[i].action);
        if (ret == kIOReturnSuccess) SetSlotIndex(iv->capture[i].action, i);
    }
    for (uint32_t i = 0; i < kMidiInFlight && ret == kIOReturnSuccess; i++) {
        ret = CreateBuffer(if0, NS7::kMidiPacketBytes, &iv->midiIn[i].buffer, &iv->midiIn[i].ptr);
        if (ret == kIOReturnSuccess)
            ret = self->CreateActionMidiInComplete(sizeof(SlotRef), &iv->midiIn[i].action);
        if (ret == kIOReturnSuccess) SetSlotIndex(iv->midiIn[i].action, i);
    }
    if (ret != kIOReturnSuccess) {
        Log("streaming setup failed: 0x%08x", ret);
        return ret;
    }

    // Feedback first so the playback pattern follows the device from the start.
    for (uint32_t i = 0; i < kFeedbackInFlight && ret == kIOReturnSuccess; i++)
        ret = SubmitFeedback(iv, i);
    for (uint32_t i = 0; i < kPlaybackInFlight && ret == kIOReturnSuccess; i++)
        ret = SubmitPlayback(iv, i);
    for (uint32_t i = 0; i < kCaptureInFlight && ret == kIOReturnSuccess; i++)
        ret = SubmitBulk(iv, kPipeCapture, iv->capture[i], kCaptureBytes);
    for (uint32_t i = 0; i < kMidiInFlight && ret == kIOReturnSuccess; i++)
        ret = SubmitBulk(iv, kPipeMidiIn, iv->midiIn[i], NS7::kMidiPacketBytes);
    if (ret != kIOReturnSuccess) {
        Log("streaming start failed: 0x%08x", ret);
        return ret;
    }

    Log("streaming: %u playback, %u feedback, %u capture, %u MIDI requests in flight",
        kPlaybackInFlight, kFeedbackInFlight, kCaptureInFlight, kMidiInFlight);
    return kIOReturnSuccess;
}

void
IMPL(NumarkNS7Device, PlaybackComplete)
{
    NumarkNS7Device_IVars * iv = ivars;
    if (iv->stopping) return;
    if (!TransferOk(status)) NoteError(iv, kPipePlayback, status);
    else iv->stats[kPipePlayback].done++;
    SubmitPlayback(iv, SlotIndex(action));
}

void
IMPL(NumarkNS7Device, FeedbackComplete)
{
    NumarkNS7Device_IVars * iv = ivars;
    if (iv->stopping) return;
    IsoSlot & slot = iv->feedback[SlotIndex(action)];
    if (!TransferOk(status)) NoteError(iv, kPipeFeedback, status);
    else iv->stats[kPipeFeedback].done++;

    for (uint32_t k = 0; k < kFeedbackFrames; k++) {
        const IOUSBIsochronousFrame & f = slot.framePtr[k];
        if (!TransferOk(f.status) || f.completeCount == 0) continue;
        const int frames = NS7::ParseFeedback(slot.dataPtr + k * kFeedbackMaxPacket, f.completeCount);
        if (frames < 0) continue;
        iv->feedbackFrames = frames;
        if (++iv->feedbackPackets % kStatsEveryFeedback == 0) LogStats(iv);
    }
    SubmitFeedback(iv, SlotIndex(action));
}

void
IMPL(NumarkNS7Device, CaptureComplete)
{
    NumarkNS7Device_IVars * iv = ivars;
    if (iv->stopping) return;
    BulkSlot & slot = iv->capture[SlotIndex(action)];
    if (status == kUSBHostReturnPipeStalled) {
        NoteError(iv, kPipeCapture, status);
        RecoverBulk(iv, kPipeCapture, slot, kCaptureBytes);
        return;
    }
    if (!TransferOk(status)) {
        NoteError(iv, kPipeCapture, status);
        return;
    }
    // Capture data is not delivered anywhere yet; the read keeps the stream alive.
    iv->stats[kPipeCapture].done++;
    iv->captureBytes += actualByteCount;
    SubmitBulk(iv, kPipeCapture, slot, kCaptureBytes);
}

void
IMPL(NumarkNS7Device, MidiInComplete)
{
    NumarkNS7Device_IVars * iv = ivars;
    if (iv->stopping) return;
    BulkSlot & slot = iv->midiIn[SlotIndex(action)];
    if (status == kUSBHostReturnPipeStalled) {
        NoteError(iv, kPipeMidiIn, status);
        RecoverBulk(iv, kPipeMidiIn, slot, NS7::kMidiPacketBytes);
        return;
    }
    if (!TransferOk(status)) {
        NoteError(iv, kPipeMidiIn, status);
        return;
    }
    iv->stats[kPipeMidiIn].done++;
    uint8_t bytes[NS7::kMidiInDataBytes];
    const uint32_t n = NS7::ExtractMidiIn(slot.ptr, actualByteCount, bytes);
    if (n) {
        iv->midiInClient = CopyMidiClient(iv);   // one lookup per transfer
        iv->midiParser.Push(bytes, n, OnMidiInUmp, iv);
        OSSafeReleaseNULL(iv->midiInClient);
    }
    SubmitBulk(iv, kPipeMidiIn, slot, NS7::kMidiPacketBytes);
}

// ── Lifecycle ────────────────────────────────────────────────────────────────

kern_return_t
IMPL(NumarkNS7Device, Start)
{
    kern_return_t ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) return ret;

    ivars->device = OSDynamicCast(IOUSBHostDevice, provider);
    if (ivars->device == nullptr) {
        Log("provider is not an IOUSBHostDevice");
        Stop(provider, SUPERDISPATCH);
        return kIOReturnNoDevice;
    }
    ivars->device->retain();

    ret = ivars->device->Open(this, 0, 0);
    if (ret != kIOReturnSuccess) {
        Log("device Open failed: 0x%08x", ret);
        goto fail;
    }

    LogConfigurationDescriptor(ivars->device);

    // matchInterfaces=false: nothing else should bind to the vendor interfaces.
    ret = ivars->device->SetConfiguration(1, false);
    if (ret != kIOReturnSuccess) {
        Log("SetConfiguration(1) failed: 0x%08x", ret);
        goto fail;
    }

    {
        uintptr_t iterator = 0;
        ret = ivars->device->CreateInterfaceIterator(&iterator);
        if (ret != kIOReturnSuccess) goto fail;

        IOUSBHostInterface * interface = nullptr;
        while (ivars->device->CopyInterface(iterator, &interface) == kIOReturnSuccess
               && interface != nullptr) {
            const IOUSBConfigurationDescriptor * config = interface->CopyConfigurationDescriptor();
            const IOUSBInterfaceDescriptor * desc = interface->GetInterfaceDescriptor(config);
            const uint8_t number = desc ? desc->bInterfaceNumber : 0xFF;
            if (config) IOUSBHostFreeDescriptor(config);

            if (number < kNumInterfaces && ivars->interfaces[number] == nullptr) {
                ivars->interfaces[number] = interface;   // keeps the copy's reference
            } else {
                interface->release();
            }
            interface = nullptr;
        }
        ivars->device->DestroyInterfaceIterator(iterator);
    }

    for (uint8_t i = 0; i < kNumInterfaces; i++) {
        IOUSBHostInterface * interface = ivars->interfaces[i];
        if (interface == nullptr) {
            Log("interface %u missing", i);
            ret = kIOReturnNotFound;
            goto fail;
        }
        ret = interface->Open(this, 0, nullptr);
        if (ret != kIOReturnSuccess) {
            Log("interface %u Open failed: 0x%08x", i, ret);
            goto fail;
        }
        ret = interface->SelectAlternateSetting(kStreamingAltSetting);
        if (ret != kIOReturnSuccess) {
            Log("interface %u SelectAlternateSetting(%u) failed: 0x%08x",
                i, kStreamingAltSetting, ret);
            goto fail;
        }
    }

    for (size_t e = 0; e < kNumEndpoints; e++) {
        const uint8_t address = kEndpoints[e];
        // EPs 0x02/0x83/0x04 are on IF0, 0x81/0x86 on IF1.
        IOUSBHostInterface * interface =
            (address == NS7::kEPFeedbackIn || address == NS7::kEPCaptureIn)
                ? ivars->interfaces[1] : ivars->interfaces[0];
        ret = interface->CopyPipe(address, &ivars->pipes[e]);
        if (ret != kIOReturnSuccess) {
            Log("CopyPipe(0x%02x) failed: 0x%08x", address, ret);
            goto fail;
        }
    }

    Log("NS7 claimed: IF0/IF1 alt %u, %zu pipes open", kStreamingAltSetting, kNumEndpoints);

    ret = CreateBuffer(ivars->interfaces[0], 8, &ivars->control, &ivars->controlPtr);
    if (ret != kIOReturnSuccess) goto fail;

    IOSleep(NS7::kSettleMs);
    ret = Handshake(ivars, this);
    if (ret != kIOReturnSuccess) goto fail;

    ret = StartStreaming(this, ivars);
    if (ret != kIOReturnSuccess) goto fail;

    RegisterService();
    return kIOReturnSuccess;

fail:
    Stop(provider);
    return ret != kIOReturnSuccess ? ret : kIOReturnError;
}

kern_return_t
IMPL(NumarkNS7Device, Stop)
{
    ivars->stopping = true;
    SetMidiClient(nullptr);
    if (ivars->playback[0].action) LogStats(ivars);

    for (size_t e = 0; e < kNumEndpoints; e++) {
        if (ivars->pipes[e]) {
            ivars->pipes[e]->Abort(kIOUSBAbortAsynchronous, kIOReturnAborted, this);
            OSSafeReleaseNULL(ivars->pipes[e]);
        }
    }
    // Actions retain this object; buffers are released in free().
    for (auto & s : ivars->playback) OSSafeReleaseNULL(s.action);
    for (auto & s : ivars->feedback) OSSafeReleaseNULL(s.action);
    for (auto & s : ivars->capture)  OSSafeReleaseNULL(s.action);
    for (auto & s : ivars->midiIn)   OSSafeReleaseNULL(s.action);

    for (uint8_t i = 0; i < kNumInterfaces; i++) {
        if (ivars->interfaces[i]) {
            ivars->interfaces[i]->Close(this, 0);
            OSSafeReleaseNULL(ivars->interfaces[i]);
        }
    }
    if (ivars->device) {
        ivars->device->Close(this, 0);
        OSSafeReleaseNULL(ivars->device);
    }
    return Stop(provider, SUPERDISPATCH);
}

IOUSBHostDevice *
NumarkNS7Device::GetUSBDevice()
{
    return ivars->device;
}

IOUSBHostPipe *
NumarkNS7Device::GetPipe(uint8_t endpointAddress)
{
    for (size_t e = 0; e < kNumEndpoints; e++) {
        if (kEndpoints[e] == endpointAddress) return ivars->pipes[e];
    }
    return nullptr;
}

void
NumarkNS7Device::SetMidiClient(NS7MIDIDriver * client)
{
    if (client) client->retain();
    IOLockLock(ivars->midiClientLock);
    NS7MIDIDriver * old = ivars->midiClient;
    ivars->midiClient = client;
    IOLockUnlock(ivars->midiClientLock);
    if (old) old->release();
}
