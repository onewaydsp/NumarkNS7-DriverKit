# NS7 CoreMIDI Service Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Publish the Numark NS7 to CoreMIDI from the existing dext: a source carrying the controller's MIDI and a destination that sends MIDI to EP 0x04.

**Architecture:** A new `NS7MIDIDriver : IOUserMIDIDriver` service matches on the existing `NumarkNS7Device` and runs in the same dext process. `NumarkNS7Device` keeps owning all USB I/O. It hands incoming UMP words to the MIDI driver and pulls ready-made 42-byte EP 0x04 packets from it. MIDI out crosses from CoreMIDI's real-time thread to the USB queue through a lock-free SPSC byte FIFO in `NS7Protocol.h`, which is unit-tested on the host.

**Tech Stack:** DriverKit 25 (USBDriverKit, MIDIDriverKit), C++17, host unit tests with clang++, a CoreMIDI command-line tool for hardware checks.

**Spec:** `docs/superpowers/specs/2026-09-26-coremidi-service-design.md`

---

## Background the engineer needs

- The NS7 is **not** a class-compliant device. Its MIDI is raw MIDI 1.0 bytes in 42-byte bulk packets:
  - IN on EP 0x83: bytes 0..40 are data, `0xFD` is filler.
  - OUT on EP 0x04: up to 39 data bytes, then `0xFD` fill, and byte 41 is `0xE0`.
  - The helpers already exist in `NumarkNS7Driver/Sources/NS7Protocol.h`: `ExtractMidiIn`, `BuildMidiOutPacket`, `RawMidiToUmp`, `UmpToRawMidi`.
- The NS7 only sends MIDI while its audio pipes stream. `NumarkNS7Device` already streams from `Start()`. Do not make streaming depend on CoreMIDI.
- DriverKit `.iig` files are processed by the `iig` tool, which generates `<Name>.h`. `LOCALONLY` methods are plain in-process C++ calls. Keep their parameter types to built-ins and forward-declared classes.
- `IONewZero` zero-fills ivars **without running constructors**. Every ivar must be valid when all-zero. `std::atomic<uint32_t>` zero-filled is value 0 on Apple platforms.
- Host tests: `make -C Tests` (ASan + UBSan). Dext build: `xcodebuild -scheme NumarkNS7Installer -configuration Debug -allowProvisioningUpdates build`.
- Installing on hardware: see Task 6. The steps come from the project memory "Dext signing & loading".

## File structure

| File | Change | Responsibility |
|---|---|---|
| `NumarkNS7Driver/Sources/NS7Protocol.h` | modify | + `ByteFifo<N>`, `MidiOutFifo`, `QueueUmpAsRawMidi`, `NextMidiOutPacket` (pure, host-testable) |
| `Tests/NS7ProtocolTests.cpp` | modify | tests for the above |
| `NumarkNS7Driver/Sources/NS7MIDIDriver.iig` | create | the CoreMIDI service interface |
| `NumarkNS7Driver/Sources/NS7MIDIDriver.cpp` | create | creates the CoreMIDI device, entity, source and destination; FIFO producer; `DeliverMidiIn` |
| `NumarkNS7Driver/Sources/NumarkNS7Device.iig` | modify | + `SetMidiClient`, + `MidiOutComplete` |
| `NumarkNS7Driver/Sources/NumarkNS7Device.cpp` | modify | MIDI client registry; MIDI-in delivery; EP 0x04 sender |
| `NumarkNS7Driver/Info.plist` | modify | + `NS7MIDIDriver` personality |
| `NumarkNS7Driver.xcodeproj/project.pbxproj` | modify | add the 2 sources, link MIDIDriverKit, DriverKit deployment target 25.0 |
| `Tools/ns7midi/ns7midi.cpp`, `Tools/ns7midi/Makefile` | create | CoreMIDI CLI: `list`, `monitor`, `send` |

---

### Task 1: SPSC byte FIFO

**Files:**
- Modify: `NumarkNS7Driver/Sources/NS7Protocol.h` (includes at the top; new section before `// ── Ring buffer copies`)
- Test: `Tests/NS7ProtocolTests.cpp`

- [ ] **Step 1: Write the failing tests**

In `Tests/NS7ProtocolTests.cpp`, add these functions just above `int main()`:

```cpp
// ── MIDI out FIFO ────────────────────────────────────────────────────────────

static void test_fifo_read_returns_written_bytes_in_order()
{
    ByteFifo<16> f;
    const uint8_t in[] = { 0x90, 0x3C, 0x7F };
    CHECK(f.Write(in, 3));
    CHECK_EQ(f.Size(), 3u);
    uint8_t out[8] = {};
    CHECK_EQ(f.Read(out, sizeof out), 3u);
    CHECK(memcmp(in, out, 3) == 0);
    CHECK_EQ(f.Size(), 0u);
}

static void test_fifo_read_respects_max_and_keeps_rest()
{
    ByteFifo<16> f;
    const uint8_t in[] = { 1, 2, 3, 4, 5 };
    f.Write(in, 5);
    uint8_t out[5] = {};
    CHECK_EQ(f.Read(out, 2), 2u);
    CHECK_EQ(out[0], 1); CHECK_EQ(out[1], 2);
    CHECK_EQ(f.Read(out, 5), 3u);
    CHECK_EQ(out[0], 3); CHECK_EQ(out[2], 5);
}

static void test_fifo_wraps_around()
{
    ByteFifo<8> f;
    uint8_t next = 0, expect = 0;
    for (int round = 0; round < 50; round++) {
        uint8_t in[5];
        for (auto & b : in) b = next++;
        CHECK(f.Write(in, 5));
        uint8_t out[5] = {};
        CHECK_EQ(f.Read(out, 5), 5u);
        for (auto b : out) CHECK_EQ(b, expect++);
    }
}

static void test_fifo_rejects_message_that_does_not_fit_whole()
{
    ByteFifo<8> f;
    const uint8_t six[6] = { 1, 2, 3, 4, 5, 6 }, three[3] = { 7, 8, 9 };
    CHECK(f.Write(six, 6));
    CHECK(!f.Write(three, 3));      // only 2 bytes free
    CHECK_EQ(f.Size(), 6u);         // nothing partial was written
    CHECK(f.Write(three, 2));       // 2 bytes still fit
    CHECK_EQ(f.Size(), 8u);
}

static void test_fifo_read_empty_returns_zero()
{
    ByteFifo<8> f;
    uint8_t out[4];
    CHECK_EQ(f.Read(out, sizeof out), 0u);
}
```

Register them in the `tests[]` table in `main()`, after `T(test_real_split_pitch_bend_reassembles),`:

```cpp
        T(test_fifo_read_returns_written_bytes_in_order),
        T(test_fifo_read_respects_max_and_keeps_rest),
        T(test_fifo_wraps_around),
        T(test_fifo_rejects_message_that_does_not_fit_whole),
        T(test_fifo_read_empty_returns_zero),
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `make -C Tests 2>&1 | grep -E "error" | head -3`
Expected: compile errors like `use of undeclared identifier 'ByteFifo'`.

- [ ] **Step 3: Implement `ByteFifo`**

In `NumarkNS7Driver/Sources/NS7Protocol.h`, add `#include <atomic>` after `#include <stddef.h>`:

```cpp
#include <atomic>
#include <stddef.h>
```

Then insert this section immediately before the line `// ── Ring buffer copies ──…`:

```cpp
// ── MIDI out FIFO ────────────────────────────────────────────────────────────

// Single-producer/single-consumer byte ring. For MIDI out the producer is
// CoreMIDI's real-time thread and the consumer is the USB queue. Writes are
// all-or-nothing, so a MIDI message is never split by a full buffer. Indices
// run free and wrap at 2^32. N must be a power of two. A zero-filled object
// is a valid empty FIFO (the dext allocates it with IONewZero).
template <uint32_t N>
class ByteFifo {
    static_assert(N != 0 && (N & (N - 1)) == 0, "N must be a power of two");

public:
    uint32_t Size() const
    {
        return mHead.load(std::memory_order_acquire) - mTail.load(std::memory_order_acquire);
    }

    // Producer side. Returns false, writing nothing, if `n` bytes do not fit.
    bool Write(const uint8_t * data, uint32_t n)
    {
        const uint32_t head = mHead.load(std::memory_order_relaxed);
        const uint32_t tail = mTail.load(std::memory_order_acquire);
        if (n > N - (head - tail)) return false;
        for (uint32_t i = 0; i < n; i++) mBuf[(head + i) & (N - 1)] = data[i];
        mHead.store(head + n, std::memory_order_release);
        return true;
    }

    // Consumer side. Copies up to `max` bytes out; returns the count.
    uint32_t Read(uint8_t * out, uint32_t max)
    {
        const uint32_t tail = mTail.load(std::memory_order_relaxed);
        const uint32_t head = mHead.load(std::memory_order_acquire);
        uint32_t n = head - tail;
        if (n > max) n = max;
        for (uint32_t i = 0; i < n; i++) out[i] = mBuf[(tail + i) & (N - 1)];
        mTail.store(tail + n, std::memory_order_release);
        return n;
    }

private:
    std::atomic<uint32_t> mHead { 0 };
    std::atomic<uint32_t> mTail { 0 };
    uint8_t               mBuf[N];
};

constexpr uint32_t kMidiOutFifoBytes = 4096;
typedef ByteFifo<kMidiOutFifoBytes> MidiOutFifo;
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `make -C Tests 2>&1 | grep -E "fifo|checks"`
Expected: five `ok   test_fifo_…` lines and `… checks, 0 failures`.

- [ ] **Step 5: Commit**

```bash
git add NumarkNS7Driver/Sources/NS7Protocol.h Tests/NS7ProtocolTests.cpp
git commit -m "Add lock-free SPSC byte FIFO for MIDI out"
```

---

### Task 2: UMP → FIFO and FIFO → EP 0x04 packet helpers

**Files:**
- Modify: `NumarkNS7Driver/Sources/NS7Protocol.h` (append to the `// ── MIDI out FIFO` section, after the `MidiOutFifo` typedef)
- Test: `Tests/NS7ProtocolTests.cpp`

- [ ] **Step 1: Write the failing tests**

Add above `int main()` in `Tests/NS7ProtocolTests.cpp`:

```cpp
static void test_queue_ump_channel_voice_as_raw_bytes()
{
    ByteFifo<64> f;
    const uint32_t ump[] = { 0x20903C7Fu, 0x20C00500u };   // note on, program change
    CHECK_EQ(QueueUmpAsRawMidi(ump, 2, f), 0u);
    uint8_t out[8] = {};
    CHECK_EQ(f.Read(out, sizeof out), 5u);
    const uint8_t expect[] = { 0x90, 0x3C, 0x7F, 0xC0, 0x05 };
    CHECK(memcmp(out, expect, 5) == 0);
}

static void test_queue_ump_sysex_as_raw_bytes()
{
    ByteFifo<64> f;
    const uint32_t ump[] = { 0x30027E7Fu, 0x00000000u };   // complete SysEx, 2 bytes
    CHECK_EQ(QueueUmpAsRawMidi(ump, 2, f), 0u);
    uint8_t out[8] = {};
    CHECK_EQ(f.Read(out, sizeof out), 4u);
    const uint8_t expect[] = { 0xF0, 0x7E, 0x7F, 0xF7 };
    CHECK(memcmp(out, expect, 4) == 0);
}

static void test_queue_ump_skips_unsupported_types()
{
    ByteFifo<64> f;
    // MIDI 2.0 note on (type 4, two words), then a MIDI 1.0 note off.
    const uint32_t ump[] = { 0x40903C00u, 0xFFFF0000u, 0x20803C00u };
    CHECK_EQ(QueueUmpAsRawMidi(ump, 3, f), 0u);
    uint8_t out[8] = {};
    CHECK_EQ(f.Read(out, sizeof out), 3u);
    const uint8_t expect[] = { 0x80, 0x3C, 0x00 };
    CHECK(memcmp(out, expect, 3) == 0);
}

static void test_queue_ump_drops_whole_message_when_full()
{
    ByteFifo<4> f;
    const uint32_t ump[] = { 0x20903C7Fu, 0x20903D7Fu };   // 3 bytes each, room for one
    CHECK_EQ(QueueUmpAsRawMidi(ump, 2, f), 1u);
    CHECK_EQ(f.Size(), 3u);
}

static void test_next_packet_takes_at_most_39_bytes()
{
    ByteFifo<64> f;
    uint8_t in[50];
    for (uint8_t i = 0; i < 50; i++) in[i] = i;
    f.Write(in, 50);

    uint8_t pkt[kMidiPacketBytes];
    CHECK_EQ(NextMidiOutPacket(f, pkt), 39u);
    CHECK(memcmp(pkt, in, 39) == 0);
    CHECK_EQ(pkt[39], kMidiFill);
    CHECK_EQ(pkt[40], kMidiFill);
    CHECK_EQ(pkt[41], kMidiCPort);

    CHECK_EQ(NextMidiOutPacket(f, pkt), 11u);
    CHECK(memcmp(pkt, in + 39, 11) == 0);
    CHECK_EQ(pkt[11], kMidiFill);
    CHECK_EQ(pkt[41], kMidiCPort);
}

static void test_next_packet_empty_fifo_returns_zero()
{
    ByteFifo<64> f;
    uint8_t pkt[kMidiPacketBytes];
    CHECK_EQ(NextMidiOutPacket(f, pkt), 0u);
}
```

Register in `tests[]` after the Task 1 entries:

```cpp
        T(test_queue_ump_channel_voice_as_raw_bytes),
        T(test_queue_ump_sysex_as_raw_bytes),
        T(test_queue_ump_skips_unsupported_types),
        T(test_queue_ump_drops_whole_message_when_full),
        T(test_next_packet_takes_at_most_39_bytes),
        T(test_next_packet_empty_fifo_returns_zero),
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `make -C Tests 2>&1 | grep -E "error" | head -3`
Expected: `use of undeclared identifier 'QueueUmpAsRawMidi'` (and `NextMidiOutPacket`).

- [ ] **Step 3: Implement the helpers**

`UmpToRawMidi` and `BuildMidiOutPacket` are defined earlier in `NS7Protocol.h` than the FIFO section, so the helpers can go right after the `MidiOutFifo` typedef:

```cpp
// Converts UMP messages to raw MIDI 1.0 bytes and queues each message whole.
// Types other than 1, 2 and 3 are skipped. Returns how many messages were
// dropped because the FIFO had no room. Real-time safe: no locks, no allocation.
template <uint32_t N>
inline uint32_t QueueUmpAsRawMidi(const uint32_t * words, size_t count, ByteFifo<N> & fifo)
{
    struct Ctx { ByteFifo<N> * fifo; uint32_t dropped; } ctx = { &fifo, 0 };
    UmpToRawMidi(words, count, [](void * c, const uint8_t * bytes, size_t n) {
        auto * x = static_cast<Ctx *>(c);
        if (!x->fifo->Write(bytes, uint32_t(n))) x->dropped++;
    }, &ctx);
    return ctx.dropped;
}

// Fills one EP 0x04 packet with up to 39 bytes from the FIFO. Returns the
// number of MIDI bytes packed; 0 means the FIFO was empty and `pkt` is untouched.
template <uint32_t N>
inline uint32_t NextMidiOutPacket(ByteFifo<N> & fifo, uint8_t pkt[kMidiPacketBytes])
{
    uint8_t bytes[kMidiOutMaxBytes];
    const uint32_t n = fifo.Read(bytes, kMidiOutMaxBytes);
    return BuildMidiOutPacket(bytes, n, pkt);
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `make -C Tests 2>&1 | grep -E "queue_ump|next_packet|checks"`
Expected: six `ok` lines and `… checks, 0 failures`.

- [ ] **Step 5: Commit**

```bash
git add NumarkNS7Driver/Sources/NS7Protocol.h Tests/NS7ProtocolTests.cpp
git commit -m "Add UMP-to-FIFO and FIFO-to-EP-0x04 packet helpers"
```

---

### Task 3: NS7MIDIDriver service, project wiring, MIDI-in delivery

**Files:**
- Create: `NumarkNS7Driver/Sources/NS7MIDIDriver.iig`, `NumarkNS7Driver/Sources/NS7MIDIDriver.cpp`
- Modify: `NumarkNS7Driver/Sources/NumarkNS7Device.iig`, `NumarkNS7Driver/Sources/NumarkNS7Device.cpp`, `NumarkNS7Driver/Info.plist`, `NumarkNS7Driver.xcodeproj/project.pbxproj`

There is no host test for DriverKit glue. The verification is a warning-free dext build here, plus the hardware checks in Task 6.

- [ ] **Step 1: Create `NumarkNS7Driver/Sources/NS7MIDIDriver.iig`**

```cpp
// NS7MIDIDriver.iig
// Publishes the NS7 to CoreMIDI: one device, one entity "MIDI" with one
// source and one destination, named the way Numark's original driver named
// them. Matches on NumarkNS7Device and runs in the same dext process;
// NumarkNS7Device owns all USB I/O and calls the two LOCALONLY methods below
// on its USB queue. See docs/superpowers/specs/2026-09-26-coremidi-service-design.md.

#ifndef NS7MIDIDriver_h
#define NS7MIDIDriver_h

#include <DriverKit/IOService.iig>
#include <MIDIDriverKit/IOUserMIDIDriver.iig>

class NS7MIDIDriver : public IOUserMIDIDriver
{
public:
    virtual bool init() override;
    virtual void free() override;

    virtual kern_return_t Start(IOService * provider) override;
    virtual kern_return_t Stop(IOService * provider) override;

    virtual kern_return_t StartIO(OSArray * deviceList) override LOCALONLY;
    virtual kern_return_t StopIO() override LOCALONLY;

    // Complete UMP messages from the NS7, sent to the CoreMIDI source.
    void DeliverMidiIn(const uint32_t * words, uint32_t count) LOCALONLY;

    // Fills one 42-byte EP 0x04 packet from pending CoreMIDI output.
    // Returns the number of MIDI bytes packed; 0 means nothing to send.
    uint32_t NextMidiOutPacket(uint8_t * packet) LOCALONLY;
};

#endif /* NS7MIDIDriver_h */
```

- [ ] **Step 2: Create `NumarkNS7Driver/Sources/NS7MIDIDriver.cpp`**

```cpp
// NS7MIDIDriver.cpp
// CoreMIDI side of the NS7 dext. See NS7MIDIDriver.iig.

#include <os/log.h>
#include <stdio.h>

#include <DriverKit/IOLib.h>
#include <DriverKit/OSCollections.h>
#include <MIDIDriverKit/MIDIDriverKit.h>

#include "NS7Protocol.h"
#include "NS7MIDIDriver.h"
#include "NumarkNS7Device.h"

#define Log(fmt, ...) os_log(OS_LOG_DEFAULT, "NumarkNS7 MIDI: " fmt, ##__VA_ARGS__)

using namespace MIDIDriverKit;

namespace {

// Names from Numark's original CoreMIDI plugin (setUpEndpoints @0x4ee8): the
// device and manufacturer come from the USB strings (these literals are the
// fallbacks and match what a real NS7 reports), and the single entity is "MIDI".
constexpr const char * kFallbackProduct = "Numark USB Audio Device";
constexpr const char * kFallbackVendor  = "Numark";
constexpr const char * kPortName        = "MIDI";
constexpr const char * kModelUID        = "com.andrewabner.ns7.15E4.0071";
constexpr uint32_t     kMaxSysExSpeed   = 39000;   // bytes/s: one 39-byte packet per ms
constexpr uint32_t     kLoggedSendErrors = 5;

} // namespace

// IONewZero zero-fills this without running constructors; every member is
// valid as zero (MidiOutFifo included).
struct NS7MIDIDriver_IVars
{
    NumarkNS7Device       * provider;
    IOUserMIDIDevice      * device;
    IOUserMIDIEntity      * entity;
    IOUserMIDISource      * source;
    IOUserMIDIDestination * destination;
    bool                    running;          // __atomic: set by StartIO/StopIO
    uint32_t                midiOutDropped;   // __atomic: messages dropped, FIFO full
    uint32_t                sendErrors;       // USB queue only
    NS7::UmpOutState        midiOutState;     // real-time thread only; SysEx framing across calls
    NS7::MidiOutFifo        midiOut;
};

bool
NS7MIDIDriver::init()
{
    if (!super::init()) return false;
    ivars = IONewZero(NS7MIDIDriver_IVars, 1);
    return ivars != nullptr;
}

void
NS7MIDIDriver::free()
{
    if (ivars) {
        OSSafeReleaseNULL(ivars->destination);
        OSSafeReleaseNULL(ivars->source);
        OSSafeReleaseNULL(ivars->entity);
        OSSafeReleaseNULL(ivars->device);
        OSSafeReleaseNULL(ivars->provider);
    }
    IOSafeDeleteNULL(ivars, NS7MIDIDriver_IVars, 1);
    super::free();
}

// Retained copy of a string property of `service`, or of `fallback`.
static OSString *
CopyStringProperty(IOService * service, const char * key, const char * fallback)
{
    OSDictionary * props = nullptr;
    OSString * result = nullptr;
    if (service && service->CopyProperties(&props) == kIOReturnSuccess && props) {
        result = OSDynamicCast(OSString, props->getObject(key));
        if (result) result->retain();
        props->release();
    }
    return result ? result : OSString::withCString(fallback);
}

static uint32_t
LocationID(IOService * service)
{
    OSDictionary * props = nullptr;
    uint32_t location = 0;
    if (service && service->CopyProperties(&props) == kIOReturnSuccess && props) {
        if (OSNumber * n = OSDynamicCast(OSNumber, props->getObject("locationID")))
            location = n->unsigned32BitValue();
        props->release();
    }
    return location;
}

static kern_return_t
CreateMidiObjects(NS7MIDIDriver * self, NS7MIDIDriver_IVars * iv)
{
    const auto protocol = IOUserMIDIProtocolID::MIDIProtocol_1_0;
    IOService * usb = iv->provider->GetUSBDevice();
    char uid[64];
    snprintf(uid, sizeof uid, "%s.%08X", kModelUID, LocationID(usb));

    OSString * product   = CopyStringProperty(usb, "USB Product Name", kFallbackProduct);
    OSString * vendor    = CopyStringProperty(usb, "USB Vendor Name", kFallbackVendor);
    OSString * deviceUID = OSString::withCString(uid);
    OSString * modelUID  = OSString::withCString(kModelUID);
    OSString * portName  = OSString::withCString(kPortName);
    OSNumber * sysex     = OSNumber::withNumber(kMaxSysExSpeed, 32);
    kern_return_t ret    = kIOReturnNoMemory;

    if (!product || !vendor || !deviceUID || !modelUID || !portName || !sysex) goto done;

    iv->device = IOUserMIDIDevice::Create(self, deviceUID, modelUID, vendor).detach();
    if (iv->device)
        iv->entity = IOUserMIDIEntity::Create(self, iv->device, portName, protocol, 1, 1).detach();
    iv->source      = IOUserMIDISource::Create(self, portName, protocol).detach();
    iv->destination = IOUserMIDIDestination::Create(self, portName, protocol).detach();
    if (!iv->device || !iv->entity || !iv->source || !iv->destination) goto done;

    if ((ret = iv->device->SetName(product)) != kIOReturnSuccess) goto done;
    iv->device->SetProperty(IOUserMIDIProperty::Manufacturer, vendor);
    iv->device->SetProperty(IOUserMIDIProperty::Model, product);
    iv->entity->SetProperty(IOUserMIDIProperty::MaxSysExSpeed, sysex);

    // CoreMIDI real-time thread: no locks, no allocation, no logging.
    ret = iv->destination->SetIOBlock(^kern_return_t(const IOUserMIDIUMPWord * words, size_t numWords) {
        const uint32_t dropped = NS7::QueueUmpAsRawMidi(words, numWords, iv->midiOut, iv->midiOutState);
        if (dropped) __atomic_fetch_add(&iv->midiOutDropped, dropped, __ATOMIC_RELAXED);
        return kIOReturnSuccess;
    });
    if (ret != kIOReturnSuccess) goto done;

    if ((ret = iv->entity->AddSource(iv->source)) != kIOReturnSuccess) goto done;
    if ((ret = iv->entity->AddDestination(iv->destination)) != kIOReturnSuccess) goto done;
    if ((ret = iv->device->AddEntity(iv->entity)) != kIOReturnSuccess) goto done;
    if ((ret = self->AddObject(iv->device)) != kIOReturnSuccess) goto done;

    // The headers don't say whether children also need AddObject. Add them
    // and tolerate "already added".
    {
        IOUserMIDIObject * const children[] = { iv->entity, iv->source, iv->destination };
        for (IOUserMIDIObject * child : children) {
            const kern_return_t r = self->AddObject(child);
            if (r != kIOReturnSuccess) Log("AddObject(child) returned 0x%08x (ignored)", r);
        }
    }
    ret = kIOReturnSuccess;

done:
    OSSafeReleaseNULL(product);
    OSSafeReleaseNULL(vendor);
    OSSafeReleaseNULL(deviceUID);
    OSSafeReleaseNULL(modelUID);
    OSSafeReleaseNULL(portName);
    OSSafeReleaseNULL(sysex);
    return ret;
}

kern_return_t
IMPL(NS7MIDIDriver, Start)
{
    kern_return_t ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) return ret;

    ivars->provider = OSDynamicCast(NumarkNS7Device, provider);
    if (ivars->provider == nullptr) {
        Log("provider is not a NumarkNS7Device");
        Stop(provider, SUPERDISPATCH);
        return kIOReturnNoDevice;
    }
    ivars->provider->retain();

    ret = CreateMidiObjects(this, ivars);
    if (ret != kIOReturnSuccess) {
        Log("creating CoreMIDI objects failed: 0x%08x", ret);
        Stop(provider);
        return ret;
    }

    ivars->provider->SetMidiClient(this);
    RegisterService();
    Log("CoreMIDI device published");
    return kIOReturnSuccess;
}

kern_return_t
IMPL(NS7MIDIDriver, Stop)
{
    __atomic_store_n(&ivars->running, false, __ATOMIC_RELEASE);
    if (ivars->provider) ivars->provider->SetMidiClient(nullptr);
    if (ivars->device) RemoveObject(ivars->device);
    return Stop(provider, SUPERDISPATCH);
}

kern_return_t
NS7MIDIDriver::StartIO(OSArray * deviceList)
{
    const kern_return_t ret = super::StartIO(deviceList);
    if (ret == kIOReturnSuccess) __atomic_store_n(&ivars->running, true, __ATOMIC_RELEASE);
    Log("StartIO: 0x%08x", ret);
    return ret;
}

kern_return_t
NS7MIDIDriver::StopIO()
{
    __atomic_store_n(&ivars->running, false, __ATOMIC_RELEASE);
    Log("StopIO");
    return super::StopIO();
}

void
NS7MIDIDriver::DeliverMidiIn(const uint32_t * words, uint32_t count)
{
    if (!__atomic_load_n(&ivars->running, __ATOMIC_ACQUIRE) || ivars->source == nullptr) return;
    const kern_return_t ret = ivars->source->Send(words, count);
    if (ret != kIOReturnSuccess && ivars->sendErrors++ < kLoggedSendErrors)
        Log("source Send failed: 0x%08x", ret);
}

uint32_t
NS7MIDIDriver::NextMidiOutPacket(uint8_t * packet)
{
    const uint32_t dropped = __atomic_exchange_n(&ivars->midiOutDropped, 0, __ATOMIC_RELAXED);
    if (dropped) Log("dropped %u MIDI out messages (FIFO full)", dropped);
    return NS7::NextMidiOutPacket(ivars->midiOut, packet);
}
```

- [ ] **Step 3: Add `SetMidiClient` to `NumarkNS7Device.iig`**

In `NumarkNS7Driver/Sources/NumarkNS7Device.iig`, add a forward declaration after `class IOUSBHostInterface;`:

```cpp
class IOUSBHostInterface;
class NS7MIDIDriver;
```

and after the `GetPipe` declaration in the `public:` section add:

```cpp
    // Registers the CoreMIDI service (retained) that receives MIDI in and
    // supplies MIDI out; nullptr unregisters it. Safe from any thread.
    void SetMidiClient(NS7MIDIDriver * client) LOCALONLY;
```

- [ ] **Step 4: Wire the MIDI client into `NumarkNS7Device.cpp`**

4a. Include the generated header after `#include "NumarkNS7Device.h"`:

```cpp
#include "NS7MIDIDriver.h"
#include "NumarkNS7Device.h"
```

(Put `NS7MIDIDriver.h` first. The order doesn't matter, but keep it alphabetical.)

4b. In `struct NumarkNS7Device_IVars`, after `uint32_t  midiLogged = 0;` add:

```cpp
    IOLock        * midiClientLock = nullptr;
    NS7MIDIDriver * midiClient = nullptr;       // guarded by midiClientLock, retained
    NS7MIDIDriver * midiInClient = nullptr;     // set only while MidiInComplete parses
```

4c. In `NumarkNS7Device::init()`, after `ivars->feedbackFrames = -1;` add:

```cpp
    ivars->midiClientLock = IOLockAlloc();
    if (ivars->midiClientLock == nullptr) return false;
```

4d. In `NumarkNS7Device::free()`, inside `if (ivars) {`, after `OSSafeReleaseNULL(ivars->control);` add:

```cpp
        OSSafeReleaseNULL(ivars->midiClient);
        if (ivars->midiClientLock) IOLockFree(ivars->midiClientLock);
```

4e. Add these helpers just above `static void OnMidiInUmp(`:

```cpp
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
```

4f. Replace the body of `OnMidiInUmp` with:

```cpp
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
```

4g. In `IMPL(NumarkNS7Device, MidiInComplete)`, replace

```cpp
    if (n) iv->midiParser.Push(bytes, n, OnMidiInUmp, iv);
```

with

```cpp
    if (n) {
        iv->midiInClient = CopyMidiClient(iv);   // one lookup per transfer
        iv->midiParser.Push(bytes, n, OnMidiInUmp, iv);
        OSSafeReleaseNULL(iv->midiInClient);
    }
```

4h. Add the method definition at the end of the file, after `GetPipe`:

```cpp
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
```

4i. In `IMPL(NumarkNS7Device, Stop)`, make the first line after `ivars->stopping = true;`:

```cpp
    SetMidiClient(nullptr);
```

- [ ] **Step 5: Add the `NS7MIDIDriver` personality to `NumarkNS7Driver/Info.plist`**

Inside `<key>IOKitPersonalities</key><dict>`, after the closing `</dict>` of the `NumarkNS7Device` personality, add:

```xml
		<key>NS7MIDIDriver</key>
		<dict>
			<key>CFBundleIdentifier</key>
			<string>$(PRODUCT_BUNDLE_IDENTIFIER)</string>
			<key>CFBundleIdentifierKernel</key>
			<string>com.apple.kpi.iokit</string>
			<key>IOClass</key>
			<string>IOUserService</string>
			<key>IOProviderClass</key>
			<string>IOUserService</string>
			<key>IOPropertyMatch</key>
			<dict>
				<key>IOUserClass</key>
				<string>NumarkNS7Device</string>
			</dict>
			<key>IOUserClass</key>
			<string>NS7MIDIDriver</string>
			<key>IOUserServerName</key>
			<string>$(PRODUCT_BUNDLE_IDENTIFIER)</string>
			<key>IOUserMIDIDriverUserClientProperties</key>
			<dict>
				<key>IOClass</key>
				<string>IOUserUserClient</string>
				<key>IOUserClass</key>
				<string>IOUserMIDIDriverUserClient</string>
			</dict>
		</dict>
```

Using the same `IOUserServerName` puts both services in one process. That is what lets `OSDynamicCast(NumarkNS7Device, provider)` and the `LOCALONLY` calls work.

- [ ] **Step 6: Wire the project file**

Edit `NumarkNS7Driver.xcodeproj/project.pbxproj`:

6a. In the `PBXBuildFile` section, after the `AA0000000000000000000102 … NumarkNS7Device.cpp in Sources` line:

```
		AA0000000000000000000111 /* NS7MIDIDriver.iig in Sources */ = {isa = PBXBuildFile; fileRef = AA0000000000000000000011 /* NS7MIDIDriver.iig */; };
		AA0000000000000000000112 /* NS7MIDIDriver.cpp in Sources */ = {isa = PBXBuildFile; fileRef = AA0000000000000000000012 /* NS7MIDIDriver.cpp */; };
```

6b. In the `PBXFileReference` section, after the `AA0000000000000000000002 … NumarkNS7Device.cpp` line:

```
		AA0000000000000000000011 /* NS7MIDIDriver.iig */ = {isa = PBXFileReference; lastKnownFileType = sourcecode.iig; path = NS7MIDIDriver.iig; sourceTree = "<group>"; };
		AA0000000000000000000012 /* NS7MIDIDriver.cpp */ = {isa = PBXFileReference; lastKnownFileType = sourcecode.cpp.cpp; path = NS7MIDIDriver.cpp; sourceTree = "<group>"; };
```

6c. In the `AA0000000000000000000302 /* Sources */` group `children`, after the `NumarkNS7Device.cpp` entry:

```
				AA0000000000000000000011 /* NS7MIDIDriver.iig */,
				AA0000000000000000000012 /* NS7MIDIDriver.cpp */,
```

6d. In `AA0000000000000000000401 /* Sources (dext) */` `files`, after the `NumarkNS7Device.cpp in Sources` entry:

```
				AA0000000000000000000111 /* NS7MIDIDriver.iig in Sources */,
				AA0000000000000000000112 /* NS7MIDIDriver.cpp in Sources */,
```

6e. In **both** dext build configurations (Debug and Release), change `DRIVERKIT_DEPLOYMENT_TARGET = 21.0;` to `25.0` (MIDIDriverKit first ships in DriverKit 25), and extend `OTHER_LDFLAGS`:

```
				OTHER_LDFLAGS = (
					"-framework",
					DriverKit,
					"-framework",
					USBDriverKit,
					"-framework",
					MIDIDriverKit,
				);
```

Command for 6e:

```bash
sed -i '' 's/DRIVERKIT_DEPLOYMENT_TARGET = 21.0;/DRIVERKIT_DEPLOYMENT_TARGET = 25.0;/' NumarkNS7Driver.xcodeproj/project.pbxproj
python3 - <<'EOF'
p='NumarkNS7Driver.xcodeproj/project.pbxproj'; s=open(p).read()
old='\t\t\t\t\t"-framework",\n\t\t\t\t\tUSBDriverKit,\n'
new=old+'\t\t\t\t\t"-framework",\n\t\t\t\t\tMIDIDriverKit,\n'
assert s.count(old)==2; s=s.replace(old,new); open(p,'w').write(s)
EOF
```

- [ ] **Step 7: Build**

Run: `xcodebuild -scheme NumarkNS7Driver -configuration Debug build CODE_SIGNING_ALLOWED=NO 2>&1 | grep -E "error|warning: |BUILD" | sort -u`
Expected: `** BUILD SUCCEEDED **` and no warnings.

If `iig` rejects `override LOCALONLY` on `StartIO`/`StopIO`, change both declarations to `virtual kern_return_t StartIO(OSArray * deviceList) LOCALONLY;` and `virtual kern_return_t StopIO() LOCALONLY;`, then rebuild. Those are the base-class spellings.

Also run `make -C Tests 2>&1 | tail -1` and expect `0 failures`.

- [ ] **Step 8: Commit**

```bash
git add NumarkNS7Driver/Sources/NS7MIDIDriver.iig NumarkNS7Driver/Sources/NS7MIDIDriver.cpp \
        NumarkNS7Driver/Sources/NumarkNS7Device.iig NumarkNS7Driver/Sources/NumarkNS7Device.cpp \
        NumarkNS7Driver/Info.plist NumarkNS7Driver.xcodeproj/project.pbxproj
git commit -m "Add NS7MIDIDriver: publish the NS7 to CoreMIDI, deliver MIDI in"
```

---

### Task 4: MIDI out on EP 0x04

**Files:**
- Modify: `NumarkNS7Driver/Sources/NumarkNS7Device.iig`, `NumarkNS7Driver/Sources/NumarkNS7Device.cpp`

- [ ] **Step 1: Declare the completion in `NumarkNS7Device.iig`**

In the `protected:` section, after the `MidiInComplete` declaration:

```cpp
    virtual void MidiOutComplete(OSAction * action, IOReturn status,
                                 uint32_t actualByteCount, uint64_t completionTimestamp)
        TYPE(IOUSBHostPipe::CompleteAsyncIO);
```

- [ ] **Step 2: Add constants and ivars in `NumarkNS7Device.cpp`**

In the anonymous namespace, after `constexpr uint32_t kLoggedMidiPerStats  = 100;`:

```cpp
constexpr uint32_t kMidiOutTimeoutMs    = 1000;
constexpr uint32_t kMidiOutStallRetries = 3;
```

In `struct NumarkNS7Device_IVars`, after `BulkSlot midiIn[kMidiInFlight] = {};`:

```cpp
    BulkSlot midiOut = {};
    uint32_t midiOutLength = 0;     // MIDI bytes in the packet in flight; 0 = idle
    uint32_t midiOutRetries = 0;    // resends of the current packet after a STALL
```

and after `uint64_t  midiMessages = 0;`:

```cpp
    uint64_t  midiOutBytes = 0;
```

- [ ] **Step 3: Add the sender**

Add below `CopyMidiClient` (from Task 3):

```cpp
static void
SendMidiOut(NumarkNS7Device_IVars * iv)
{
    kern_return_t ret = iv->pipes[kPipeMidiOut]->AsyncIO(iv->midiOut.buffer, NS7::kMidiPacketBytes,
                                                        iv->midiOut.action, kMidiOutTimeoutMs);
    if (ret != kIOReturnSuccess) {
        NoteError(iv, kPipeMidiOut, ret);
        iv->midiOutLength = 0;
    }
}

// Starts the next EP 0x04 transfer if none is in flight and CoreMIDI has
// queued bytes. Runs on the USB queue: from FeedbackComplete (~every 4 ms)
// and from MidiOutComplete (back to back while data remains).
static void
PumpMidiOut(NumarkNS7Device_IVars * iv)
{
    if (iv->stopping || iv->midiOutLength != 0 || iv->midiOut.action == nullptr) return;
    NS7MIDIDriver * client = CopyMidiClient(iv);
    if (client == nullptr) return;
    const uint32_t n = client->NextMidiOutPacket(iv->midiOut.ptr);
    client->release();
    if (n == 0) return;
    iv->midiOutLength = n;
    iv->midiOutRetries = 0;
    SendMidiOut(iv);
}
```

`NoteError`, `kPipeMidiOut` and `BulkSlot` already exist in this file. `NoteError` is defined above the streaming section, so place these functions after it: directly below `CopyMidiClient`, which sits above `OnMidiInUmp`.

- [ ] **Step 4: Add the completion handler**

Add after `IMPL(NumarkNS7Device, MidiInComplete)`:

```cpp
void
IMPL(NumarkNS7Device, MidiOutComplete)
{
    NumarkNS7Device_IVars * iv = ivars;
    (void)action;
    (void)actualByteCount;
    (void)completionTimestamp;
    if (iv->stopping) return;

    if (status == kUSBHostReturnPipeStalled && iv->midiOutRetries < kMidiOutStallRetries) {
        NoteError(iv, kPipeMidiOut, status);
        iv->stats[kPipeMidiOut].stalls++;
        iv->midiOutRetries++;
        if (iv->pipes[kPipeMidiOut]->ClearStall(true) == kIOReturnSuccess) {
            SendMidiOut(iv);   // resend the same packet
            return;
        }
    } else if (!TransferOk(status)) {
        NoteError(iv, kPipeMidiOut, status);
    } else {
        iv->stats[kPipeMidiOut].done++;
        iv->midiOutBytes += iv->midiOutLength;
    }
    iv->midiOutLength = 0;
    PumpMidiOut(iv);
}
```

- [ ] **Step 5: Allocate, start, stop and free the OUT slot**

5a. In `StartStreaming`, after the `midiIn` allocation loop and before `if (ret != kIOReturnSuccess) { Log("streaming setup failed…`, add:

```cpp
    if (ret == kIOReturnSuccess)
        ret = CreateBuffer(if0, NS7::kMidiPacketBytes, &iv->midiOut.buffer, &iv->midiOut.ptr);
    if (ret == kIOReturnSuccess)
        ret = self->CreateActionMidiOutComplete(sizeof(SlotRef), &iv->midiOut.action);
```

5b. At the end of `IMPL(NumarkNS7Device, FeedbackComplete)`, after `SubmitFeedback(iv, SlotIndex(action));`, add:

```cpp
    PumpMidiOut(iv);
```

5c. In `IMPL(NumarkNS7Device, Stop)`, after `for (auto & s : ivars->midiIn)   OSSafeReleaseNULL(s.action);` add:

```cpp
    OSSafeReleaseNULL(ivars->midiOut.action);
```

5d. In `NumarkNS7Device::free()`, after `ReleaseBulkSlots(ivars->midiIn, kMidiInFlight);` add:

```cpp
        ReleaseBulkSlots(&ivars->midiOut, 1);
```

- [ ] **Step 6: Add MIDI out to the stats line**

In `LogStats`, replace the `Log("stats: …` call with:

```cpp
    const PipeStats & o = iv->stats[kPipeMidiOut];
    Log("stats: playback %llu (%llu err, %llu resync) | feedback %llu pkts, %d frames/ms "
        "(%llu err, %llu resync) | capture %llu B (%llu err, %llu stall) | "
        "MIDI in %llu xfers, %llu msgs (%llu err, %llu stall) | "
        "MIDI out %llu pkts, %llu B (%llu err, %llu stall)",
        p.done, p.errors, p.resyncs, iv->feedbackPackets, iv->feedbackFrames, f.errors, f.resyncs,
        iv->captureBytes, c.errors, c.stalls, m.done, iv->midiMessages, m.errors, m.stalls,
        o.done, iv->midiOutBytes, o.errors, o.stalls);
```

- [ ] **Step 7: Build and run the tests**

Run: `xcodebuild -scheme NumarkNS7Driver -configuration Debug build CODE_SIGNING_ALLOWED=NO 2>&1 | grep -E "error|warning: |BUILD" | sort -u`
Expected: `** BUILD SUCCEEDED **`, no warnings.

Run: `make -C Tests 2>&1 | tail -1`
Expected: `… checks, 0 failures`.

- [ ] **Step 8: Commit**

```bash
git add NumarkNS7Driver/Sources/NumarkNS7Device.iig NumarkNS7Driver/Sources/NumarkNS7Device.cpp
git commit -m "Send CoreMIDI output to the NS7 on EP 0x04"
```

---

### Task 5: `ns7midi` CoreMIDI test tool

**Files:**
- Create: `Tools/ns7midi/ns7midi.cpp`, `Tools/ns7midi/Makefile`

- [ ] **Step 1: Create `Tools/ns7midi/Makefile`**

```make
CXX      ?= clang++
CXXFLAGS := -std=c++17 -Wall -Wextra -Werror -g
BUILD    := build

.PHONY: all clean

all: $(BUILD)/ns7midi

$(BUILD)/ns7midi: ns7midi.cpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) ns7midi.cpp -framework CoreMIDI -framework CoreFoundation -o $@

clean:
	rm -rf $(BUILD)
```

- [ ] **Step 2: Create `Tools/ns7midi/ns7midi.cpp`**

```cpp
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
        return 1;
    }
    std::printf("Monitoring \"%s\" for %.0f s. Move controls on the NS7.\n",
                StringProperty(source, kMIDIPropertyName).c_str(), seconds);
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, false);
    std::printf("%u packets\n", messages.load());
    MIDIClientDispose(client);
    return 0;
}

// Parses hex bytes into MIDI 1.0 channel voice messages as UMP type-2 words.
static bool ParseChannelVoice(int argc, const char * argv[], std::vector<UInt32> * words)
{
    std::vector<uint8_t> bytes;
    for (int i = 0; i < argc; i++) bytes.push_back(uint8_t(std::strtoul(argv[i], nullptr, 16)));
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
    for (UInt32 w : words) packet = MIDIEventListAdd(list, sizeof storage, packet, 0, 1, &w);
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
```

- [ ] **Step 3: Build it**

Run: `make -C Tools/ns7midi 2>&1 | grep -E "error|warning"; ./Tools/ns7midi/build/ns7midi list | head -5`
Expected: no errors. `list` prints whatever MIDI devices exist now (the NS7 may not be there yet).

- [ ] **Step 4: Commit**

```bash
git add Tools/ns7midi/ns7midi.cpp Tools/ns7midi/Makefile
git commit -m "Add ns7midi: CoreMIDI list/monitor/send tool for hardware checks"
```

---

### Task 6: Install and verify on the NS7 (needs the user at the hardware)

- [ ] **Step 1: Bump the build number and build signed**

```bash
sed -i '' 's/CURRENT_PROJECT_VERSION = 401;/CURRENT_PROJECT_VERSION = 402;/' NumarkNS7Driver.xcodeproj/project.pbxproj
xcodebuild -scheme NumarkNS7Installer -configuration Debug -allowProvisioningUpdates build 2>&1 | grep -E "error|BUILD" | sort -u
```
Expected: `** BUILD SUCCEEDED **`. If signing fails because the entitlements changed, show the user the exact error first; see the project memory "Dext signing & loading".

- [ ] **Step 2: Replace the installed app and activate**

```bash
APP=$(xcodebuild -scheme NumarkNS7Installer -configuration Debug -showBuildSettings 2>/dev/null | awk -F' = ' '/ BUILT_PRODUCTS_DIR /{print $2}')/NumarkNS7Installer.app
osascript -e 'tell application id "com.andrewabner.ns7" to quit' 2>/dev/null; sleep 1
rm -rf /Applications/NumarkNS7Installer.app && ditto "$APP" /Applications/NumarkNS7Installer.app
codesign --verify --deep --strict /Applications/NumarkNS7Installer.app && echo verify-ok
open /Applications/NumarkNS7Installer.app; sleep 3
osascript -e 'tell application "System Events" to tell process "NumarkNS7Installer"' -e 'set frontmost to true' -e 'click button 1 of group 1 of window 1' -e 'end tell'
sleep 6; systemextensionsctl list | tail -1
```
Expected: `verify-ok`, then `com.andrewabner.ns7.driverkit (4.0.0/402) … [activated enabled]`. If it says `waiting for user`, ask the user to approve it in System Settings.

- [ ] **Step 3: Start a log capture, then ask the user to replug the NS7**

```bash
/usr/bin/log stream --style compact --predicate 'eventMessage CONTAINS "NumarkNS7"' > "$SCRATCH/midi.log" 2>&1 &
```
(`$SCRATCH` is the session scratchpad; use the full `/usr/bin/log` path because zsh has a `log` builtin.) Ask the user to unplug and replug the NS7.

Check:
```bash
grep -E "NumarkNS7( MIDI)?: " "$SCRATCH/midi.log" | grep -v "MIDI in " | tail -20
ioreg -w0 -r -n "Numark USB Audio Device" | grep -E "\+-o" | head -6
```
Expected: `handshake done`, `streaming: …`, `NumarkNS7 MIDI: CoreMIDI device published`, and `StartIO: 0x00000000` once MIDIServer attaches. ioreg shows `NS7MIDIDriver` under `NumarkNS7Device`.

- [ ] **Step 4: CoreMIDI names**

Run: `./Tools/ns7midi/build/ns7midi list`
Expected, among the output:
```
device "Numark USB Audio Device"  manufacturer "Numark"  model "Numark USB Audio Device"  uniqueID …
    entity "MIDI"
      source      "MIDI"  uniqueID …  maxSysExSpeed 39000
      destination "MIDI"  uniqueID …  maxSysExSpeed 39000
```
If the device is missing, check the log for `creating CoreMIDI objects failed` or `AddObject(child)` messages, and check that `IOUserMIDIDriverUserClient` appears under `NS7MIDIDriver` in `ioreg -w0 -l -r -c IOUserService`.

- [ ] **Step 5: MIDI in**

Run `./Tools/ns7midi/build/ns7midi monitor 20` while the user moves knobs and spins a platter.
Expected: `UMP: 20B0xxxx` / `20E0xxxx` lines. The final count is > 0, and the next driver stats line shows `MIDI in … (0 err, 0 stall)` and `playback … (0 err …)`.

If CoreMIDI shows nothing but the driver's `MIDI in` log lines appear, `Send()` probably needs the MIDI work queue. Record that finding and apply the spec's fallback: in `DeliverMidiIn`, copy the words into an `OSData` and dispatch a block onto `GetWorkQueue()` that calls `Send`.

- [ ] **Step 6: MIDI out**

Run: `./Tools/ns7midi/build/ns7midi send 90 11 7F`, wait 1 s, then `./Tools/ns7midi/build/ns7midi send 90 11 00`.
Expected: `sent 1 message(s): ok` both times. Within ~10 s the stats line shows `MIDI out 2 pkts, 6 B (0 err, 0 stall)`. Ask the user whether any LED changed; that's a bonus, not a pass criterion.

- [ ] **Step 7: Stability**

Run `./Tools/ns7midi/build/ns7midi monitor 60` while the user spins a platter. In a second shell, send a burst:
```bash
for i in $(seq 1 50); do ./Tools/ns7midi/build/ns7midi send 90 11 7F 90 11 00 >/dev/null; done
```
Expected: the stats lines during the run show 0 errors and 0 stalls on every pipe, and no `dropped … MIDI out messages` lines.

- [ ] **Step 8: Record the results and commit**

Update the project memory file `ns7-midi-needs-init.md` with:
- whether `Send()` worked from the USB queue
- whether the child `AddObject` calls were needed
- whether any LED responded to `90 11 7F`

Then:
```bash
git add NumarkNS7Driver.xcodeproj/project.pbxproj
git commit -m "Bump build to 402 for CoreMIDI hardware test"
```
