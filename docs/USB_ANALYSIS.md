# Numark NS7 — USB Device Analysis
## Extracted from original Ploytec kext v3.3.11 (2016-09-29)

---

## USB Identification

| Field | Value | Notes |
|---|---|---|
| **idVendor** | `0x15E4` (5604 decimal) | Ploytec GmbH — OEM manufacturer for Numark |
| **idProduct** | `0x0071` (113 decimal) | Numark NS7 |
| Original bundle ID | `com.numark.ns7.usb` | |
| DriverKit bundle ID | `com.numark.ns7.driverkit` | |

---

## Original kext Structure (from pkg payload cpio archive)

```
NumarkNS7Audio.kext/
└── Contents/
    ├── Info.plist                  (3,292 bytes)
    └── MacOS/
        └── NumarkNS7Audio          (438,312 bytes — x86 Mach-O 32-bit)
```

The binary is **x86 32-bit only** (`cpu_type = 0x7`).  
Rosetta 2 **does not** translate kernel extensions — hence the need for this DriverKit rewrite.

---

## IOKit Matching Personality (from Info.plist)

```xml
<key>IOKitPersonalities</key>
<dict>
    <key>Numark</key>
    <dict>
        <key>CFBundleIdentifier</key>  <string>com.numark.ns7.usb</string>
        <key>IOClass</key>             <string>PGKernelDeviceNUMARKNS7</string>
        <key>IOMatchCategory</key>     <string>PGKernelDeviceNUMARKNS7</string>
        <key>IOProbeScore</key>        <integer>1000</integer>
        <key>IOProviderClass</key>     <string>IOUSBDevice</string>
        <key>idProduct</key>           <integer>113</integer>
        <key>idVendor</key>            <integer>5604</integer>
        <key>PtSpecific</key>
        <dict>
            <key>kIDRoot</key>         <string>com.numark.ns7</string>
            <key>kInACR</key>          <integer>8</integer>
            <key>kMIDIPortNames</key>  <array><string>MIDI</string></array>
            <key>kMsWait</key>         <integer>200</integer>
            <key>kNumMIDIPorts</key>   <integer>1</integer>
            <key>kConfigurationData</key> <data>...176 bytes...</data>
        </dict>
    </dict>
</dict>
```

### kConfigurationData Blob (decoded)

The 176-byte `kConfigurationData` blob is a Ploytec-proprietary binary structure  
describing the USB Audio Class 1.0 endpoint configuration. Decoded values:

```
Bytes 0x00-0x05:  0x18 0x02 0x00 0x01 ...  → USB Audio Terminal Descriptor prefix
Bytes 0x06-0x0B:  0x44 0xAC 0x00 0x00 ...  → Sample rate: 0x0000AC44 = 44100 Hz
Bytes repeating in 4 blocks: likely 4 channel descriptors (2 stereo pairs)

Inferred endpoint configuration:
  Interface 1, Alt 1: Isochronous IN  — 4ch, 24-bit, 44100 Hz
  Interface 2, Alt 1: Isochronous OUT — 4ch, 24-bit, 44100 Hz
  Interface 3:        MIDI Bulk       — endpoint 0x08
```

### kInACR = 8

This field likely encodes the MIDI bulk endpoint address (0x08).  
"ACR" may stand for "Asynchronous Control Request" — a Ploytec-internal term.

---

## Installed Components (from pkg sub-packages)

| Package | Bundle ID | Install Location | Size |
|---|---|---|---|
| `kext.pkg` | `com.numark.ns7.usb` | `/System/Library/Extensions/` | 434 KB |
| `skext.pkg` | `com.numark.ns7.signed.usb` | `/Library/Extensions/` | 451 KB |
| `hal.pkg` | `com.numark.ns7.hal` | `/Library/Audio/Plug-Ins/HAL/` | 888 KB |
| `coreaudio.pkg` | `com.numark.ns7.coreaudio` | `/Library/Audio/Plug-Ins/HAL/` | 888 KB |
| `midi.pkg` | `com.numark.ns7.midi` | `/Library/Audio/MIDI Drivers/` | 212 KB |
| `control.pkg` | `com.numark.ns7.control` | `/Applications/` | 387 KB |

**Duplicate kext explanation:**  
`kext.pkg` installed to `/System/Library/Extensions/` (pre-SIP location, macOS ≤10.11).  
`skext.pkg` installed to `/Library/Extensions/` (signed kext location, macOS 10.12+).  
Both contain the same `NumarkNS7Audio.kext` binary — only one is active at runtime.

---

## DriverKit Mapping (old → new)

| Original Component | DriverKit Replacement |
|---|---|
| `NumarkNS7Audio.kext` (IOUSBDevice kernel driver) | `NumarkNS7Driver.dext` (IOUSBHostDevice system extension) |
| `NumarkNS7AudioHAL.plugin` (CoreAudio HAL plugin) | `NumarkNS7AudioEngine` class + AudioDriverKit |
| `NumarkNS7Audio.driver` (CoreAudio driver) | Built into `NumarkNS7AudioEngine` via `IOUserAudioDriver` |
| `Numark NS7 MIDI Driver.plugin` (CoreMIDI driver) | `NumarkNS7MIDIDriver` class + MIDIDriverKit |
| `Numark NS7 USB Audio Panel.app` (control panel) | Not reproduced (EQ/FX settings are in Serato/Traktor) |

---

## Audio Format Details

The NS7 uses **USB Audio Class 1.0** (not 2.0).  
UAC 1.0 uses isochronous transfers with 1ms packet intervals.

```
Channels     : 4 (2 stereo pairs — Deck A L/R, Deck B L/R)
Bit depth    : 24-bit signed integer, little-endian, packed
Sample rate  : 44100 Hz (primary), 48000 Hz (secondary)
Packet size  : ⌈44100 / 1000⌉ × 4 channels × 3 bytes = 529–530 bytes/ms
Buffer used  : 576 bytes/ms (rounded up for ±1 sample jitter)
```

Conversion formula in `NumarkNS7Driver.cpp`:
```cpp
// 24-bit LE → float [-1.0, 1.0]
int32_t s = src[0] | (src[1]<<8) | (src[2]<<16);
if (s & 0x800000) s |= 0xFF000000;   // sign-extend
float f = (float)s / 8388608.0f;     // 2^23

// float → 24-bit LE
int32_t s = (int32_t)(clamp(f, -1.0f, 1.0f) * 8388607.0f);
dst[0]=s; dst[1]=s>>8; dst[2]=s>>16;
```

---

## Developer Signing Certificate (from pkg XAR archive)

The original installer was signed by:

```
Subject : Developer ID Installer: Ploytec GmbH (348SCJ68PR)
Issuer  : Developer ID Certification Authority
Valid   : 2012-05-16 → 2017-05-17  ← EXPIRED
Team ID : 348SCJ68PR
Country : DE (Germany)
```

The certificate expired in 2017. macOS Ventura+ will refuse to install the original pkg
without SIP disabled. The DriverKit replacement requires a current Developer ID certificate.

---

## Known Issues in Original Driver

From `Release Notes.txt` included in the zip:

> This driver is only compatible with OS 10.12. It is not backwards compatible.
>
> Known issue: Latency occurs after wake from sleep.
> Solution: Disable "put hard disk to sleep" / disable wake from sleep.

The DriverKit replacement avoids the sleep latency issue because DriverKit extensions
are managed by the OS and automatically re-matched when USB devices re-enumerate after wake.
