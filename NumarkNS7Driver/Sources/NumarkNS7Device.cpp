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
#include <time.h>

#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/OSAction.h>
#include <DriverKit/OSCollections.h>
#include <DriverKit/IOTimerDispatchSource.h>
#include <USBDriverKit/IOUSBHostDevice.h>
#include <USBDriverKit/IOUSBHostFamilyDefinitions.h>
#include <USBDriverKit/IOUSBHostInterface.h>
#include <USBDriverKit/IOUSBHostPipe.h>
#include <USBDriverKit/USBDriverKitDefs.h>

#include "NS7MIDIDriver.h"
#include "NS7Protocol.h"
#include "NumarkNS7Device.h"

#define Log(fmt, ...) os_log(OS_LOG_DEFAULT, "NumarkNS7: " fmt, ##__VA_ARGS__)
// Recovery lines, at most kLoggedRecoveryPerStats per stats period.
#define LogRecovery(iv, fmt, ...) do { \
        if ((iv)->recoveryLogged++ < kLoggedRecoveryPerStats) Log(fmt, ##__VA_ARGS__); \
    } while (0)

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
constexpr uint32_t kMidiOutTimeoutMs    = 1000;
// STALL retries and backoff: NS7::kMidiOutStallRetries, NS7::kMidiOutStallBackoffTicks.
constexpr uint32_t kLoggedRecoveryPerStats = 5;               // recovery lines per stats period
constexpr uint64_t kWatchdogPeriodNs    = 100ull * 1000 * 1000;   // 100 ms
constexpr uint64_t kWatchdogLeewayNs    = 10ull * 1000 * 1000;
constexpr uint32_t kWatchdogStuckTicks  = 10;                 // ~1 s without an iso completion

// A request never legitimately runs further ahead of the bus than the lead
// plus its queue; NextIsoFrame treats anything beyond kIsoMaxAheadFrames as a
// moved frame counter.
static_assert(kIsoLeadFrames + kPlaybackInFlight * kPlaybackMicroframes / 8 < NS7::kIsoMaxAheadFrames,
              "playback queue deeper than kIsoMaxAheadFrames");
static_assert(kIsoLeadFrames + kFeedbackInFlight * kFeedbackFrames < NS7::kIsoMaxAheadFrames,
              "feedback queue deeper than kIsoMaxAheadFrames");
static_assert(NS7::kReturnIsoTooNew == uint32_t(kIOReturnIsoTooNew) &&
              NS7::kReturnIsoTooOld == uint32_t(kIOReturnIsoTooOld) &&
              NS7::kReturnAborted == uint32_t(kIOReturnAborted) &&
              NS7::kReturnNotResponding == uint32_t(kIOReturnNotResponding) &&
              NS7::kReturnNoDevice == uint32_t(kIOReturnNoDevice) &&
              NS7::kReturnOffline == uint32_t(kIOReturnOffline), "IOReturn values");

typedef NS7::MidiOutStateMachine::Action MidiOutAction;
typedef NS7::MidiOutStateMachine::Result MidiOutResult;

struct SlotRef { uint32_t index; };

struct IsoSlot {
    IOBufferMemoryDescriptor * data;
    IOBufferMemoryDescriptor * frames;
    uint8_t                  * dataPtr;
    IOUSBIsochronousFrame    * framePtr;
    OSAction                 * action;
    uint64_t                   firstFrame;
    bool                       armed;     // a request is in flight (see "Keeping the chains alive")
};

struct BulkSlot {
    IOBufferMemoryDescriptor * buffer;
    uint8_t                  * ptr;
    OSAction                 * action;
    bool                       armed;     // a request is in flight
};

struct PipeStats { uint64_t done, errors, resyncs, stalls, rearms; };

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
    BulkSlot midiOut = {};
    NS7::MidiOutStateMachine midiOutState;   // in-flight packet, STALL retries, backoff, counters

    uint64_t playbackFrame = 0;     // next frame for a playback request
    uint64_t feedbackFrame = 0;
    uint32_t playbackSlot  = 0;     // running microframe counter for the 5/6 pattern
    int      feedbackFrames = -1;   // latest frames/ms from EP 0x81

    bool     stopping = false;
    bool     sleeping = false;      // between SetPowerState(Off) and the next wake

    IOTimerDispatchSource * watchdog = nullptr;
    OSAction              * watchdogAction = nullptr;
    uint64_t  isoCompletions = 0;   // both iso chains; the watchdog checks it moves
    uint64_t  watchdogSeen = 0;
    uint32_t  watchdogIdleTicks = 0;
    uint64_t  isoAborts = 0;        // watchdog aborts of stuck iso pipes
    uint32_t  wakes = 0;
    uint32_t  recoveryLogged = 0;   // reset with each stats line

    NS7::RawMidiToUmp midiParser;

    PipeStats stats[kNumEndpoints] = {};
    uint64_t  feedbackPackets = 0;
    uint64_t  captureBytes = 0;
    uint64_t  midiMessages = 0;
    uint64_t  midiOutDropped = 0;   // messages CoreMIDI queued that did not fit the FIFO
    uint64_t  midiOutPumpClient = 0;    // PumpMidiOut calls that found a MIDI client
    uint64_t  midiOutPumpBytes = 0;     // ... and got bytes to send
    uint32_t  midiLogged = 0;       // reset with each stats line
    IOLock        * midiClientLock = nullptr;
    NS7MIDIDriver * midiClient = nullptr;       // guarded by midiClientLock, retained
    NS7MIDIDriver * midiInClient = nullptr;     // set only while MidiInComplete parses
    IOService     * midiService = nullptr;      // from Create(), retained; nullptr if it failed
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
        ReleaseBulkSlots(&ivars->midiOut, 1);
        OSSafeReleaseNULL(ivars->control);
        OSSafeReleaseNULL(ivars->watchdog);   // cancelled in Stop()
        OSSafeReleaseNULL(ivars->midiClient);
        OSSafeReleaseNULL(ivars->midiService);
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

// ── Keeping the chains alive ─────────────────────────────────────────────────
//
// Each request slot (4 playback, 8 feedback, 3 capture, 16 MIDI in) has an
// `armed` flag: set when its request is accepted, cleared when it completes
// or when a submission fails. Normally every completion resubmits its own
// slot, so the flag only drops for an instant. A slot stays idle when:
//   - its submission failed synchronously (no completion will ever come);
//   - it completed with a "pipe going away" status (aborted, not responding,
//     no device, offline: NS7::CompletionResubmitsNow), which is not
//     resubmitted in place so an endpoint that aborts everything can't spin;
//   - a bulk read ended with a non-STALL error, or ClearStall failed;
//   - it completed while the system is asleep.
// Before this, such a slot was lost for good. After a sleep/wake on hardware
// every iso resubmit failed with kIOReturnIsoTooNew and both iso chains, and
// with them the stats line and all MIDI, stopped permanently.
//
// RearmIdle() restarts every idle slot (bounded: one attempt per slot). It
// runs after each successful playback or feedback completion (~every 4 ms
// while either chain lives, so one living chain revives the other and the
// bulk reads) and from the watchdog timer (every 100 ms), which covers the
// case where every slot died at once and no completion will ever arrive. The
// watchdog also aborts the iso pipes if no iso request completed for ~1 s:
// a request stranded at a frame the bus will not reach soon then comes back
// aborted and is re-armed. Nothing is re-armed while `stopping` or
// `sleeping`. A detached device costs at most one failed call per idle slot
// per re-arm pass until Stop() runs; it never loops.

static uint64_t
NextFrame(NumarkNS7Device_IVars * iv, int pipe, uint64_t planned)
{
    uint64_t now = 0;
    if (iv->interfaces[0]->GetFrameNumber(&now, nullptr) != kIOReturnSuccess) return planned;
    if (NS7::IsoFrameTooFarAhead(planned, now))
        LogRecovery(iv, "iso resync: EP 0x%02x planned frame %llu is %llu ahead of the bus",
                    kEndpoints[pipe], planned, planned - now);
    const uint64_t frame = NS7::NextIsoFrame(planned, now, kIsoLeadFrames);
    if (frame != planned && planned != 0) iv->stats[pipe].resyncs++;
    return frame;
}

// Submits `slot` on iso pipe `pipe`, continuing the chain at *chainFrame. If
// the controller rejects the start frame (IsoTooNew/IsoTooOld), reads the
// bus frame again and retries once at now + lead. The chain frame advances
// only when a request was accepted.
static kern_return_t
SubmitIso(NumarkNS7Device_IVars * iv, int pipe, IsoSlot & slot, uint64_t * chainFrame,
          uint64_t framesPerRequest)
{
    slot.firstFrame = NextFrame(iv, pipe, *chainFrame);
    kern_return_t ret = iv->pipes[pipe]->IsochIO(slot.data, slot.frames, slot.firstFrame, slot.action);
    if (NS7::IsoSubmitRetryable(uint32_t(ret))) {
        const kern_return_t first = ret;
        uint64_t now = 0;
        if (iv->interfaces[0]->GetFrameNumber(&now, nullptr) == kIOReturnSuccess) {
            iv->stats[pipe].resyncs++;
            slot.firstFrame = now + kIsoLeadFrames;
            ret = iv->pipes[pipe]->IsochIO(slot.data, slot.frames, slot.firstFrame, slot.action);
            if (ret == kIOReturnSuccess)
                LogRecovery(iv, "iso resync: EP 0x%02x after 0x%08x", kEndpoints[pipe], first);
            else
                LogRecovery(iv, "iso resync: EP 0x%02x after 0x%08x failed: 0x%08x; slot re-armed later",
                            kEndpoints[pipe], first, ret);
        }
    }
    slot.armed = ret == kIOReturnSuccess;
    if (slot.armed) *chainFrame = slot.firstFrame + framesPerRequest;
    else            NoteError(iv, pipe, ret);
    return ret;
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
    return SubmitIso(iv, kPipePlayback, slot, &iv->playbackFrame, kPlaybackMicroframes / 8);
}

static kern_return_t
SubmitFeedback(NumarkNS7Device_IVars * iv, uint32_t i)
{
    IsoSlot & slot = iv->feedback[i];
    for (uint32_t k = 0; k < kFeedbackFrames; k++)
        slot.framePtr[k] = { kIOReturnInvalid, kFeedbackMaxPacket, 0, 0, 0 };
    return SubmitIso(iv, kPipeFeedback, slot, &iv->feedbackFrame, kFeedbackFrames);
}

static kern_return_t
SubmitBulk(NumarkNS7Device_IVars * iv, int pipe, BulkSlot & slot, uint32_t length)
{
    kern_return_t ret = iv->pipes[pipe]->AsyncIO(slot.buffer, length, slot.action, 0);
    slot.armed = ret == kIOReturnSuccess;
    if (ret != kIOReturnSuccess) NoteError(iv, pipe, ret);
    return ret;
}

// Restarts every idle slot; see "Keeping the chains alive". `why` is logged.
static void
RearmIdle(NumarkNS7Device_IVars * iv, const char * why)
{
    if (iv->stopping || iv->sleeping) return;
    uint32_t play = 0, fb = 0, cap = 0, midi = 0;
    // Feedback first, as at start, so playback follows the device's rate.
    for (uint32_t i = 0; i < kFeedbackInFlight; i++) {
        if (iv->feedback[i].armed || iv->feedback[i].action == nullptr) continue;
        if (SubmitFeedback(iv, i) == kIOReturnSuccess) fb++;
    }
    for (uint32_t i = 0; i < kPlaybackInFlight; i++) {
        if (iv->playback[i].armed || iv->playback[i].action == nullptr) continue;
        if (SubmitPlayback(iv, i) == kIOReturnSuccess) play++;
    }
    for (uint32_t i = 0; i < kCaptureInFlight; i++) {
        if (iv->capture[i].armed || iv->capture[i].action == nullptr) continue;
        if (SubmitBulk(iv, kPipeCapture, iv->capture[i], kCaptureBytes) == kIOReturnSuccess) cap++;
    }
    for (uint32_t i = 0; i < kMidiInFlight; i++) {
        if (iv->midiIn[i].armed || iv->midiIn[i].action == nullptr) continue;
        if (SubmitBulk(iv, kPipeMidiIn, iv->midiIn[i], NS7::kMidiPacketBytes) == kIOReturnSuccess) midi++;
    }
    if (play + fb + cap + midi == 0) return;
    iv->stats[kPipePlayback].rearms += play;
    iv->stats[kPipeFeedback].rearms += fb;
    iv->stats[kPipeCapture].rearms  += cap;
    iv->stats[kPipeMidiIn].rearms   += midi;
    LogRecovery(iv, "re-armed %u playback, %u feedback, %u capture, %u MIDI in (%{public}s)",
                play, fb, cap, midi, why);
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

// Runs on the USB queue. Two short lines, since os_log cuts long ones off.
static void
LogStats(NumarkNS7Device_IVars * iv)
{
    // IO block counters, from the CoreMIDI destination: calls, UMP words in,
    // raw bytes queued into the FIFO, first word of the latest call.
    uint32_t blkCalls = 0, blkWords = 0, blkBytes = 0, blkLast = 0;
    NS7MIDIDriver * client = CopyMidiClient(iv);
    const bool haveClient = client != nullptr;
    if (client) {
        iv->midiOutDropped += client->TakeMidiOutDropped();
        client->GetMidiOutBlockStats(&blkCalls, &blkWords, &blkBytes, &blkLast);
        client->release();
    }
    const PipeStats & p = iv->stats[kPipePlayback], & f = iv->stats[kPipeFeedback],
                    & c = iv->stats[kPipeCapture],  & m = iv->stats[kPipeMidiIn];
    const PipeStats & o = iv->stats[kPipeMidiOut];
    Log("stats: play %llu (%llu err, %llu rs) | fb %llu, %d f/ms (%llu err, %llu rs) | "
        "cap %llu B (%llu err, %llu st) | MIDI in %llu xf, %llu msg (%llu err, %llu st)",
        p.done, p.errors, p.resyncs, iv->feedbackPackets, iv->feedbackFrames, f.errors, f.resyncs,
        iv->captureBytes, c.errors, c.stalls, m.done, iv->midiMessages, m.errors, m.stalls);
    Log("MIDI out: %llu pkt %llu B %llu err %llu st %llu lost %llu drop | blk %u call %u w "
        "%u B last %08x | pump %llu cl %llu data | len %u bo %u reg %d",
        iv->midiOutState.PacketsSent(), iv->midiOutState.BytesSent(), o.errors, o.stalls,
        iv->midiOutState.PacketsLost(), iv->midiOutDropped,
        blkCalls, blkWords, blkBytes, blkLast, iv->midiOutPumpClient, iv->midiOutPumpBytes,
        iv->midiOutState.InFlightBytes(), iv->midiOutState.BackoffTicks(), haveClient ? 1 : 0);
    if (iv->midiLogged > kLoggedMidiPerStats)
        Log("%u MIDI messages not logged", iv->midiLogged - kLoggedMidiPerStats);
    const PipeStats & pr = iv->stats[kPipePlayback], & fr = iv->stats[kPipeFeedback];
    if (pr.rearms + fr.rearms + c.rearms + m.rearms + iv->isoAborts + iv->wakes)
        Log("recovery: re-armed play %llu fb %llu cap %llu MIDI in %llu | iso aborts %llu | wakes %u",
            pr.rearms, fr.rearms, c.rearms, m.rearms, iv->isoAborts, iv->wakes);
    iv->midiLogged = 0;
    iv->recoveryLogged = 0;
    iv->midiOutState.OnStatsLogged();
}

// Clears a STALL on EP 0x04. Returns true if the pipe can be used again.
static bool
ClearMidiOutStall(NumarkNS7Device_IVars * iv)
{
    iv->stats[kPipeMidiOut].stalls++;
    const kern_return_t ret = iv->pipes[kPipeMidiOut]->ClearStall(true);
    if (ret != kIOReturnSuccess) {
        Log("EP 0x%02x ClearStall failed: 0x%08x", kEndpoints[kPipeMidiOut], ret);
        return false;
    }
    return true;
}

// After the state machine gave up on a packet (a STALL it couldn't get
// past) and paused MIDI out so a pipe that keeps stalling cannot crowd the
// feedback chain: logs that once per stats period.
static void
LogMidiOutPause(NumarkNS7Device_IVars * iv)
{
    if (iv->midiOutState.TakePauseLog())
        Log("EP 0x%02x keeps stalling: MIDI out paused for ~1 s", kEndpoints[kPipeMidiOut]);
}

// Classifies the return of AsyncIO (submission).
static MidiOutResult
MidiOutSubmitResult(kern_return_t ret)
{
    if (ret == kIOReturnSuccess)          return MidiOutResult::Ok;
    if (ret == kUSBHostReturnPipeStalled) return MidiOutResult::Stalled;
    return MidiOutResult::Error;
}

// Classifies a completion status.
static MidiOutResult
MidiOutCompletionResult(IOReturn status)
{
    if (status == kUSBHostReturnPipeStalled) return MidiOutResult::Stalled;
    return TransferOk(status) ? MidiOutResult::Ok : MidiOutResult::Error;
}

// Submits the packet in iv->midiOut. On failure the state machine drops it;
// the next FeedbackComplete pumps again.
static void
SendMidiOut(NumarkNS7Device_IVars * iv)
{
    kern_return_t ret = iv->pipes[kPipeMidiOut]->AsyncIO(iv->midiOut.buffer, NS7::kMidiPacketBytes,
                                                        iv->midiOut.action, kMidiOutTimeoutMs);
    if (ret != kIOReturnSuccess) NoteError(iv, kPipeMidiOut, ret);
    if (iv->midiOutState.OnSendResult(MidiOutSubmitResult(ret)) == MidiOutAction::ClearStallThenDrop) {
        ClearMidiOutStall(iv);
        LogMidiOutPause(iv);
    }
}

// Starts the next EP 0x04 transfer if none is in flight and CoreMIDI has
// queued bytes. Runs on the USB queue: from FeedbackComplete (~every 4 ms)
// and from MidiOutComplete (back to back while data remains). Sends nothing
// while a stall backoff is running; FeedbackComplete counts it down.
static void
PumpMidiOut(NumarkNS7Device_IVars * iv)
{
    if (iv->stopping || iv->sleeping || iv->midiOut.action == nullptr || !iv->midiOutState.CanStart())
        return;
    NS7MIDIDriver * client = CopyMidiClient(iv);
    if (client == nullptr) return;
    iv->midiOutPumpClient++;
    const uint32_t n = client->NextMidiOutPacket(iv->midiOut.ptr);
    client->release();
    if (n == 0) return;
    iv->midiOutPumpBytes++;
    if (iv->midiOutState.OnPacketReady(n) == MidiOutAction::Send) SendMidiOut(iv);
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
    if (ret == kIOReturnSuccess)
        ret = CreateBuffer(if0, NS7::kMidiPacketBytes, &iv->midiOut.buffer, &iv->midiOut.ptr);
    if (ret == kIOReturnSuccess)
        ret = self->CreateActionMidiOutComplete(sizeof(SlotRef), &iv->midiOut.action);
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

    Log("streaming: %u playback, %u feedback, %u capture, %u MIDI in + 1 MIDI out requests in flight",
        kPlaybackInFlight, kFeedbackInFlight, kCaptureInFlight, kMidiInFlight);
    return kIOReturnSuccess;
}

void
IMPL(NumarkNS7Device, PlaybackComplete)
{
    NumarkNS7Device_IVars * iv = ivars;
    const uint32_t i = SlotIndex(action);
    iv->playback[i].armed = false;
    if (iv->stopping) return;
    iv->isoCompletions++;
    const bool ok = TransferOk(status);
    if (!ok) NoteError(iv, kPipePlayback, status);
    else iv->stats[kPipePlayback].done++;
    if (iv->sleeping || !NS7::CompletionResubmitsNow(uint32_t(status))) return;   // re-armed later
    SubmitPlayback(iv, i);
    if (ok) RearmIdle(iv, "playback");
}

void
IMPL(NumarkNS7Device, FeedbackComplete)
{
    NumarkNS7Device_IVars * iv = ivars;
    const uint32_t i = SlotIndex(action);
    IsoSlot & slot = iv->feedback[i];
    slot.armed = false;
    if (iv->stopping) return;
    iv->isoCompletions++;
    const bool ok = TransferOk(status);
    if (!ok) NoteError(iv, kPipeFeedback, status);
    else iv->stats[kPipeFeedback].done++;

    for (uint32_t k = 0; k < kFeedbackFrames; k++) {
        const IOUSBIsochronousFrame & f = slot.framePtr[k];
        if (!TransferOk(f.status) || f.completeCount == 0) continue;
        const int frames = NS7::ParseFeedback(slot.dataPtr + k * kFeedbackMaxPacket, f.completeCount);
        if (frames < 0) continue;
        iv->feedbackFrames = frames;
        if (++iv->feedbackPackets % kStatsEveryFeedback == 0) LogStats(iv);
    }
    if (iv->sleeping || !NS7::CompletionResubmitsNow(uint32_t(status))) return;   // re-armed later
    SubmitFeedback(iv, i);
    if (ok) RearmIdle(iv, "feedback");
    iv->midiOutState.OnTick();
    PumpMidiOut(iv);
}

void
IMPL(NumarkNS7Device, CaptureComplete)
{
    NumarkNS7Device_IVars * iv = ivars;
    if (iv->stopping) return;
    BulkSlot & slot = iv->capture[SlotIndex(action)];
    slot.armed = false;
    if (status == kUSBHostReturnPipeStalled) {
        NoteError(iv, kPipeCapture, status);
        RecoverBulk(iv, kPipeCapture, slot, kCaptureBytes);
        return;
    }
    if (!TransferOk(status)) {
        NoteError(iv, kPipeCapture, status);
        return;   // idle: RearmIdle restarts it on the next feedback tick
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
    slot.armed = false;
    if (status == kUSBHostReturnPipeStalled) {
        NoteError(iv, kPipeMidiIn, status);
        RecoverBulk(iv, kPipeMidiIn, slot, NS7::kMidiPacketBytes);
        return;
    }
    if (!TransferOk(status)) {
        NoteError(iv, kPipeMidiIn, status);
        return;   // idle: RearmIdle restarts it on the next feedback tick
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

void
IMPL(NumarkNS7Device, MidiOutComplete)
{
    NumarkNS7Device_IVars * iv = ivars;
    (void)action;
    (void)actualByteCount;
    (void)completionTimestamp;
    if (iv->stopping) return;

    const MidiOutResult result = MidiOutCompletionResult(status);
    if (result != MidiOutResult::Ok) NoteError(iv, kPipeMidiOut, status);
    MidiOutAction next = iv->midiOutState.OnComplete(result);
    // Always clear a STALL so the pipe stays usable; the state machine
    // resends the same packet only while retries remain.
    if (next == MidiOutAction::ClearStall) next = iv->midiOutState.OnStallCleared(ClearMidiOutStall(iv));
    switch (next) {
    case MidiOutAction::Send:
        SendMidiOut(iv);
        return;
    case MidiOutAction::StartBackoff:
        LogMidiOutPause(iv);
        break;
    default:   // Pump
        break;
    }
    PumpMidiOut(iv);
}

// ── Watchdog and power ───────────────────────────────────────────────────────

static void
ArmWatchdog(NumarkNS7Device_IVars * iv)
{
    const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    iv->watchdog->WakeAtTime(kIOTimerClockUptimeRaw, now + kWatchdogPeriodNs, kWatchdogLeewayNs);
}

// Runs on the default queue, like every completion, so it needs no locking.
void
IMPL(NumarkNS7Device, WatchdogFired)
{
    NumarkNS7Device_IVars * iv = ivars;
    (void)action;
    (void)time;
    if (iv->stopping || iv->watchdog == nullptr) return;
    if (iv->sleeping) {
        iv->watchdogIdleTicks = 0;
    } else {
        RearmIdle(iv, "watchdog");
        if (iv->isoCompletions != iv->watchdogSeen) {
            iv->watchdogSeen = iv->isoCompletions;
            iv->watchdogIdleTicks = 0;
        } else if (++iv->watchdogIdleTicks >= kWatchdogStuckTicks) {
            // Requests may be parked at frames the bus won't reach soon.
            // Aborting returns them (status aborted); the next pass re-arms.
            iv->watchdogIdleTicks = 0;
            iv->isoAborts++;
            LogRecovery(iv, "iso watchdog: no iso completion for ~1 s, aborting EP 0x%02x and 0x%02x",
                        kEndpoints[kPipePlayback], kEndpoints[kPipeFeedback]);
            iv->pipes[kPipePlayback]->Abort(kIOUSBAbortAsynchronous, kIOReturnAborted, this);
            iv->pipes[kPipeFeedback]->Abort(kIOUSBAbortAsynchronous, kIOReturnAborted, this);
        }
    }
    ArmWatchdog(iv);
}

// Creates and starts the watchdog on the default queue. Not fatal: without
// it, completions still re-arm idle slots while any iso request lives.
static void
StartWatchdog(NumarkNS7Device * self, NumarkNS7Device_IVars * iv)
{
    IODispatchQueue * queue = nullptr;
    kern_return_t ret = self->CopyDispatchQueue(kIOServiceDefaultQueueName, &queue);
    if (ret == kIOReturnSuccess) ret = IOTimerDispatchSource::Create(queue, &iv->watchdog);
    if (ret == kIOReturnSuccess) ret = self->CreateActionWatchdogFired(0, &iv->watchdogAction);
    if (ret == kIOReturnSuccess) ret = iv->watchdog->SetHandler(iv->watchdogAction);
    OSSafeReleaseNULL(queue);
    if (ret != kIOReturnSuccess) {
        Log("watchdog unavailable: 0x%08x; idle slots re-armed from completions only", ret);
        OSSafeReleaseNULL(iv->watchdogAction);
        OSSafeReleaseNULL(iv->watchdog);
        return;
    }
    ArmWatchdog(iv);
}

// Power changes arrive on the default queue. On sleep (Off) the host aborts
// or suspends our requests; completions then leave their slots idle instead
// of resubmitting into a sleeping bus. On the next non-Off state the chain
// frames are dropped (so NextIsoFrame restarts both chains at the current
// bus frame + lead) and every idle slot is re-armed.
kern_return_t
IMPL(NumarkNS7Device, SetPowerState)
{
    NumarkNS7Device_IVars * iv = ivars;
    const bool off = powerFlags == kIOServicePowerCapabilityOff;
    const bool waking = !off && iv->sleeping;
    if (off && !iv->sleeping) {
        Log("power: sleeping (0x%08x)", powerFlags);
        iv->sleeping = true;
    } else if (!waking) {
        Log("power: state 0x%08x", powerFlags);
    }
    const kern_return_t ret = SetPowerState(powerFlags, SUPERDISPATCH);
    if (waking) {
        Log("power: waking (0x%08x)", powerFlags);
        iv->sleeping = false;
        iv->wakes++;
        iv->playbackFrame = 0;
        iv->feedbackFrame = 0;
        iv->watchdogIdleTicks = 0;
        RearmIdle(iv, "wake");
    }
    return ret;
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

    // The CoreMIDI service is created here, in this process, from the
    // NS7MIDIDriverProperties dictionary of our personality. A separate
    // matching personality ran in its own dext process on hardware, where it
    // could not reach this object. Create() waits for NS7MIDIDriver::Start,
    // which needs only GetUSBDevice() and SetMidiClient(). It is done before
    // streaming because Start runs on the default queue that services the
    // USB completions: blocking it after StartStreaming would let the
    // in-flight feedback requests run out, and the NS7 then stalls its bulk
    // IN endpoints. MIDI is optional, so a failure is logged, not fatal.
    {
        const kern_return_t midiRet = Create(this, "NS7MIDIDriverProperties", &ivars->midiService);
        if (midiRet != kIOReturnSuccess) {
            Log("creating NS7MIDIDriver failed: 0x%08x; continuing without MIDI", midiRet);
            ivars->midiService = nullptr;
        }
    }

    IOSleep(NS7::kSettleMs);
    ret = Handshake(ivars, this);
    if (ret != kIOReturnSuccess) goto fail;

    ret = StartStreaming(this, ivars);
    if (ret != kIOReturnSuccess) goto fail;
    StartWatchdog(this, ivars);

    RegisterService();
    return kIOReturnSuccess;

fail:
    // A created service is torn down with its provider only on termination;
    // a failed Start must remove it explicitly (asynchronous).
    if (ivars->midiService) ivars->midiService->Terminate(0);
    Stop(provider);
    return ret != kIOReturnSuccess ? ret : kIOReturnError;
}

kern_return_t
IMPL(NumarkNS7Device, Stop)
{
    ivars->stopping = true;
    // The handler checks `stopping`; the source itself is released in free().
    if (ivars->watchdog) ivars->watchdog->Cancel(^{});
    OSSafeReleaseNULL(ivars->watchdogAction);
    ivars->midiOutState.OnStop();
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
    OSSafeReleaseNULL(ivars->midiOut.action);

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
    // On termination IOKit stops NS7MIDIDriver (our child) before calling
    // this Stop, so only our reference is left to drop.
    OSSafeReleaseNULL(ivars->midiService);
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

void
NumarkNS7Device::SyncUsbQueue()
{
    IODispatchQueue * queue = nullptr;
    const kern_return_t ret = CopyDispatchQueue(kIOServiceDefaultQueueName, &queue);
    if (ret != kIOReturnSuccess || queue == nullptr) {
        Log("SyncUsbQueue: CopyDispatchQueue failed: 0x%08x", ret);
        return;
    }
    queue->DispatchSync(^{});
    queue->release();
}
