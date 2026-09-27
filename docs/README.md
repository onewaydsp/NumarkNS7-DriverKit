# Building the NS7 dext: an engineering log

This is a narrative of one working session on the Numark NS7 DriverKit
project — the hardware bring-up that got USB streaming working on real
hardware, the port into the dext, and the CoreMIDI service now in progress.
It is written for other developers picking up this codebase, not as a spec
(the specs live in `docs/superpowers/specs/` and `docs/superpowers/plans/`).

**What this session achieved:** replaced a stale USB-Audio-Class assumption
with the device's real (vendor-specific, Ploytec) protocol, reverse-engineered
and proved the startup handshake and streaming pattern against real hardware
with a bring-up tool, diagnosed and fixed a bulk-endpoint stall under load,
ported the working logic into the DriverKit extension, got the signed dext
loading and streaming with zero errors on an actual NS7, and started building
a CoreMIDI service on top of it.

## Where we started

The repository's original `README.md` and `docs/USB_ANALYSIS.md` were written
from Numark's Info.plist and a decoded `kConfigurationData` blob, and they
describe the NS7 as a **USB Audio Class 1.0 / USB MIDI Class 1.0** device: 4
channels of 24-bit audio over isochronous endpoints, a MIDI bulk endpoint at
`0x08`, sample rates of 44100/48000 Hz. `docs/USB_ANALYSIS.md` still records
that inference today; the top-level `README.md` was rewritten at the end of
this session to describe the real protocol below.

That assumption was wrong. An earlier session read the actual USB descriptors
off a real NS7 and found:

- **Both interfaces are vendor-specific (`bInterfaceClass = 0xFF`)**, not
  Audio or MIDI class. This is Ploytec's proprietary "BulkIn/IsocOut"
  protocol, the same one used by other Ploytec-OEM'd devices.
- The real endpoint layout, on **alternate setting 1** of each interface:

  | Interface | Endpoint | Direction/type | Role |
  |---|---|---|---|
  | IF0 | `0x02` | isochronous OUT | playback, 156 B/microframe |
  | IF0 | `0x83` | bulk IN | MIDI in |
  | IF0 | `0x04` | bulk OUT | MIDI out |
  | IF1 | `0x81` | isochronous IN | rate feedback, 1 ms |
  | IF1 | `0x86` | bulk IN | capture |

That same earlier session reverse-engineered Numark's original x86 kext
(`NumarkNS7Audio.kext` v3.3.11) and its CoreMIDI plugin to recover the control
handshake, audio packetisation, and MIDI framing, and wrote:

- `Tools/ns7probe` — a user-space bring-up tool built on `IOUSBHost.framework`,
  used to test protocol assumptions against real hardware before they go into
  the dext.
- `NumarkNS7Driver/Sources/NS7Protocol.h` — a header-only, DriverKit-free
  protocol library (descriptor validation, packet framing, MIDI parsing),
  covered by host unit tests in `Tests/`.

This session picked up from there: prove the protocol on hardware, then port
it into the dext.

## Hardware bring-up

### 1. MIDI needs the device to be streaming

The first hardware test — opening `Tools/ns7probe midi-in` and listening on
EP `0x83` with no other initialization — got no MIDI at all. The device
NAKs the bulk IN endpoint until something else wakes it up.

### 2. The control handshake alone isn't enough

`ns7probe init --send` replays the kext's startup control transfers. By
default (no `--send`) it only prints the exact setup-packet bytes, so every
request could be inspected before it went anywhere:

| bmRequestType | bRequest | wValue | wIndex | wLength | Purpose |
|---|---|---|---|---|---|
| `0xC0` (device, vendor, IN) | `0x56` | `0x0000` | `0x0000` | 8 | firmware/channel info — the device answers with only **5** bytes (`31 01 00 02 02`) even when 8 are requested |
| `0xC0` | `0x56` | `0x0000` | `0x0000` | 5 | same request, sized to match the real reply |
| `0xC0` | `0x49` | `0x0000` | `0x0000` | 1 | read register 0 — on a fresh device this already read `0x32` (clock + stream bits set) |
| `0x40` (device, vendor, OUT) | `0x49` | reg0 value | `0x0000` | 0 | write register 0 (value in `wValue`, no data stage) |
| `0xA2` (endpoint, class, IN) | `0x81` (`GET_CUR`) | `0x0100` (`SAMPLING_FREQ_CONTROL << 8`) | endpoint (`0x86` or `0x02`) | 3 | UAC1-style get sampling frequency |
| `0x22` (endpoint, class, OUT) | `0x01` (`SET_CUR`) | `0x0100` | endpoint (`0x86` or `0x02`) | 3 | set sampling frequency to 44100 Hz (`44 AC 00`, little-endian) |

The full sequence (`Handshake` in `Tools/ns7probe/ns7probe.mm`, mirrored later
in `NumarkNS7Device.cpp`): read firmware/channel info, select the internal
clock (register 0 `|= 0x02`), set 44.1 kHz on both `0x86` and `0x02`, then
enable streaming (register 0 `|= 0x30`: stream bit plus the "inputs ≤ 16"
bit).

Running this with `--send` and then listening on `0x83` again: **still no
MIDI.** Control requests alone don't unlock it.

### 3. MIDI only flows while audio streams

Adding `--stream` to the probe made it run the full set of pipes at once:
silent isochronous OUT on `0x02` (4–6 frames × 12 bytes per 125 µs
microframe, sized from the feedback), feedback reads on `0x81`, capture reads
on `0x86` (10,240-byte transfers), and 16 pending 42-byte MIDI reads on
`0x83`, all in flight together. MIDI arrived immediately once the pipes were
running.

The first attempt at this crashed the probe — a bug where it tried to change
the length of an already-allocated `IOUSBHost` I/O buffer, which can't be
resized. Once fixed, the observations were:

- MIDI in is a **raw MIDI 1.0 byte stream**, not USB-MIDI framing, packed 41
  bytes at a time into 42-byte bulk transfers with `0xFD` as filler. When
  idle, the device doesn't send filler packets at all — it NAKs instead.
- **Messages can split across packet boundaries.** Seen on hardware during a
  platter move and now a regression test
  (`test_real_split_pitch_bend_reassembles` in `Tests/MidiTests.cpp`):

  ```
  packet N:   B0 00 6A E0 00        (pitch bend status + 2 data bytes, then
                                      a running-status pitch bend missing its
                                      second data byte)
  packet N+1: 78                    (the missing data byte)

  → UMP: 0x20B0006A, 0x20E00078
  ```

  `RawMidiToUmp` in `NS7Protocol.h` keeps parser state across calls so a
  message split like this reassembles correctly.
- Feedback packets are 3 bytes; byte 0 is the frame count for that
  millisecond (44 or 45 frames), averaging to 44.1 kHz.
- Capture padding bytes are always zero.

### 4. Stability: feedback starvation stalls the bulk endpoints

Streams died reliably after roughly 360 ms, or sometimes after about 4.6 s.
Adding per-pipe first-error logging to the probe pinned it down:

- The feedback chain was only 2 requests × 4 ms deep (matching the original
  kext), and its completions shared a dispatch queue with heavy MIDI
  logging. Under that load the feedback chain fell behind the bus and a
  request came back "too old" to schedule (`kIOReturnIsoTooOld`,
  `0xE00002EE`).
- **Within half a millisecond** of that, the device STALLed its bulk IN
  endpoints (`0xE0005000`) — while the control endpoint stayed completely
  healthy. The inference (not proven, but consistent with every run): the
  NS7 stalls its bulk INs whenever the host stops servicing isochronous
  feedback promptly enough.

Fix, applied to both the probe and later the dext:

- **8** feedback requests in flight instead of 2 (`kFeedbackInFlight` in
  `NumarkNS7Device.cpp`, `kFbInFlight` in `ns7probe.mm`) — enough slack that
  falling behind under logging load doesn't starve the chain.
- Logging moved off the completion queue.
- Isochronous resync: if a request's planned start frame is no longer in the
  future, restart it `kIsoLeadFrames` (10) frames ahead of the current bus
  time instead of resubmitting a request the controller will reject
  (`NS7::NextIsoFrame`, unit-tested in `Tests/DeviceProtocolTests.cpp`).
- Stall recovery on bulk INs: `ClearStall(true)` followed by requeuing the
  same read (`RecoverBulk` in `NumarkNS7Device.cpp`, `RecoverStall` in the
  probe).

A subsequent 30-second run produced zero errors across every pipe.

## Porting to DriverKit

The engine proved in `ns7probe` was ported into `NumarkNS7Device`
(`NumarkNS7Driver/Sources/NumarkNS7Device.iig` / `.cpp`), an `IOService`
that claims the whole `IOUSBHostDevice`, matched on VID `0x15E4` / PID
`0x0071`:

- Same control handshake, same alternate-setting selection on both
  interfaces, same `kSettleMs` wait after `SET_INTERFACE`.
- `IsochIO`/`AsyncIO` completions delivered through `OSAction`, one action per
  in-flight slot, with the slot index carried in the action's reference
  (`SlotRef`).
- Same queue depths as the proven probe configuration: 4 playback, 8
  feedback, 3 capture, 16 MIDI-in requests in flight
  (`kPlaybackInFlight`, `kFeedbackInFlight`, `kCaptureInFlight`,
  `kMidiInFlight` in `NumarkNS7Device.cpp`).
- Same resync and stall-recovery logic as above.
- MIDI in is parsed to UMP (`NS7::RawMidiToUmp`) and logged; there was no
  CoreMIDI consumer yet at this point (`OnMidiInUmp` had a
  `// TODO: hand the UMP to the CoreMIDI source` comment).
- The device streams silence from `Start()` regardless of whether anything
  is consuming audio or MIDI, because the NS7 only emits MIDI while its
  audio pipes are running.

Two pure helpers were added test-first, in `NS7Protocol.h`, alongside the
existing protocol code:

- `FillPlaybackPacketSizes(feedbackFrames, slot, count, sizes)` — the 5/6
  frames-per-microframe pattern for a run of microframes, driven by the
  latest feedback reading.
- `NextIsoFrame(planned, now, lead)` — the resync decision described above.

## Signing & loading: lessons learned

Getting a signed dext onto real hardware surfaced several non-obvious
requirements. As a checklist for anyone repeating this:

- [ ] An Apple Developer account must be signed into Xcode, and the current
      Program License Agreement accepted — a stale acceptance blocks signing
      with an unhelpful error.
- [ ] Bundle identifiers must be your own, not Numark's placeholders. This
      project uses `com.andrewabner.ns7` (installer app) and
      `com.andrewabner.ns7.driverkit` (dext), team `TEK8D3YVA4`.
- [ ] The dext target's `PRODUCT_NAME` should be set to its own bundle
      identifier (`PRODUCT_NAME = "$(PRODUCT_BUNDLE_IDENTIFIER)"`) — DriverKit
      extensions are named after their bundle ID.
- [ ] Development provisioning profiles grant
      `com.apple.developer.driverkit.transport.usb` with `idVendor = "*"`
      (any vendor), and Xcode requires the entitlements file to match that
      exactly — so `NumarkNS7Driver.entitlements` also uses `"*"`, even
      though `Info.plist` still limits actual matching to `0x15E4`/`0x0071`.
      Distribution would need Apple to grant the specific vendor ID (5604)
      in place of `"*"`.
- [ ] The dext also needs `com.apple.developer.driverkit.family.midi` (and,
      later, `.family.audio`) declared in the entitlements.
- [ ] If the Mac was only just added to the developer team when a DriverKit
      profile was generated, the profile can be stale enough that AMFI
      rejects the extension at launch with **"No matching profile found"**
      (`-413`). Fix: delete the cached provisioning profile, rebuild with
      `-allowProvisioningUpdates`, and bump `CURRENT_PROJECT_VERSION` so the
      new build is unambiguously newer.
- [ ] After enabling the extension in System Settings, macOS may keep using
      its generic composite driver for the already-attached device — **replug
      the NS7** so it re-enumerates against the new dext.
- [ ] In `zsh`, `log` is a shell builtin; use `/usr/bin/log stream …` or the
      builtin silently does the wrong thing.

## Results on hardware

With the ported dext running, all five pipes streamed simultaneously with
zero errors:

| Pipe | Measured rate | Notes |
|---|---|---|
| Playback (EP `0x02`) | 250 transfers/s | exactly real time (4 ms per transfer) |
| Feedback (EP `0x81`) | 1000 packets/s | steady 44 frames/ms |
| Capture (EP `0x86`) | 2.82 MB/s | = 44,100 samples/s × 64 bytes/frame |
| MIDI in (EP `0x83`) | ~21,800 messages / 20 s | while platters were moved |

Register 0 went `0x12 → 0x32` across the handshake (internal clock already
set at `0x12`; the write added the stream-enable and inputs-≤16 bits).

## CoreMIDI service — status

Design: `docs/superpowers/specs/2026-09-26-coremidi-service-design.md`.
Plan: `docs/superpowers/plans/2026-09-26-coremidi-service.md`.

The plan is being executed with subagent-driven development: a fresh
implementer per task, followed by a spec-compliance review and then a
code-quality review, each as a separate subagent.

**Architecture.** A new `NS7MIDIDriver : IOUserMIDIDriver` service runs in the
same dext process as `NumarkNS7Device` (same `IOUserServerName`), matched on
`NumarkNS7Device` itself rather than on the USB device directly. It publishes
one CoreMIDI device named **"Numark USB Audio Device"** (manufacturer
**"Numark"**) — names taken from Numark's original CoreMIDI plugin binary so
that software like Serato has the best chance of recognizing the port — with
one entity **"MIDI"**, one source, one destination, and `MaxSysExSpeed`
39000 bytes/s. `NumarkNS7Device` keeps owning all USB I/O; the two services
talk only through `LOCALONLY` (in-process, plain-C++) methods so the `iig`
tool never needs to parse `NS7Protocol.h`.

MIDI out crosses from CoreMIDI's real-time callback thread (which must not
block or allocate) to the USB queue through a lock-free single-producer/
single-consumer byte FIFO.

**Done:**
- **Task 1 — `ByteFifo<N>`** (`NumarkNS7Driver/Sources/NS7Protocol.h`): a
  4096-byte ring (`MidiOutFifo`) with atomic head/tail indices, all-or-nothing
  writes (a MIDI message is never split by a full buffer), zero-initialized
  by `IONewZero` as a valid empty FIFO. Reviewed and approved.
- **Task 2 — `QueueUmpAsRawMidi` / `NextMidiOutPacket`** (same file): converts
  UMP words to raw MIDI 1.0 bytes and queues each message whole; fills one
  42-byte EP `0x04` packet from the FIFO. Code review found that the current
  signature — `QueueUmpAsRawMidi(words, count, fifo)` with no persistent
  state — can drop one SysEx UMP packet independently of its siblings, which
  would leak orphan SysEx data bytes onto the wire. A per-destination state
  object that keeps SysEx framing intact across drops is being added before
  this is wired into the MIDI driver; the design's `NS7::UmpOutState` /
  4-argument `QueueUmpAsRawMidi(words, count, fifo, state)` in the plan
  reflects that fix but is not yet implemented in `NS7Protocol.h`.

**In progress:** the SysEx-safe queuing fix above.

**Remaining (Tasks 3–6 of the plan):**
- The `NS7MIDIDriver` service itself (`NS7MIDIDriver.iig`/`.cpp`, a new
  `IOKitPersonalities` entry, project wiring, `DRIVERKIT_DEPLOYMENT_TARGET`
  bumped to 25.0 for `MIDIDriverKit`) and MIDI-in delivery from
  `NumarkNS7Device`.
- MIDI out on EP `0x04` (`SendMidiOut`/`PumpMidiOut`/`MidiOutComplete` in
  `NumarkNS7Device.cpp`), driven off the existing feedback completion so a
  write waits at most ~4 ms for its first packet.
- A CoreMIDI CLI, `Tools/ns7midi` (`list`, `monitor [s]`, `send <hex…>`), for
  hardware verification without Serato or Audio MIDI Setup.
- A hardware test pass (Task 6 of the plan): confirm the device/entity names
  in Audio MIDI Setup, confirm MIDI in/out over CoreMIDI, and a 60-second
  stability run with concurrent platter movement and an output burst.

Serato will very likely also need the NS7's **audio** device
(AudioDriverKit), not just MIDI — that's explicitly out of scope for this
service and is a later milestone.

## Open questions & risks

- **`IOUserMIDISource::Send()` thread safety.** The plan calls `Send()`
  directly from the USB completion queue (`DeliverMidiIn` in
  `NS7MIDIDriver.cpp`, not yet built). Whether that's actually safe, versus
  needing to hop onto the MIDI driver's own work queue
  (`GetWorkQueue()`) first, is unverified and explicitly flagged as
  something to check on hardware in Task 6 of the plan.
- **Whether CoreMIDI child objects need `AddObject`.** The plan's draft
  `CreateMidiObjects` calls `self->AddObject()` on the device and then, out
  of uncertainty, on the entity/source/destination too, tolerating an
  "already added" failure. The MIDIDriverKit headers don't say which is
  correct; this needs confirming once the service actually runs.
- **`iig` and `override LOCALONLY`.** The plan flags that `iig` may reject
  `override LOCALONLY` on methods like `StartIO`/`StopIO`, with a fallback
  spelling (`LOCALONLY` without `override`) noted in Task 3, Step 7 —
  unverified until that code is built.
- **Serato likely needs the audio device too**, not just MIDI. Getting MIDI
  working is necessary but probably not sufficient for Serato support.
- **The USB transport entitlement (`idVendor = "*"`) only works for
  development.** Shipping to other users' machines needs Apple to grant the
  specific vendor ID (5604) — a distribution-time dependency this project
  doesn't control yet.
- **`docs/USB_ANALYSIS.md` is stale.** It still describes the device as USB
  Audio/MIDI Class 1.0 and hasn't been rewritten to reflect the
  vendor-specific protocol documented here and in
  `NumarkNS7Driver/Sources/NS7Protocol.h`. (The top-level `README.md` has been
  rewritten.)

## How this was built

The whole session was run with Claude Code, following a few explicit
practices baked into this repo's workflow (see `docs/superpowers/`):

- **Test-driven development** for every piece of pure protocol logic:
  `NS7Protocol.h`'s parsing, framing, scheduling and FIFO code all has
  failing tests written first in `Tests/`, which is a host-side
  (non-DriverKit) build runnable with `make -C Tests` under ASan and UBSan
  (`make -C Tests tsan` under ThreadSanitizer, `make -C Tests coverage` for
  line/branch coverage of `NS7Protocol.h`).
- **Subagent-driven development** for the CoreMIDI service: each plan task
  is handed to a fresh implementer subagent with no memory of the others,
  then to a separate spec-compliance reviewer, then to a separate
  code-quality reviewer — catching issues (like the SysEx-drop bug above)
  that a single continuous session might rationalize past.
- **Hardware-first verification** for anything that can't be unit tested:
  `Tools/ns7probe` proved the USB protocol against a real NS7 before any of
  it went into the dext, and the plan's Task 6 repeats that pattern for
  CoreMIDI with `Tools/ns7midi`.
- **Archify** (a Claude Code skill) generated the interactive system
  architecture diagram embedded in the top-level `README.md`; its source
  data lives in `docs/architecture/`.

## Relevant files

- `NumarkNS7Driver/Sources/NS7Protocol.h` — hardware-independent protocol
  code: descriptor validation, playback packetisation, iso scheduling,
  feedback/capture decoding, MIDI framing and MIDI↔UMP conversion, the MIDI
  out FIFO.
- `NumarkNS7Driver/Sources/NumarkNS7Device.iig` /
  `NumarkNS7Driver/Sources/NumarkNS7Device.cpp` — the DriverKit USB service:
  handshake, all five pipes, stall recovery, MIDI-in parsing.
- `Tools/ns7probe/ns7probe.mm` — the user-space bring-up tool used to prove
  the protocol on hardware before it went into the dext.
- `Tests/` — host unit tests for `NS7Protocol.h`: `DeviceProtocolTests.cpp`
  (descriptors, audio, control), `MidiTests.cpp` (MIDI framing and
  conversion, FIFO), `PropertyTests.cpp` (seeded randomized properties),
  `FifoConcurrencyTests.cpp` (two-thread FIFO), and
  `MidiOutStateMachineTests.cpp`, sharing `TestHarness.h`.
- `docs/USB_ANALYSIS.md` — the original (now superseded) USB Audio Class
  analysis.
- `docs/superpowers/specs/2026-09-26-coremidi-service-design.md` — the
  CoreMIDI service design.
- `docs/superpowers/plans/2026-09-26-coremidi-service.md` — the task-by-task
  implementation plan for the CoreMIDI service.
- `docs/architecture/` — source data for the architecture diagram in the
  top-level `README.md`.
