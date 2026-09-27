// ns7probe.mm
// User-space bring-up tool for the Numark NS7. Talks to the device through
// IOUSBHost.framework while no driver owns its interfaces, to check protocol
// assumptions (docs/PROTOCOL.md) on real hardware before they go into the dext.
//
//   ns7probe midi-in [seconds]          listen on EP 0x83 and print received MIDI
//   ns7probe init [seconds] [--send] [--stream]
//                                       replay the kext's startup control
//                                       requests, then listen on EP 0x83.
//                                       --stream also runs the audio pipes
//                                       (silent iso OUT, feedback, capture)
//
// midi-in only selects IF0 alternate setting 1 and reads. init prints every
// control request it would send and sends nothing without --send. Alternate
// settings are put back to 0 on exit.

#import <Foundation/Foundation.h>
#import <IOUSBHost/IOUSBHost.h>

#include "NS7Protocol.h"

#include <atomic>
#include <cstdio>
#include <string>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

static IOUSBHostInterface * OpenInterface(NSUInteger number, dispatch_queue_t queue = nil)
{
    // The USB-specific keys from +createMatchingDictionary... are not applied
    // by IOServiceGetMatchingService, so match on the registry properties.
    NSDictionary * match = @{
        @kIOProviderClassKey : @"IOUSBHostInterface",
        @kIOPropertyMatchKey : @{ @"idVendor" : @0x15E4, @"idProduct" : @0x0071,
                                  @"bInterfaceNumber" : @(number) },
    };
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault,
                                                       (__bridge_retained CFDictionaryRef)match);
    if (!service) {
        std::fprintf(stderr, "NS7 interface %lu not found\n", (unsigned long)number);
        return nil;
    }
    NSError * err = nil;
    IOUSBHostInterface * iface = [[IOUSBHostInterface alloc]
        initWithIOService:service options:IOUSBHostObjectInitOptionsNone
                    queue:queue error:&err interestHandler:nil];
    IOObjectRelease(service);
    if (!iface) std::fprintf(stderr, "open interface %lu: %s\n", (unsigned long)number,
                             err.localizedDescription.UTF8String);
    return iface;
}

static int Listen(IOUSBHostInterface * iface, double seconds)
{
    NSError * err = nil;
    IOUSBHostPipe * pipe = [iface copyPipeWithAddress:NS7::kEPMidiIn error:&err];
    if (!pipe) {
        std::fprintf(stderr, "pipe 0x83: %s\n", err.localizedDescription.UTF8String);
        return 1;
    }

    std::printf("Listening on EP 0x83 for %.0f s. Move a control on the NS7.\n", seconds);
    NSMutableData * buf = [iface ioDataWithCapacity:NS7::kMidiPacketBytes error:&err];
    unsigned transfers = 0, timeouts = 0, fillOnly = 0, errors = 0;
    unsigned lengths[513] = {};
    int lastByte41 = -1;
    NSDate * end = [NSDate dateWithTimeIntervalSinceNow:seconds];

    while ([end timeIntervalSinceNow] > 0) {
        memset(buf.mutableBytes, 0, buf.length);
        NSUInteger got = 0;
        NSError * ioErr = nil;
        BOOL ok = [pipe sendIORequestWithData:buf bytesTransferred:&got
                            completionTimeout:1.0 error:&ioErr];
        if (!ok) {
            if (ioErr.code == kIOReturnTimeout || ioErr.code == kUSBHostReturnPipeStalled) {
                timeouts++;
                if (ioErr.code == kUSBHostReturnPipeStalled) [pipe clearStallWithError:nil];
                continue;
            }
            std::printf("read error: %s (0x%lx)\n", ioErr.localizedDescription.UTF8String,
                        (long)ioErr.code);
            if (++errors > 5) break;
            continue;
        }
        transfers++;
        lengths[got <= 512 ? got : 512]++;
        const uint8_t * p = static_cast<const uint8_t *>(buf.bytes);
        if (got >= NS7::kMidiPacketBytes && p[41] != lastByte41) {
            std::printf("byte 41 = 0x%02X\n", p[41]);
            lastByte41 = p[41];
        }
        uint8_t midi[NS7::kMidiInDataBytes];
        uint32_t n = NS7::ExtractMidiIn(p, uint32_t(got), midi);
        if (n == 0) { fillOnly++; continue; }
        std::printf("%4lu bytes:", (unsigned long)got);
        for (uint32_t i = 0; i < n; i++) std::printf(" %02X", midi[i]);
        std::printf("\n");
    }

    std::printf("\n%u transfers (%u filler-only), %u timeouts, %u errors\n",
                transfers, fillOnly, timeouts, errors);
    for (unsigned len = 0; len <= 512; len++)
        if (lengths[len]) std::printf("  length %u: %u transfers\n", len, lengths[len]);

    [pipe abortWithError:nil];
    return errors > 5 ? 1 : 0;
}

static int MidiIn(double seconds)
{
    IOUSBHostInterface * iface = OpenInterface(0);
    if (!iface) return 1;

    NSError * err = nil;
    if (![iface selectAlternateSetting:1 error:&err]) {
        std::fprintf(stderr, "select alt 1: %s\n", err.localizedDescription.UTF8String);
        return 1;
    }
    int rc = Listen(iface, seconds);
    [iface selectAlternateSetting:0 error:nil];
    [iface destroy];
    return rc;
}

// ── init: the kext's startup handshake (docs: re/01-init-control.md §1.5–4) ──

struct Control {
    IOUSBHostInterface * iface;
    bool send;
    bool failed;
};

// Prints the 8-byte setup packet (and OUT data), then sends it with --send.
// IN data is zeroed first and left zero in a dry run.
static bool Request(Control & c, uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue,
                    uint16_t wIndex, uint8_t * data, uint16_t wLength, const char * what)
{
    std::printf("  %02X %02X %02X %02X %02X %02X %02X %02X", bmRequestType, bRequest,
                wValue & 0xFF, wValue >> 8, wIndex & 0xFF, wIndex >> 8,
                wLength & 0xFF, wLength >> 8);
    bool in = bmRequestType & 0x80;
    if (!in && wLength) {
        std::printf(" |");
        for (uint16_t i = 0; i < wLength; i++) std::printf(" %02X", data[i]);
    }
    std::printf("   %s", what);
    if (in && data) memset(data, 0, wLength);
    if (!c.send) { std::printf("\n"); return true; }

    IOUSBDeviceRequest req = { bmRequestType, bRequest, wValue, wIndex, wLength };
    NSMutableData * buf = wLength ? [NSMutableData dataWithBytes:data length:wLength] : nil;
    NSUInteger got = 0;
    NSError * err = nil;
    BOOL ok = [c.iface sendDeviceRequest:req data:buf bytesTransferred:&got
                       completionTimeout:5.0 error:&err];
    if (!ok) {
        std::printf("  -> FAILED: %s (0x%lx)\n", err.localizedDescription.UTF8String,
                    (long)err.code);
        c.failed = true;
        return false;
    }
    if (in) {
        memcpy(data, buf.bytes, got);
        std::printf("  -> %lu:", (unsigned long)got);
        for (NSUInteger i = 0; i < got; i++) std::printf(" %02X", data[i]);
    }
    std::printf("\n");
    return true;
}

static bool ReadReg0(Control & c, uint8_t * v)
{
    return Request(c, 0xC0, NS7::kReqRegister, 0, 0, v, 1, "read reg0");
}

static bool WriteReg0(Control & c, uint8_t v, const char * what)
{
    return Request(c, 0x40, NS7::kReqRegister, v, 0, nullptr, 0, what);
}

static bool GetRate(Control & c, uint16_t wIndex, uint32_t * hz, const char * what)
{
    uint8_t b[3];
    bool ok = Request(c, 0xA2, NS7::kUacGetCur, NS7::kUacSamplingFreq, wIndex, b, 3, what);
    *hz = NS7::DecodeSampleRate(b);
    return ok;
}

static bool SetRate(Control & c, uint8_t ep, const char * what)
{
    uint8_t b[3];
    NS7::EncodeSampleRate(NS7::kSampleRate, b);
    return Request(c, 0x22, NS7::kUacSetCur, NS7::kUacSamplingFreq, ep, b, 3, what);
}

static void Sleep(Control & c, unsigned ms)
{
    std::printf("  sleep %u ms\n", ms);
    if (c.send) usleep(ms * 1000);
}

static void Handshake(Control & c)
{
    uint8_t fw[8];
    Request(c, 0xC0, NS7::kReqFirmwareInfo, 0, 0, fw, 8, "firmware version");
    Request(c, 0xC0, NS7::kReqFirmwareInfo, 0, 0, fw, 5, "channel info");

    // updateAjInputSelector: select the internal clock.
    uint8_t v = 0;
    if (ReadReg0(c, &v)) {
        if (v & NS7::kReg0Busy) { Sleep(c, 20); ReadReg0(c, &v); }
        uint8_t nv = NS7::Reg0WithInternalClock(v);
        uint32_t hz = 0;
        if (nv != v || !c.send) {
            WriteReg0(c, nv, "write reg0: internal clock");
            Sleep(c, 40);
            GetRate(c, 0, &hz, "get rate (wIndex 0)");
            ReadReg0(c, &v);
        }
        GetRate(c, 0, &hz, "get rate (wIndex 0)");
        if (hz != NS7::kSampleRate || !c.send)
            SetRate(c, NS7::kEPCaptureIn, "set 44100 on EP 0x86 (if rate differs)");
    }
    if (c.failed) return;

    // startStreaming / resumeStreaming / bulkAudioRun.
    SetRate(c, NS7::kEPCaptureIn, "set 44100 on EP 0x86");
    SetRate(c, NS7::kEPPlaybackOut, "set 44100 on EP 0x02");
    uint32_t hz = 0;
    GetRate(c, NS7::kEPCaptureIn, &hz, "get rate on EP 0x86");
    if (c.send) std::printf("  device rate: %u Hz\n", hz);
    Sleep(c, 50);
    if (ReadReg0(c, &v))
        WriteReg0(c, NS7::Reg0WithStreamEnable(v, 2), "write reg0: stream enable (|0x30)");
}

// ── stream: run the audio pipes the way bulkAudioRun / rtsIsocAudioRun do ────
// (docs: re/02-audio-stream.md §2.4–5, re/03-midi.md §4). All completions run
// on one serial queue, so the state below is only touched from that queue.

constexpr uint32_t kOutMicroframes = 32;     // 4 ms per iso OUT transfer
constexpr uint32_t kOutInFlight    = 4;      // kext: 3

constexpr uint32_t kOutMaxPacket   = 6 * NS7::kPlaybackFrameBytes;
constexpr uint32_t kFbFrames       = 4;      // one feedback packet per ms
constexpr uint32_t kFbInFlight     = 8;      // kext: 2; user space needs more slack
constexpr uint32_t kFbMaxPacket    = 64;
constexpr uint32_t kCapBytes       = 20 * 512;   // 160 frames
constexpr uint32_t kCapInFlight    = 3;
constexpr uint32_t kMidiInFlight   = 16;
constexpr uint64_t kStartLeadFrames = 10;

struct Streams {
    IOUSBHostPipe * out, * fb, * cap, * midi;
    std::atomic<bool> stopping { false };

    IOUSBHostIsochronousTransaction outTx[kOutInFlight][kOutMicroframes];
    IOUSBHostIsochronousTransaction fbTx[kFbInFlight][kFbFrames];
    NSMutableArray<NSMutableData *> * outBuf, * fbBuf, * capBuf, * midiBuf;
    uint64_t outFrame, fbFrame;
    uint32_t outSlot;
    int      feedback = -1;

    unsigned outDone, outErr, fbDone, fbErr, fbPackets, capDone, capErr, midiDone, midiFill, midiErr;
    uint64_t outBytes, capBytes, capNonZero;
    unsigned fbValues[256], fbLengths[kFbMaxPacket + 1], midiLengths[513];
    uint8_t  capPadOr;           // OR of bytes 24..31 and 56..63 of every capture frame
    IOReturn lastErr;
    double   t0;                 // CFAbsoluteTime when the streams were started
    double   lastOk[4];          // ms of the last good completion, per pipe
    unsigned resyncs[4], stalls[4];
    IOUSBHostInterface * if0;
    dispatch_queue_t queue;
    std::string log;             // printed from the main thread, so the queue never blocks on stdout
};

enum { kOut, kFb, kCap, kMidi };
static const char * const kPipeName[] = { "out 0x02", "fb 0x81", "cap 0x86", "midi 0x83" };

static double Ms(Streams & s) { return (CFAbsoluteTimeGetCurrent() - s.t0) * 1000.0; }

__attribute__((format(printf, 2, 3)))
static void Log(Streams & s, const char * fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    s.log += line;
}

// Next frame a new iso request can safely start on.
static uint64_t FreshFrame(Streams & s) { return [s.if0 frameNumberWithTime:nil] + kStartLeadFrames; }

static bool Ok(IOReturn r) { return r == kIOReturnSuccess || r == kIOReturnUnderrun; }

static void Fail(Streams & s, int pipe, unsigned & counter, IOReturn r)
{
    if (counter++ < 5)
        Log(s, "%7.1f ms  %s: error 0x%08X (last good completion at %.1f ms)\n",
            Ms(s), kPipeName[pipe], r, s.lastOk[pipe]);
    s.lastErr = r;
}

// Prints the per-transaction statuses of the first failed iso transfer on a pipe.
static void DumpIso(Streams & s, int pipe, unsigned errors, const IOUSBHostIsochronousTransaction * t,
                    uint32_t n, uint64_t first)
{
    if (errors != 1) return;
    Log(s, "           %s transfer at frame %llu, transactions (status/req/done):",
        kPipeName[pipe], (unsigned long long)first);
    for (uint32_t k = 0; k < n; k++)
        Log(s, "%s %X/%u/%u", k % 8 ? "" : "\n            ", t[k].status, t[k].requestCount,
            t[k].completeCount);
    Log(s, "\n");
}

// Clears a STALL off the completion queue, then calls `resubmit` back on it.
static void RecoverStall(Streams & s, int pipe, IOUSBHostPipe * p, void (^resubmit)(void))
{
    s.stalls[pipe]++;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSError * err = nil;
        BOOL ok = [p clearStallWithError:&err];
        dispatch_async(s.queue, ^{
            if (s.stopping) return;
            Log(s, "%7.1f ms  %s: clearStall %s\n", Ms(s), kPipeName[pipe],
                ok ? "ok" : err.localizedDescription.UTF8String);
            if (ok) resubmit();
        });
    });
}

static void SubmitOut(Streams & s, uint32_t i)
{
    IOUSBHostIsochronousTransaction * t = s.outTx[i];
    uint32_t off = 0;
    for (uint32_t k = 0; k < kOutMicroframes; k++) {
        uint32_t n = NS7::PlaybackFramesForMicroframe(s.feedback, s.outSlot++) * NS7::kPlaybackFrameBytes;
        t[k] = { kIOReturnInvalid, n, off, 0, 0, IOUSBHostIsochronousTransactionOptionsNone };
        off += n;
    }
    if (s.outFrame < FreshFrame(s) - kStartLeadFrames + 1) {   // fell behind: skip ahead
        s.resyncs[kOut]++;
        s.outFrame = FreshFrame(s);
    }
    uint64_t first = s.outFrame;
    s.outFrame += kOutMicroframes / 8;
    NSError * err = nil;
    BOOL ok = [s.out enqueueIORequestWithData:s.outBuf[i] transactionList:t
                         transactionListCount:kOutMicroframes firstFrameNumber:first
                                      options:IOUSBHostIsochronousTransferOptionsNone error:&err
                            completionHandler:^(IOReturn status, IOUSBHostIsochronousTransaction * list) {
        if (s.stopping) return;
        if (!Ok(status)) {
            Fail(s, kOut, s.outErr, status);
            DumpIso(s, kOut, s.outErr, list, kOutMicroframes, first);
        } else s.lastOk[kOut] = Ms(s);
        for (uint32_t k = 0; k < kOutMicroframes; k++) s.outBytes += list[k].completeCount;
        s.outDone++;
        SubmitOut(s, i);
    }];
    if (!ok) Fail(s, kOut, s.outErr, IOReturn(err.code));
}

static void SubmitFeedback(Streams & s, uint32_t i)
{
    IOUSBHostIsochronousTransaction * t = s.fbTx[i];
    for (uint32_t k = 0; k < kFbFrames; k++)
        t[k] = { kIOReturnInvalid, kFbMaxPacket, k * kFbMaxPacket, 0, 0,
                 IOUSBHostIsochronousTransactionOptionsNone };
    if (s.fbFrame < FreshFrame(s) - kStartLeadFrames + 1) {
        s.resyncs[kFb]++;
        s.fbFrame = FreshFrame(s);
    }
    uint64_t first = s.fbFrame;
    s.fbFrame += kFbFrames;
    NSMutableData * buf = s.fbBuf[i];
    NSError * err = nil;
    BOOL ok = [s.fb enqueueIORequestWithData:buf transactionList:t transactionListCount:kFbFrames
                            firstFrameNumber:first options:IOUSBHostIsochronousTransferOptionsNone
                                       error:&err
                           completionHandler:^(IOReturn status, IOUSBHostIsochronousTransaction * list) {
        if (s.stopping) return;
        if (!Ok(status)) {
            Fail(s, kFb, s.fbErr, status);
            DumpIso(s, kFb, s.fbErr, list, kFbFrames, first);
        } else s.lastOk[kFb] = Ms(s);
        const uint8_t * p = static_cast<const uint8_t *>(buf.bytes);
        for (uint32_t k = 0; k < kFbFrames; k++) {
            uint32_t n = list[k].completeCount;
            if (!Ok(list[k].status) || n == 0) continue;
            s.fbPackets++;
            s.fbLengths[n <= kFbMaxPacket ? n : kFbMaxPacket]++;
            if (s.fbPackets == 1) {
                Log(s, "%7.1f ms  first feedback packet (%u bytes):", Ms(s), n);
                for (uint32_t b = 0; b < n; b++) Log(s, " %02X", p[list[k].offset + b]);
                Log(s, "\n");
            }
            int v = NS7::ParseFeedback(p + list[k].offset, n);
            if (v >= 0) { s.feedback = v; s.fbValues[v]++; }
        }
        s.fbDone++;
        SubmitFeedback(s, i);
    }];
    if (!ok) Fail(s, kFb, s.fbErr, IOReturn(err.code));
}

static void SubmitCapture(Streams & s, uint32_t i)
{
    NSMutableData * buf = s.capBuf[i];
    NSError * err = nil;
    BOOL ok = [s.cap enqueueIORequestWithData:buf completionTimeout:0 error:&err
                            completionHandler:^(IOReturn status, NSUInteger got) {
        if (s.stopping) return;
        if (!Ok(status)) {
            Fail(s, kCap, s.capErr, status);
            if (status == kUSBHostReturnPipeStalled) RecoverStall(s, kCap, s.cap, ^{ SubmitCapture(s, i); });
            return;
        }
        s.lastOk[kCap] = Ms(s);
        const uint8_t * p = static_cast<const uint8_t *>(buf.bytes);
        for (NSUInteger b = 0; b < got; b++) {
            if (p[b]) s.capNonZero++;
            if ((b & 31) >= 24) s.capPadOr |= p[b];
        }
        s.capBytes += got;
        s.capDone++;
        SubmitCapture(s, i);
    }];
    if (!ok) Fail(s, kCap, s.capErr, IOReturn(err.code));
}

static void SubmitMidi(Streams & s, uint32_t i)
{
    NSMutableData * buf = s.midiBuf[i];
    NSError * err = nil;
    BOOL ok = [s.midi enqueueIORequestWithData:buf completionTimeout:0 error:&err
                             completionHandler:^(IOReturn status, NSUInteger got) {
        if (s.stopping) return;
        if (!Ok(status)) {
            Fail(s, kMidi, s.midiErr, status);
            if (status == kUSBHostReturnPipeStalled) RecoverStall(s, kMidi, s.midi, ^{ SubmitMidi(s, i); });
            return;
        }
        s.lastOk[kMidi] = Ms(s);
        s.midiDone++;
        s.midiLengths[got <= 512 ? got : 512]++;
        uint8_t midi[NS7::kMidiInDataBytes];
        uint32_t n = NS7::ExtractMidiIn(static_cast<const uint8_t *>(buf.bytes), uint32_t(got), midi);
        if (n == 0) s.midiFill++;
        else {
            Log(s, "%7.1f ms  MIDI:", Ms(s));
            for (uint32_t b = 0; b < n; b++) Log(s, " %02X", midi[b]);
            Log(s, "\n");
        }
        SubmitMidi(s, i);
    }];
    if (!ok) Fail(s, kMidi, s.midiErr, IOReturn(err.code));
}

static IOUSBHostPipe * Pipe(IOUSBHostInterface * iface, uint8_t ep)
{
    NSError * err = nil;
    IOUSBHostPipe * pipe = [iface copyPipeWithAddress:ep error:&err];
    if (!pipe) std::fprintf(stderr, "pipe 0x%02X: %s\n", ep, err.localizedDescription.UTF8String);
    return pipe;
}

static NSMutableArray<NSMutableData *> * Buffers(IOUSBHostInterface * iface, uint32_t count, uint32_t bytes)
{
    NSMutableArray * a = [NSMutableArray array];
    for (uint32_t i = 0; i < count; i++) {
        NSMutableData * d = [iface ioDataWithCapacity:bytes error:nil];
        if (!d || d.length != bytes) return nil;   // ioData length is fixed at the capacity
        memset(d.mutableBytes, 0, bytes);
        [a addObject:d];
    }
    return a;
}

static void PrintStats(Streams & s, const char * prefix)
{
    std::fputs(s.log.c_str(), stdout);
    s.log.clear();
    std::printf("%sout %u xfers %llu B (%u err, %u resync) | fb %u pkts, last %d (%u err, %u resync)"
                " | cap %u xfers %llu B (%u err, %u stall) | midi %u xfers, %u filler (%u err, %u stall)\n",
                prefix, s.outDone, (unsigned long long)s.outBytes, s.outErr, s.resyncs[kOut],
                s.fbPackets, s.feedback, s.fbErr, s.resyncs[kFb], s.capDone,
                (unsigned long long)s.capBytes, s.capErr, s.stalls[kCap], s.midiDone, s.midiFill,
                s.midiErr, s.stalls[kMidi]);
    std::fflush(stdout);
}

static int Stream(IOUSBHostInterface * if0, IOUSBHostInterface * if1, dispatch_queue_t queue,
                  double seconds, Control & c)
{
    Streams * sp = new Streams();
    Streams & s = *sp;
    s.if0 = if0;
    s.queue = queue;
    s.out  = Pipe(if0, NS7::kEPPlaybackOut);
    s.midi = Pipe(if0, NS7::kEPMidiIn);
    s.fb   = Pipe(if1, NS7::kEPFeedbackIn);
    s.cap  = Pipe(if1, NS7::kEPCaptureIn);
    if (!s.out || !s.midi || !s.fb || !s.cap) return 1;
    s.outBuf  = Buffers(if0, kOutInFlight, kOutMicroframes * kOutMaxPacket);
    s.fbBuf   = Buffers(if1, kFbInFlight, kFbFrames * kFbMaxPacket);
    s.capBuf  = Buffers(if1, kCapInFlight, kCapBytes);
    s.midiBuf = Buffers(if0, kMidiInFlight, NS7::kMidiPacketBytes);
    if (!s.outBuf || !s.fbBuf || !s.capBuf || !s.midiBuf) {
        std::fprintf(stderr, "buffer allocation failed\n");
        return 1;
    }

    std::printf("Streaming silence for %.0f s: iso OUT 0x02, feedback 0x81, capture 0x86, "
                "MIDI 0x83. Move a control on the NS7.\n", seconds);
    dispatch_sync(queue, ^{
        s.t0 = CFAbsoluteTimeGetCurrent();
        uint64_t now = [if0 frameNumberWithTime:nil];
        s.outFrame = s.fbFrame = now + kStartLeadFrames;
        for (uint32_t i = 0; i < kFbInFlight; i++)   SubmitFeedback(s, i);
        for (uint32_t i = 0; i < kOutInFlight; i++)  SubmitOut(s, i);
        for (uint32_t i = 0; i < kCapInFlight; i++)  SubmitCapture(s, i);
        for (uint32_t i = 0; i < kMidiInFlight; i++) SubmitMidi(s, i);
    });

    for (int t = 1; t <= int(seconds); t++) {
        sleep(1);   // stats are flushed once per second
        dispatch_sync(queue, ^{
            char prefix[16];
            std::snprintf(prefix, sizeof prefix, "[%2d s] ", t);
            PrintStats(s, prefix);
        });
    }

    s.stopping = true;
    for (IOUSBHostPipe * p in @[ s.out, s.fb, s.cap, s.midi ])
        [p abortWithOption:IOUSBHostAbortOptionSynchronous error:nil];
    dispatch_sync(queue, ^{});

    std::printf("\n");
    PrintStats(s, "total: ");
    if (s.lastErr) std::printf("  last error 0x%08X\n", s.lastErr);
    for (unsigned v = 0; v < 256; v++)
        if (s.fbValues[v]) std::printf("  feedback %u frames/ms: %u packets\n", v, s.fbValues[v]);
    for (unsigned n = 0; n <= kFbMaxPacket; n++)
        if (s.fbLengths[n]) std::printf("  feedback length %u: %u packets\n", n, s.fbLengths[n]);
    for (unsigned n = 0; n <= 512; n++)
        if (s.midiLengths[n]) std::printf("  MIDI length %u: %u transfers\n", n, s.midiLengths[n]);
    std::printf("  capture: %llu non-zero bytes, pad bytes OR = 0x%02X\n",
                (unsigned long long)s.capNonZero, s.capPadOr);
    for (int p = kOut; p <= kMidi; p++)
        std::printf("  %-9s last good completion at %.1f ms\n", kPipeName[p], s.lastOk[p]);

    std::printf("Is the control endpoint still answering?\n");
    uint8_t v = 0;
    ReadReg0(c, &v);
    // Leaked on purpose: aborted completions may still reference it.
    return (s.outErr || s.fbErr || s.capErr || s.midiErr) ? 1 : 0;
}

static int Init(double seconds, bool send, bool stream)
{
    std::printf(send ? "Sending to the NS7:\n"
                     : "Dry run: nothing is sent. Reads show as zero, so every conditional "
                       "request is listed.\n");
    dispatch_queue_t queue = dispatch_queue_create("ns7probe", DISPATCH_QUEUE_SERIAL);
    IOUSBHostInterface * if0 = OpenInterface(0, queue);
    IOUSBHostInterface * if1 = if0 ? OpenInterface(1, queue) : nil;
    if (!if1) return 1;

    std::printf("  SET_INTERFACE IF0 alt 1, IF1 alt 1 (standard, via IOUSBHost)\n");
    NSError * err = nil;
    if (send && (![if0 selectAlternateSetting:1 error:&err] ||
                 ![if1 selectAlternateSetting:1 error:&err])) {
        std::fprintf(stderr, "select alt 1: %s\n", err.localizedDescription.UTF8String);
        [if0 selectAlternateSetting:0 error:nil];
        return 1;
    }

    Control c = { if0, send, false };
    Sleep(c, NS7::kSettleMs);
    Handshake(c);

    int rc = c.failed ? 1 : 0;
    if (!send && stream)
        std::printf("  then stream: silent iso OUT 0x02 (5/6 frames x 12 B per microframe), "
                    "iso IN 0x81, bulk IN 0x86, bulk IN 0x83\n");
    if (send && !c.failed) rc = stream ? Stream(if0, if1, queue, seconds, c) : Listen(if0, seconds);
    if (send) {
        [if1 selectAlternateSetting:0 error:nil];
        [if0 selectAlternateSetting:0 error:nil];
    }
    std::printf("  SET_INTERFACE IF1 alt 0, IF0 alt 0 on exit\n");
    [if1 destroy];
    [if0 destroy];
    return rc;
}

int main(int argc, const char * argv[])
{
    @autoreleasepool {
        if (argc >= 2 && std::strcmp(argv[1], "midi-in") == 0)
            return MidiIn(argc >= 3 ? std::atof(argv[2]) : 10.0);
        if (argc >= 2 && std::strcmp(argv[1], "init") == 0) {
            double seconds = 10.0;
            bool send = false, stream = false;
            for (int i = 2; i < argc; i++) {
                if (std::strcmp(argv[i], "--send") == 0) send = true;
                else if (std::strcmp(argv[i], "--stream") == 0) stream = true;
                else seconds = std::atof(argv[i]);
            }
            return Init(seconds, send, stream);
        }
        std::fprintf(stderr, "usage: %s midi-in [seconds] | init [seconds] [--send] [--stream]\n", argv[0]);
        return 2;
    }
}
