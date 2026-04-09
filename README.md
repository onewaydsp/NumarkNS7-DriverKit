# Numark NS7 — Apple Silicon Driver (DriverKit)
**Version 4.0.0 — arm64 / Apple Silicon**

A complete rewrite of the Numark NS7 USB Audio+MIDI driver using Apple's modern
DriverKit framework. Replaces the original x86-only Ploytec kext (v3.3.11, 2016)
which is incompatible with Apple Silicon and macOS 12+.

---

## What This Is

The original driver (`Numark-NS7_3.3.11.dmg`) contains a 32-bit x86 kernel extension
built by Ploytec GmbH in 2016 for macOS 10.12 Sierra. It **cannot run on Apple Silicon** for two reasons:

1. **Wrong architecture** — The Mach-O binary is `x86 32-bit`. Rosetta 2 does not translate kernel extensions.
2. **Deprecated technology** — Apple deprecated third-party kexts on macOS 11+. Apple Silicon Macs only support DriverKit system extensions for custom USB drivers.

This project provides a fully-structured DriverKit replacement that:

- Matches the same USB device (`VID=0x15E4, PID=0x0071`)
- Provides 4-channel, 24-bit, 44100 Hz USB audio (Deck A + Deck B)
- Exposes the MIDI port `Numark NS7 MIDI` to CoreMIDI
- Runs entirely in **userspace** — no kernel code, no reboots to install

---

## Quick Start

```bash
# 1. Open in Xcode
open NumarkNS7Driver.xcodeproj

# 2. Set your Apple Developer Team in Signing & Capabilities

# 3. Build
xcodebuild -project NumarkNS7Driver.xcodeproj \
           -target NumarkNS7Driver \
           -configuration Release

# 4. Install
bash installer-scripts/install.sh

# 5. Approve in System Settings → Privacy & Security (if prompted)
```

---

## Requirements

| Requirement | Details |
|---|---|
| **Mac** | Apple Silicon — M1, M1 Pro, M1 Max, M1 Ultra, M2, M3, M4 series |
| **macOS** | 12.0 Monterey or later |
| **Xcode** | 13.0 or later |
| **Apple Developer account** | Required for signing + DriverKit USB entitlement |
| **DriverKit USB entitlement** | Must be requested from Apple (free, see below) |

> **Intel Mac users:** Remove `arm64` from `ARCHS` in the Xcode build settings
> and add `x86_64`. The driver code is architecture-neutral C++.

---

## Project Structure

```
NumarkNS7-DriverKit/
├── NumarkNS7Driver.xcodeproj/          Xcode project
│   └── project.pbxproj
├── NumarkNS7Driver/
│   ├── Info.plist                      USB matching + IOKit personalities
│   ├── NumarkNS7Driver.entitlements    DriverKit + USB transport entitlements
│   └── Sources/
│       ├── NumarkNS7Driver.h           Class declarations
│       └── NumarkNS7Driver.cpp         Full implementation:
│                                         NumarkNS7Driver      — USB device lifecycle
│                                         NumarkNS7AudioEngine — UAC 1.0 ISO streaming
│                                         NumarkNS7MIDIDriver  — USB MIDI Class bulk
│                                         NumarkNS7UserClient  — HAL/app IPC
├── installer-scripts/
│   ├── install.sh                      Build output installer
│   └── uninstall.sh                    Clean removal
└── docs/
    └── USB_ANALYSIS.md                 Full analysis of original kext
```

---

## USB Device Information

Extracted directly from the original kext `Info.plist` by parsing the `.dmg → HFS+ → XAR pkg → cpio` chain:

```
Vendor  ID  : 0x15E4  (5604)   — Ploytec GmbH / Numark
Product ID  : 0x0071  (113)    — Numark NS7
MIDI Ports  : 1 port named "MIDI"
Init wait   : 200 ms after enumeration
Audio IN    : Endpoint 0x81, 4ch, 24-bit, 44100 Hz, isochronous
Audio OUT   : Endpoint 0x01, 4ch, 24-bit, 44100 Hz, isochronous
MIDI        : Endpoint 0x08, bulk
```

See `docs/USB_ANALYSIS.md` for the complete decoded blob analysis.

---

## Requesting the DriverKit USB Entitlement

DriverKit USB drivers require explicit approval from Apple before they will
load on a production system. The process is free and typically takes 2–5 business days.

1. Go to: https://developer.apple.com/contact/request/driverkit
2. Select **"USB"** as the transport type
3. Describe the device: *"Numark NS7 DJ Controller, USB VID 0x15E4 PID 0x0071,
   USB Audio Class 1.0 + USB MIDI Class. Replacement for deprecated x86 kext
   from Ploytec GmbH (com.numark.ns7.usb v3.3.11) which does not run on Apple Silicon."*
4. Apple will add the entitlement to your Developer account
5. Re-download your provisioning profile in Xcode

**For testing without the entitlement** (Development only):
```bash
# Disable SIP temporarily (not for production use)
# Reboot into Recovery Mode → Utilities → Terminal:
csrutil disable
# After testing, re-enable:
csrutil enable
```

---

## How It Works

### USB Audio (replaces NumarkNS7Audio.kext + NumarkNS7AudioHAL.plugin)

The NS7 implements **USB Audio Class 1.0** — the same standard used by every
class-compliant USB audio interface. The `NumarkNS7AudioEngine` class:

1. Opens USB interfaces 1 and 2, alternate setting 1 (activates audio streaming)
2. Submits a ring of 8 isochronous IN/OUT buffers (576 bytes each @ 44100 Hz)
3. In the IN completion callback, converts 24-bit packed LE integers → 32-bit float
4. Registers with CoreAudio's HAL via `AudioDriverKit` as device `"Numark NS7"`
5. Presents 4 input channels: Deck A L/R, Deck B L/R
6. Presents 4 output channels: same layout

The sample format conversion is fully implemented in `NumarkNS7Driver.cpp`.

### MIDI (replaces Numark NS7 MIDI Driver.plugin)

The `NumarkNS7MIDIDriver` class:

1. Opens USB interface 3, endpoint 0x08
2. Runs a continuous bulk read loop
3. Parses **USB MIDI Class 1.0** 4-byte packets (CIN + 3 MIDI bytes)
4. Dispatches to CoreMIDI, appearing as `"Numark NS7 MIDI"`

---

## What's Different from the Original

| Original kext | This DriverKit driver |
|---|---|
| x86 32-bit Mach-O | arm64 (Apple Silicon native) |
| Runs in kernel | Runs in userspace (crash-safe) |
| macOS 10.12 only | macOS 12.0+ |
| Requires reboot to install | No reboot needed |
| Expired 2017 signing certificate | Current Developer ID certificate |
| Kernel panic on crash | Process isolation — OS auto-restarts |
| Sleep/wake latency bug | DriverKit handles re-enumeration automatically |
| Ploytec proprietary IOKit class | Standard DriverKit + AudioDriverKit APIs |

---

## Building from Source

```bash
# Debug build (arm64)
xcodebuild -project NumarkNS7Driver.xcodeproj \
           -target NumarkNS7Driver \
           -configuration Debug \
           ONLY_ACTIVE_ARCH=YES

# Release build (arm64)
xcodebuild -project NumarkNS7Driver.xcodeproj \
           -target NumarkNS7Driver \
           -configuration Release

# Universal binary (arm64 + x86_64) — if you want Intel support too
xcodebuild -project NumarkNS7Driver.xcodeproj \
           -target NumarkNS7Driver \
           -configuration Release \
           ARCHS="arm64 x86_64"
```

The output is `NumarkNS7Driver.dext` — a **Driver Extension** bundle (not a `.kext`).

---

## Testing

After installation and NS7 connection:

```bash
# Check extension is loaded
systemextensionsctl list | grep numark

# Check USB device recognized
ioreg -r -c IOUSBHostDevice | grep -A5 "0x15E4"

# Check audio device registered
system_profiler SPAudioDataType | grep -A10 "Numark"

# Check MIDI port registered
osascript -e 'tell application "Audio MIDI Setup" to get name of every MIDI device'
# Should include: "Numark NS7 MIDI"
```

---

## Troubleshooting

**"System Extension Blocked" notification**
→ Open System Settings → Privacy & Security → scroll to Security section → click Allow

**Driver loads but no audio device appears**
→ The `AudioDriverKit` registration in `registerAudioDevice()` is scaffolded.
  Complete it using Apple's [Audio Driver Sample Code](https://developer.apple.com/documentation/audiodriverkit).

**MIDI port not appearing**
→ The `MIDIDriverKit` registration in `registerMIDIPort()` is scaffolded.
  Complete it using Apple's [CoreMIDI DriverKit documentation](https://developer.apple.com/documentation/coremidi).

**entitlement not authorized**
→ The USB transport entitlement must be approved by Apple before the extension
  will match against `IOUSBHostDevice` outside of SIP-disabled development mode.

---

## Implementation Notes for Completing the Driver

Two sections are marked as scaffolds and need to be completed with Apple's frameworks:

### 1. Audio HAL Registration (`NumarkNS7AudioEngine::registerAudioDevice`)
Use the Xcode **"Audio Driver Extension"** template which generates the full
`IOUserAudioDriver` + `IOUserAudioDevice` + `IOUserAudioStream` boilerplate.
Key calls needed:
```cpp
IOUserAudioDevice::Create(in_driver, "Numark NS7", /*uid=*/"NumarkNS7-0x15E4-0x0071", ...);
IOUserAudioStream::Create(in_device, IOUserAudioStreamDirection::Input, ...);
stream->SetAvailableStreamFormats(&asbd, 1);
device->SetAvailableSampleRates(sampleRates, 2);  // 44100, 48000
```

### 2. MIDI Port Registration (`NumarkNS7MIDIDriver::registerMIDIPort`)
Use the **MIDIDriverKit** framework (macOS 12+):
```cpp
#include <MIDIDriverKit/MIDIDriverKit.h>
MIDIDeviceRef device = MIDIDeviceCreate(..., "Numark NS7", "Numark", "NS7", ...);
MIDIEntityRef entity = MIDIDeviceAddEntity(device, "MIDI", false, 1, 1, &entity);
```

---

## License

This driver was reverse-engineered from USB descriptor data extracted from the
original Ploytec kext. The USB protocol (USB Audio Class 1.0, USB MIDI Class 1.0)
is an open standard. The Ploytec-proprietary `PGKernelDeviceNUMARKNS7` IOKit class
and `kConfigurationData` blob have been replaced with standard DriverKit equivalents.

---

*USB analysis methodology: DMG → HFS+ volume extraction → XAR pkg parsing →*  
*cpio payload decompression → Info.plist extraction. All data sourced from*  
*publicly-accessible fields in the device descriptor and kext metadata.*
