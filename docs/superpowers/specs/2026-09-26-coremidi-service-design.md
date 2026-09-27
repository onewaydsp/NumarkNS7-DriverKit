# CoreMIDI service for the NS7 dext — design

Date: 2026-09-26
Status: approved in conversation; implementation plan to follow

## Goal

Publish the NS7 to CoreMIDI from the existing dext:

- a **source**: control changes, notes and pitch bends from the NS7 reach apps
- a **destination**: apps can send MIDI to the NS7 (e.g. LED feedback)

MIDI must keep flowing the way it already does on hardware. The NS7 only sends MIDI while its audio pipes stream, so `NumarkNS7Device` keeps streaming regardless of CoreMIDI state.

## Non-goals

- Audio (AudioDriverKit), which is a later step. Serato probably needs the NS7 audio device too, not just MIDI.
- MIDI 2.0 protocol. Endpoints are MIDI 1.0.
- A map of the NS7's LED notes.
- Serato certification or verification.

## Architecture

Two IOService classes run in the same dext process (same `IOUserServerName`):

| Class | Base | Matches on | Owns |
|---|---|---|---|
| `NumarkNS7Device` (exists) | `IOService` | `IOUSBHostDevice` 0x15E4/0x0071 | USB: handshake, all pipes, MIDI-in parsing, MIDI-out packets on EP 0x04 |
| `NS7MIDIDriver` (new) | `IOUserMIDIDriver` | `NumarkNS7Device` (new personality) | CoreMIDI: device, entity, source, destination |

The two talk only through `LOCALONLY` methods (plain types, so iig never has to parse `NS7Protocol.h`):

- `NumarkNS7Device::SetMidiClient(NS7MIDIDriver *)` registers the MIDI driver (retained), and `nullptr` unregisters it. The pointer is guarded by an `IOLock`; the USB side takes a retained copy per use.
- `NS7MIDIDriver::DeliverMidiIn(const uint32_t * words, uint32_t count)` is called on the USB queue with complete UMP messages.
- `NS7MIDIDriver::NextMidiOutPacket(uint8_t * packet)` is called on the USB queue. It fills one 42-byte EP 0x04 packet from the FIFO, which `NS7MIDIDriver` owns, and returns how many MIDI bytes it packed (0 = nothing to send).

The new personality needs `IOUserMIDIDriverUserClientProperties` (IOClass `IOUserUserClient`, IOUserClass `IOUserMIDIDriverUserClient`), as the MIDIDriverKit headers require. The dext already has the `com.apple.developer.driverkit.family.midi` entitlement.

Rejected alternatives:
- Making `NumarkNS7Device` itself the `IOUserMIDIDriver` would mix USB and CoreMIDI code, and it can't also inherit `IOUserAudioDriver` later.
- A separate dext for MIDI would need a user-client protocol to reach the pipes.

## Names (match Numark's original driver)

From the original CoreMIDI plugin (`Numark NS7 MIDI Driver.plugin`, `setUpEndpoints` @0x4ee8–0x513b):

- Device name = the product string from the kext; manufacturer = the vendor string from the kext. These are inferred to be the USB descriptor strings, which read **"Numark USB Audio Device"** and **"Numark"** on the hardware. The plugin only overrides the name for the V7 and Mixdeck.
- One entity named **"MIDI"** (the NS7 has one port: `kNumMIDIPorts = 1`, `kMIDIPortNames = ["MIDI"]`), with one source and one destination.
- MaxSysExSpeed = **39000** bytes/s, the value the plugin sets for 0x15E4/0x0071.

The driver reads the strings from the USB device at runtime. It falls back to the literals above if a string is missing. The device UID is built from VID, PID and USB `locationID`, so it stays stable across replugs on the same port.

## Data flow

### MIDI in (NS7 → CoreMIDI)

1. `MidiInComplete` (USB queue): `ExtractMidiIn` → `RawMidiToUmp::Push` → `client->DeliverMidiIn`.
2. `DeliverMidiIn` drops words unless CoreMIDI I/O has started (`StartIO` / `StopIO` on the `IOUserMIDIDevice`). Otherwise it calls `IOUserMIDISource::Send(words, n)`.
3. The MIDI-in parser state persists across transfers; messages split across packets are already handled and tested.

Open question, to be checked on hardware: whether `Send()` is safe to call from the USB queue rather than the MIDI driver's work queue. If it isn't, `DeliverMidiIn` moves each transfer's words onto `GetWorkQueue()` with one async dispatch per USB transfer.

### MIDI out (CoreMIDI → NS7)

1. The destination IO block (CoreMIDI real-time thread, which must not block or allocate) converts each UMP message with `UmpToRawMidi` and writes it into `MidiOutFifo`, **the whole message or nothing**.
2. `MidiOutFifo` is a single-producer/single-consumer byte ring of 4096 bytes. It uses atomic read and write indices and lives in `NS7Protocol.h`, so it is unit-tested.
3. The USB queue drains the FIFO. When no OUT transfer is in flight, it calls `NextMidiOutPacket`, which takes up to 39 bytes and calls `BuildMidiOutPacket` (42 bytes, `0xFD` fill, `0xE0` C-port). If that returns bytes, it runs `AsyncIO` on EP 0x04. On completion it sends the next packet straight away if more bytes are waiting, like the kext.
4. The chain is started from `FeedbackComplete`, which runs every ~4 ms. A write therefore waits at most ~4 ms before its first packet goes out. No extra timer.

## Error handling

| Condition | Action |
|---|---|
| FIFO has no room for a whole message | drop the message, count `midiOutDropped` |
| EP 0x04 STALL | `ClearStall(true)`, resend the same packet |
| EP 0x04 other error | drop the packet, count the error, continue with the next one |
| Aborted or stopping | no resubmit (existing `stopping` flag) |
| `NumarkNS7Device::Stop` | `SetMidiClient(nullptr)` first, stop draining |
| `NS7MIDIDriver::Stop` | `provider->SetMidiClient(nullptr)`, stop delivering, remove the CoreMIDI device |

The stats line gains MIDI-out counters: packets sent, bytes, errors, drops.

## Testing

Host unit tests in `Tests/NS7ProtocolTests.cpp`, written first:

- `MidiOutFifo`: push and pop, wrap-around, empty, and "rejects a message that doesn't fit whole"
- Drain into packets: 39-byte chunks, `0xE0` C-port, long runs split with no loss
- UMP into the FIFO: 1-word, 2-word and SysEx messages go in intact; unsupported UMP types are skipped

Build: dext and installer build with no warnings.

Hardware, with the new CoreMIDI CLI `Tools/ns7midi` (`list`, `monitor [s]`, `send <hex…>`):

1. `list` (and Audio MIDI Setup) shows "Numark USB Audio Device" / "Numark" / entity "MIDI" with one source and one destination.
2. `monitor`: moved controls show up, matching the driver log, with zero errors in the driver stats.
3. `send 90 11 7F`: the driver logs a successful EP 0x04 transfer and streaming stays error-free. A lit LED is a bonus, not a pass criterion.
4. Stability: 60 s of platter movement plus an output burst, with zero errors or stalls.
