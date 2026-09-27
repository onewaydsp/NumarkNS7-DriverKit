// NS7MIDIDriver.cpp
// CoreMIDI side of the NS7 dext. See NS7MIDIDriver.iig.

#include <os/log.h>
#include <stdio.h>

#include <DriverKit/IOLib.h>
#include <DriverKit/OSCollections.h>
#include <MIDIDriverKit/MIDIDriverKit.h>
#include <USBDriverKit/IOUSBHostDevice.h>

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
constexpr const char * kManufacturerUID = "Numark";
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
    bool                    deviceAdded;      // AddObject(device) succeeded
    bool                    detached;         // __atomic: set in Stop; IO block then ignores output
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
    OSString * mfrUID    = OSString::withCString(kManufacturerUID);
    OSString * portName  = OSString::withCString(kPortName);
    OSNumber * sysex     = OSNumber::withNumber(kMaxSysExSpeed, 32);
    kern_return_t ret    = kIOReturnNoMemory;

    if (!product || !vendor || !deviceUID || !modelUID || !mfrUID || !portName || !sysex) goto done;

    iv->device = IOUserMIDIDevice::Create(self, deviceUID, modelUID, mfrUID).detach();
    if (!iv->device) goto done;
    // Create(..., 1, 1) builds the entity's one source and one destination
    // itself; use those rather than adding more.
    iv->entity = IOUserMIDIEntity::Create(self, iv->device, portName, protocol, 1, 1).detach();
    if (!iv->entity) goto done;
    iv->source      = iv->entity->GetSource(0).detach();
    iv->destination = iv->entity->GetDestination(0).detach();
    if (!iv->source || !iv->destination) {
        Log("entity has no source or destination");
        ret = kIOReturnNotFound;
        goto done;
    }

    if ((ret = iv->device->SetName(product)) != kIOReturnSuccess) goto done;
    if ((ret = iv->source->SetName(portName)) != kIOReturnSuccess) goto done;
    if ((ret = iv->destination->SetName(portName)) != kIOReturnSuccess) goto done;
    {
        kern_return_t r;
        if ((r = iv->device->SetProperty(IOUserMIDIProperty::Manufacturer, vendor)) != kIOReturnSuccess)
            Log("SetProperty(Manufacturer) returned 0x%08x (ignored)", r);
        if ((r = iv->device->SetProperty(IOUserMIDIProperty::Model, product)) != kIOReturnSuccess)
            Log("SetProperty(Model) returned 0x%08x (ignored)", r);
        if ((r = iv->entity->SetProperty(IOUserMIDIProperty::MaxSysExSpeed, sysex)) != kIOReturnSuccess)
            Log("SetProperty(MaxSysExSpeed) returned 0x%08x (ignored)", r);
    }

    // CoreMIDI real-time thread: no locks, no allocation, no logging.
    // Stop sets `detached` before removing the device, so a late call from
    // CoreMIDI no longer touches the FIFO.
    ret = iv->destination->SetIOBlock(^kern_return_t(const IOUserMIDIUMPWord * words, size_t numWords) {
        if (__atomic_load_n(&iv->detached, __ATOMIC_ACQUIRE)) return kIOReturnSuccess;
        const uint32_t dropped = NS7::QueueUmpAsRawMidi(words, numWords, iv->midiOut, iv->midiOutState);
        if (dropped) __atomic_fetch_add(&iv->midiOutDropped, dropped, __ATOMIC_RELAXED);
        return kIOReturnSuccess;
    });
    if (ret != kIOReturnSuccess) goto done;

    if ((ret = iv->device->AddEntity(iv->entity)) != kIOReturnSuccess) goto done;
    if ((ret = self->AddObject(iv->device)) != kIOReturnSuccess) goto done;
    iv->deviceAdded = true;

done:
    OSSafeReleaseNULL(product);
    OSSafeReleaseNULL(vendor);
    OSSafeReleaseNULL(deviceUID);
    OSSafeReleaseNULL(modelUID);
    OSSafeReleaseNULL(mfrUID);
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
    // Unregister, then wait out the USB queue: MidiInComplete runs there and
    // calls DeliverMidiIn -> source Send with a client reference it took
    // before we unregistered. After the sync no Send can be in flight, so the
    // device can be removed safely. This runs on this service's queue, not
    // NumarkNS7Device's, so the sync cannot deadlock.
    if (ivars->provider) {
        ivars->provider->SetMidiClient(nullptr);
        ivars->provider->SyncUsbQueue();
    }
    // SetIOBlock(nullptr) is not documented as allowed, so the IO block is
    // detached with a flag instead.
    __atomic_store_n(&ivars->detached, true, __ATOMIC_RELEASE);
    if (ivars->deviceAdded) {
        const kern_return_t ret = RemoveObject(ivars->device);
        if (ret != kIOReturnSuccess) Log("RemoveObject failed: 0x%08x", ret);
        ivars->deviceAdded = false;
    }
    return Stop(provider, SUPERDISPATCH);
}

kern_return_t
NS7MIDIDriver::StartIO(OSArray * deviceList)
{
    // Reset SysEx framing state for a new I/O session (the destination IO
    // block is not expected to run before StartIO).
    ivars->midiOutState = {};
    const kern_return_t ret = super::StartIO(deviceList);
    Log("StartIO: 0x%08x", ret);
    return ret;
}

kern_return_t
NS7MIDIDriver::StopIO()
{
    Log("StopIO");
    return super::StopIO();
}

void
NS7MIDIDriver::DeliverMidiIn(const uint32_t * words, uint32_t count)
{
    if (ivars->source == nullptr) return;
    // Send returns kIOReturnNotReady while the source isn't enabled (no I/O
    // session); that is expected, not an error.
    const kern_return_t ret = ivars->source->Send(words, count);
    if (ret != kIOReturnSuccess && ret != kIOReturnNotReady
        && ivars->sendErrors++ < kLoggedSendErrors)
        Log("source Send failed: 0x%08x", ret);
}

uint32_t
NS7MIDIDriver::NextMidiOutPacket(uint8_t * packet)
{
    const uint32_t dropped = __atomic_exchange_n(&ivars->midiOutDropped, 0, __ATOMIC_RELAXED);
    if (dropped) Log("dropped %u MIDI out messages (FIFO full)", dropped);
    return NS7::NextMidiOutPacket(ivars->midiOut, packet);
}
