// NumarkNS7Driver.h
// Numark NS7 USB Audio/MIDI DriverKit Extension
//
// Replaces the original x86-only IOKit kext (com.numark.ns7.usb v3.3.11)
// with a modern DriverKit system extension that runs natively on Apple Silicon.
//
// Original device:  Numark NS7 DJ Controller
// USB Vendor  ID :  0x15E4 (Ploytec GmbH / Numark)
// USB Product ID :  0x0071
// USB Protocol  :  USB Audio Class 1.0 (isochronous) + USB MIDI Class
//
// Architecture:
//   ┌────────────────────────────────────────────────────────────┐
//   │  NumarkNS7Driver (this file)                                │
//   │  ├── IOUSBHostDevice matching                               │
//   │  ├── NumarkNS7AudioEngine  — USB Audio Class 1.0 streams    │
//   │  │    ├── Isochronous IN  (NS7 → Mac, 4ch, 24-bit, 44.1k)  │
//   │  │    └── Isochronous OUT (Mac → NS7, 4ch, 24-bit, 44.1k)  │
//   │  └── NumarkNS7MIDIDriver  — USB MIDI Class port "MIDI"      │
//   └────────────────────────────────────────────────────────────┘
//
// Build requirements:
//   • Xcode 13+
//   • macOS 12.0+ SDK
//   • Apple Developer account with DriverKit USB transport entitlement
//   • Target: arm64 (Apple Silicon); add x86_64 for universal if needed

#ifndef NumarkNS7Driver_h
#define NumarkNS7Driver_h

#include <DriverKit/DriverKit.h>
#include <DriverKit/IOUserClient.h>
#include <USBDriverKit/USBDriverKit.h>
#include <AudioDriverKit/AudioDriverKit.h>

// ─── Constants ────────────────────────────────────────────────────────────────

// USB identifiers (from original kext IOKitPersonalities)
static constexpr uint16_t kNumarkVendorID  = 0x15E4;   // Ploytec GmbH / Numark
static constexpr uint16_t kNS7ProductID    = 0x0071;   // NS7

// USB Audio endpoint addresses (decoded from kConfigurationData blob)
static constexpr uint8_t  kAudioINEndpoint  = 0x81;    // Isochronous IN  (device→host)
static constexpr uint8_t  kAudioOUTEndpoint = 0x01;    // Isochronous OUT (host→device)
static constexpr uint8_t  kMIDIEndpoint     = 0x08;    // Bulk endpoint for MIDI

// Audio format
static constexpr uint32_t kSampleRate       = 44100;
static constexpr uint32_t kChannelCount     = 4;       // 2 stereo pairs (Deck A + Deck B)
static constexpr uint32_t kBitDepth         = 24;
static constexpr uint32_t kBytesPerFrame    = (kBitDepth / 8) * kChannelCount;

// ISO transfer parameters
// USB Audio Class 1.0: 1 ms isochronous packets at full-speed
// Packet size = sample_rate / 1000 * bytes_per_frame = 44.1 * 12 ≈ 529 bytes/ms
// Round up to 576 to handle +1 samples/ms jitter
static constexpr uint32_t kISOPacketSize    = 576;
static constexpr uint32_t kNumISOBuffers    = 8;       // ring buffer depth

// MIDI
static constexpr uint32_t kMIDIPortCount    = 1;
static constexpr uint32_t kMIDIBulkSize     = 64;

// Timing: milliseconds to wait after USB enumeration (original kMsWait)
static constexpr uint32_t kInitWaitMs       = 200;


// ─── NumarkNS7Driver ─────────────────────────────────────────────────────────
//
// Top-level driver object. Matched by IOKit against IOUSBHostDevice when the
// NS7 is plugged in. Owns the device pipe and spawns the audio/MIDI engines.

class NumarkNS7Driver : public IOService
{
    // DriverKit macro: generates the OS_OBJECT boilerplate
    OSDeclareDefaultStructors(NumarkNS7Driver);

public:
    // ── IOService lifecycle ──────────────────────────────────────────────────

    /// Called when the NS7 USB device is matched. Opens the USB device,
    /// waits kInitWaitMs, then starts the audio and MIDI engines.
    virtual kern_return_t Start(IOService * provider) override;

    /// Called when the NS7 is unplugged or the driver is unloaded.
    /// Stops all engines and releases USB resources.
    virtual kern_return_t Stop(IOService * provider) override;

    // ── User client support (for the host app) ───────────────────────────────

    /// Creates an IOUserClient when an application opens a connection.
    /// Used by the control panel app and CoreAudio HAL server.
    virtual kern_return_t NewUserClient(uint32_t type,
                                        IOUserClient ** userClient) override;

private:
    // The matched USB device
    IOUSBHostDevice * _device     = nullptr;

    // Child service objects (owned, start()ed as children of this driver)
    IOService       * _audioEngine = nullptr;
    IOService       * _midiDriver  = nullptr;

    // Helpers
    kern_return_t openDevice(IOService * provider);
    kern_return_t startEngines();
    void          stopEngines();
};


// ─── NumarkNS7AudioEngine ────────────────────────────────────────────────────
//
// Handles USB Audio Class 1.0 isochronous streaming.
//
// The NS7 presents a standard USB Audio Class 1.0 interface:
//   Interface 1, Alternate 1: 4-channel, 24-bit, 44100 Hz isochronous IN
//   Interface 2, Alternate 1: 4-channel, 24-bit, 44100 Hz isochronous OUT
//
// This engine registers with CoreAudio's HAL via AudioDriverKit, replacing
// both the original NumarkNS7AudioHAL.plugin and NumarkNS7Audio.driver.

class NumarkNS7AudioEngine : public IOService
{
    OSDeclareDefaultStructors(NumarkNS7AudioEngine);

public:
    virtual kern_return_t Start(IOService * provider) override;
    virtual kern_return_t Stop(IOService * provider) override;

    // ── Audio device registration ─────────────────────────────────────────

    /// Sets up the CoreAudio HAL device with the correct channel layout,
    /// sample rate, and buffer sizes. Called from Start().
    kern_return_t registerAudioDevice();

    /// Called by CoreAudio when the HAL wants to begin streaming.
    /// Starts isochronous transfers on both IN and OUT pipes.
    kern_return_t startIO();

    /// Called by CoreAudio when the HAL wants to stop streaming.
    kern_return_t stopIO();

    // ── Isochronous callbacks ─────────────────────────────────────────────

    /// Completion callback for isochronous IN (NS7→Mac audio).
    /// Converts USB Audio Class 1.0 packed 24-bit LE to CoreAudio 32-bit float
    /// and hands the buffer to the HAL.
    void isoInComplete(IOUSBHostIsochronousFrame * frameList,
                       uint32_t                    frameCount,
                       IOMemoryDescriptor *        buffer);

    /// Completion callback for isochronous OUT (Mac→NS7 audio).
    /// Converts CoreAudio 32-bit float to USB Audio Class 1.0 packed 24-bit LE.
    void isoOutComplete(IOUSBHostIsochronousFrame * frameList,
                        uint32_t                    frameCount,
                        IOMemoryDescriptor *        buffer);

private:
    IOUSBHostDevice    * _device    = nullptr;
    IOUSBHostInterface * _audioINIface  = nullptr;
    IOUSBHostInterface * _audioOUTIface = nullptr;
    IOUSBHostPipe      * _isoINPipe     = nullptr;
    IOUSBHostPipe      * _isoOUTPipe    = nullptr;

    // Circular ISO frame ring buffers
    IOBufferMemoryDescriptor * _inBuffers[kNumISOBuffers]  = {};
    IOBufferMemoryDescriptor * _outBuffers[kNumISOBuffers] = {};
    uint32_t _inBufIndex  = 0;
    uint32_t _outBufIndex = 0;

    bool _streaming = false;

    kern_return_t openAudioInterfaces();
    kern_return_t allocateISOBuffers();
    void          freeISOBuffers();
    void          submitNextISOIn();
    void          submitNextISOOut();

    // Sample format conversion helpers
    static void convertUSBAudioToCoreAudio(const uint8_t  * src,
                                            float          * dst,
                                            uint32_t         frameCount,
                                            uint32_t         channelCount);

    static void convertCoreAudioToUSBAudio(const float    * src,
                                            uint8_t        * dst,
                                            uint32_t         frameCount,
                                            uint32_t         channelCount);
};


// ─── NumarkNS7MIDIDriver ─────────────────────────────────────────────────────
//
// Handles USB MIDI Class bulk transfers.
//
// The NS7 exposes one MIDI port named "MIDI" (was kMIDIPortNames in original
// kext). Bulk endpoint 0x08 carries standard USB MIDI Class 1.0 packets.
//
// This replaces the original "Numark NS7 MIDI Driver.plugin" CoreMIDI driver.

class NumarkNS7MIDIDriver : public IOService
{
    OSDeclareDefaultStructors(NumarkNS7MIDIDriver);

public:
    virtual kern_return_t Start(IOService * provider) override;
    virtual kern_return_t Stop(IOService * provider) override;

    // ── MIDI port management ──────────────────────────────────────────────

    /// Registers the single MIDI port with CoreMIDI. The port is named
    /// "Numark NS7 MIDI" to match the original driver's kMIDIPortNames entry.
    kern_return_t registerMIDIPort();

    // ── Bulk transfer callbacks ───────────────────────────────────────────

    /// Completion callback for MIDI IN bulk read (NS7→Mac).
    /// Parses USB MIDI Class 1.0 packets and dispatches to CoreMIDI.
    void midiInComplete(IOMemoryDescriptor * buffer, uint32_t bytesRead);

    /// Called by CoreMIDI when the host app sends MIDI OUT data.
    /// Packs into USB MIDI Class 1.0 format and submits a bulk write.
    kern_return_t sendMIDIData(const uint8_t * data, uint32_t length);

private:
    IOUSBHostDevice    * _device    = nullptr;
    IOUSBHostInterface * _midiIface = nullptr;
    IOUSBHostPipe      * _midiINPipe  = nullptr;
    IOUSBHostPipe      * _midiOUTPipe = nullptr;

    IOBufferMemoryDescriptor * _midiInBuffer  = nullptr;
    IOBufferMemoryDescriptor * _midiOutBuffer = nullptr;

    kern_return_t openMIDIInterface();
    void          scheduleNextMIDIRead();

    // USB MIDI Class 1.0 packet parser
    static uint32_t parseUSBMIDIPacket(const uint8_t * packet,
                                        uint8_t       * midiData);

    // USB MIDI Class 1.0 packet builder
    static uint32_t buildUSBMIDIPacket(const uint8_t * midiData,
                                        uint32_t        midiLength,
                                        uint8_t       * packet);
};


// ─── NumarkNS7UserClient ─────────────────────────────────────────────────────
//
// User client for communication between the driver and user-space processes
// (CoreAudio HAL server, the control panel app).
//
// Exposes a minimal set of external methods for device info queries and
// firmware version reading. The original kext used a proprietary IOUserClient
// subclass; this provides a clean DriverKit equivalent.

class NumarkNS7UserClient : public IOUserClient
{
    OSDeclareDefaultStructors(NumarkNS7UserClient);

public:
    virtual kern_return_t Start(IOService * provider) override;
    virtual kern_return_t Stop(IOService * provider) override;

    // External method selectors (passed as 'selector' in IOConnectCall*)
    enum ExternalMethod : uint64_t {
        kGetDriverVersion    = 0,  // Returns version string
        kGetDeviceSerial     = 1,  // Returns USB serial number string
        kGetFirmwareVersion  = 2,  // Returns device firmware version
        kSetSampleRate       = 3,  // Set audio sample rate (44100 or 48000)
        kGetSampleRate       = 4,  // Get current audio sample rate
        kExternalMethodCount = 5,
    };

    virtual kern_return_t ExternalMethod(uint64_t              selector,
                                         IOUserClientMethodArguments * arguments,
                                         IOUserClientMethodDispatch  * dispatch,
                                         OSObject                    * target,
                                         void                        * reference) override;

private:
    NumarkNS7Driver * _driver = nullptr;

    // Method implementations
    kern_return_t getDriverVersion(IOUserClientMethodArguments * args);
    kern_return_t getDeviceSerial(IOUserClientMethodArguments  * args);
    kern_return_t getFirmwareVersion(IOUserClientMethodArguments * args);
    kern_return_t setSampleRate(IOUserClientMethodArguments  * args);
    kern_return_t getSampleRate(IOUserClientMethodArguments  * args);
};

#endif /* NumarkNS7Driver_h */
