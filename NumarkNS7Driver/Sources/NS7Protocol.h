// NS7Protocol.h
// Hardware-independent pieces of the Numark NS7 USB protocol (Ploytec
// "BulkIn/IsocOut" device). Header-only and free of DriverKit types so the
// host unit tests in Tests/ can exercise it. See docs/PROTOCOL.md.

#ifndef NS7Protocol_h
#define NS7Protocol_h

#include <stddef.h>
#include <atomic>
#include <stdint.h>
#include <string.h>

namespace NS7 {

// ── Constants ────────────────────────────────────────────────────────────────

constexpr uint8_t  kEPPlaybackOut = 0x02;   // IF0 alt1, isochronous OUT
constexpr uint8_t  kEPMidiIn      = 0x83;   // IF0 alt1, bulk IN
constexpr uint8_t  kEPMidiOut     = 0x04;   // IF0 alt1, bulk OUT
constexpr uint8_t  kEPFeedbackIn  = 0x81;   // IF1 alt1, isochronous IN
constexpr uint8_t  kEPCaptureIn   = 0x86;   // IF1 alt1, bulk IN

constexpr uint32_t kChannels            = 4;
constexpr uint32_t kBytesPerSample      = 3;
constexpr uint32_t kPlaybackFrameBytes  = kChannels * kBytesPerSample;   // 12
constexpr uint32_t kCaptureFrameBytes   = 64;
constexpr uint32_t kCaptureFramesPerPacket = 8;                          // 512-byte packet

constexpr uint32_t kMidiPacketBytes  = 42;
constexpr uint32_t kMidiInDataBytes  = 41;
constexpr uint32_t kMidiOutMaxBytes  = 39;
constexpr uint8_t  kMidiFill         = 0xFD;
constexpr uint8_t  kMidiCPort        = 0xE0;

// ── Control requests ─────────────────────────────────────────────────────────
// Vendor requests are device-recipient. Register 0x49 reads return 1 byte;
// writes carry the value in wValue with no data stage. Rate requests are
// UAC1-style endpoint requests with a 3-byte little-endian value.

constexpr uint8_t  kReqFirmwareInfo  = 0x56;   // vendor IN, 8 then 5 bytes
constexpr uint8_t  kReqRegister      = 0x49;   // vendor IN (read) / OUT (write), wIndex = reg
constexpr uint8_t  kReqDeepSleep     = 0x44;   // vendor OUT, before system sleep
constexpr uint8_t  kUacSetCur        = 0x01;
constexpr uint8_t  kUacGetCur        = 0x81;
constexpr uint16_t kUacSamplingFreq  = 0x0100;   // SAMPLING_FREQ_CONTROL << 8
constexpr uint32_t kSampleRate       = 44100;    // the only rate the NS7 supports
constexpr uint32_t kSettleMs         = 200;      // plist kMsWait, after SET_INTERFACE

constexpr uint8_t  kReg0InternalClock = 0x02;
constexpr uint8_t  kReg0Busy          = 0x04;   // re-read after 20 ms if set
constexpr uint8_t  kReg0Stream        = 0x10;
constexpr uint8_t  kReg0InputsLE16    = 0x20;

inline uint8_t Reg0WithInternalClock(uint8_t v) { return uint8_t(v | kReg0InternalClock); }

// Written after the rate is set, just before streaming starts.
inline uint8_t Reg0WithStreamEnable(uint8_t v, uint32_t inputChannels)
{
    v = inputChannels <= 16 ? uint8_t(v | kReg0InputsLE16) : uint8_t(v & ~kReg0InputsLE16);
    return uint8_t(v | kReg0Stream);
}

inline void EncodeSampleRate(uint32_t hz, uint8_t out[3])
{
    out[0] = uint8_t(hz); out[1] = uint8_t(hz >> 8); out[2] = uint8_t(hz >> 16);
}

inline uint32_t DecodeSampleRate(const uint8_t in[3])
{
    return uint32_t(in[0]) | uint32_t(in[1]) << 8 | uint32_t(in[2]) << 16;
}

// ── Descriptor layout ────────────────────────────────────────────────────────

struct Endpoint {
    uint16_t maxPacketSize;
    uint8_t  interval;
    bool     found;
};

struct Layout {
    Endpoint playbackOut;   // 0x02
    Endpoint midiIn;        // 0x83
    Endpoint midiOut;       // 0x04
    Endpoint feedbackIn;    // 0x81
    Endpoint captureIn;     // 0x86
};

// Walks a configuration descriptor and checks that every endpoint the driver
// uses is present on the expected interface/alternate with the expected
// transfer type. Returns false for anything else, including malformed input.
inline bool ParseLayout(const uint8_t * d, size_t len, Layout * out)
{
    memset(out, 0, sizeof *out);
    if (len < 9 || d[0] < 9 || d[1] != 0x02) return false;
    const size_t total = size_t(d[2]) | (size_t(d[3]) << 8);
    if (total > len) return false;

    struct Want { uint8_t addr, iface, xferType; Endpoint * ep; };
    const Want wants[] = {
        { kEPPlaybackOut, 0, 1, &out->playbackOut },
        { kEPMidiIn,      0, 2, &out->midiIn },
        { kEPMidiOut,     0, 2, &out->midiOut },
        { kEPFeedbackIn,  1, 1, &out->feedbackIn },
        { kEPCaptureIn,   1, 2, &out->captureIn },
    };

    int iface = -1, alt = -1;
    for (size_t off = 0; off < total; ) {
        const uint8_t bLength = d[off];
        if (bLength < 2 || off + bLength > total) return false;
        const uint8_t * p = d + off;
        if (p[1] == 0x04 && bLength >= 9) {
            iface = p[2]; alt = p[3];
        } else if (p[1] == 0x05 && bLength >= 7) {
            for (const Want & w : wants) {
                if (p[2] != w.addr) continue;
                if (iface != w.iface || alt != 1 || (p[3] & 0x03) != w.xferType) return false;
                w.ep->maxPacketSize = uint16_t((p[4] | (p[5] << 8)) & 0x07FF);
                w.ep->interval      = p[6];
                w.ep->found         = true;
            }
        }
        off += bLength;
    }
    for (const Want & w : wants)
        if (!w.ep->found) return false;
    return true;
}

// ── Playback packetisation (EP 0x02) ─────────────────────────────────────────

// Frames to send in one 125 µs microframe. `feedbackFrames` is the latest
// value from EP 0x81 (frames the device wants this millisecond); `slot` is
// a running microframe counter. Values outside 42..46 fall back to the
// nominal 44.1 kHz pattern, which averages 441 frames per 80 microframes.
inline uint32_t PlaybackFramesForMicroframe(int feedbackFrames, uint32_t slot)
{
    static const uint8_t kSub[5][8] = {
        { 6,5,5,5,6,5,5,5 },   // 42
        { 5,5,6,5,6,5,6,5 },   // 43
        { 6,5,6,5,6,5,6,5 },   // 44
        { 6,6,5,6,5,6,5,6 },   // 45
        { 5,6,6,6,5,6,6,6 },   // 46
    };
    if (feedbackFrames >= 42 && feedbackFrames <= 46)
        return kSub[feedbackFrames - 42][slot & 7];
    return 5 + (slot & 1) + ((slot % 80) == 0 ? 1 : 0);
}

// Byte count of each OUT packet for `count` consecutive microframes, starting
// at `*slot` (which is advanced). Returns the total, i.e. the data length.
inline uint32_t FillPlaybackPacketSizes(int feedbackFrames, uint32_t * slot, uint32_t count,
                                        uint32_t * sizes)
{
    uint32_t total = 0;
    for (uint32_t k = 0; k < count; k++) {
        sizes[k] = PlaybackFramesForMicroframe(feedbackFrames, (*slot)++) * kPlaybackFrameBytes;
        total += sizes[k];
    }
    return total;
}

// ── Isochronous scheduling ───────────────────────────────────────────────────

// Largest plausible distance between the frame the next request continues
// from and the current bus frame. In steady state `planned` runs at most
// lead + queue depth ahead: 10 + 4 x 4 ms playback = 26, 10 + 8 x 4 ms
// feedback = 42 frames. Anything further means the bus frame counter moved
// under us (it went backwards or restarted, e.g. across system sleep), and
// the controller would reject the request with kIOReturnIsoTooNew.
constexpr uint64_t kIsoMaxAheadFrames = 64;

// True if `planned` is more than kIsoMaxAheadFrames after `now`. Written
// with a subtraction so it cannot overflow near the top of the 64-bit range.
inline bool IsoFrameTooFarAhead(uint64_t planned, uint64_t now)
{
    return planned > now && planned - now > kIsoMaxAheadFrames;
}

// Frame an iso request should start on. `planned` continues the previous
// request. Restart `lead` frames after `now` when `planned` is no longer in
// the future (the host fell behind; the controller would reject it as too
// old) or is implausibly far ahead (the bus frame counter went backwards or
// was reset, e.g. after system sleep; the controller would reject it as too
// new, and before this check every resubmit kept failing that way).
inline uint64_t NextIsoFrame(uint64_t planned, uint64_t now, uint64_t lead)
{
    if (planned > now && !IsoFrameTooFarAhead(planned, now)) return planned;
    return now + lead;
}

// IOReturn values used by the streaming policy below (IOReturn.h:
// iokit_common_err(code)), repeated here so the host tests can use them.
constexpr uint32_t kReturnNoDevice      = 0xe00002c0;
constexpr uint32_t kReturnOffline       = 0xe00002d7;
constexpr uint32_t kReturnAborted       = 0xe00002eb;
constexpr uint32_t kReturnNotResponding = 0xe00002ed;
constexpr uint32_t kReturnIsoTooOld     = 0xe00002ee;
constexpr uint32_t kReturnIsoTooNew     = 0xe00002ef;

// An iso submission that failed with one of these is worth one immediate
// retry on a freshly read bus frame: the frame was wrong, not the pipe.
// Other failures (device gone, not responding, aborted) are not retried in
// place, so a detached device can never make the driver spin.
inline bool IsoSubmitRetryable(uint32_t ret)
{
    return ret == kReturnIsoTooNew || ret == kReturnIsoTooOld;
}

// Whether a completion with this status resubmits its slot right away.
// Statuses meaning the pipe is being aborted or the device is going away
// (system sleep, unplug, controller reset) leave the slot idle instead; the
// driver's periodic re-arm restarts it, paced, so an endpoint that keeps
// aborting new requests cannot turn into a busy loop.
inline bool CompletionResubmitsNow(uint32_t status)
{
    return status != kReturnAborted && status != kReturnNotResponding
        && status != kReturnNoDevice && status != kReturnOffline;
}

// ── Rate feedback (EP 0x81) ──────────────────────────────────────────────────

// Returns the frame count for the millisecond, or -1 if the packet is not
// the 3 bytes the device is expected to send.
inline int ParseFeedback(const uint8_t * p, uint32_t len)
{
    return len == 3 ? int(p[0]) : -1;
}

// ── Capture decoding (EP 0x86) ───────────────────────────────────────────────

// Each 64-byte wire frame is two 32-byte halves. In each half, bytes 0..23
// are bit-planes MSB first: bit b of byte k is bit (23-k) of wire channel
// (2b + half). The NS7 uses wire channels 0..3; bytes 24..31 are padding.
// Output is packed 24-bit little-endian, 4 channels (12 bytes per frame).
inline void DecodeCaptureFrames(const uint8_t * src, uint32_t frames, uint8_t * dst)
{
    for (uint32_t f = 0; f < frames; f++, src += kCaptureFrameBytes, dst += kPlaybackFrameBytes) {
        uint32_t ch[4] = { 0, 0, 0, 0 };
        for (int half = 0; half < 2; half++) {
            const uint8_t * h = src + 32 * half;
            for (int k = 0; k < 24; k++) {
                ch[half]     = (ch[half]     << 1) | ( h[k]       & 1u);
                ch[half + 2] = (ch[half + 2] << 1) | ((h[k] >> 1) & 1u);
            }
        }
        for (int c = 0; c < 4; c++) {
            dst[3 * c + 0] = uint8_t(ch[c]);
            dst[3 * c + 1] = uint8_t(ch[c] >> 8);
            dst[3 * c + 2] = uint8_t(ch[c] >> 16);
        }
    }
}

// ── MIDI transport framing (EP 0x83 in / EP 0x04 out) ────────────────────────

// Copies the raw MIDI bytes out of an IN transfer. Only the first 41 bytes
// carry data; 0xFD is filler. `out` must hold 41 bytes. Returns the count.
inline uint32_t ExtractMidiIn(const uint8_t * pkt, uint32_t len, uint8_t * out)
{
    const uint32_t n = len < kMidiInDataBytes ? len : kMidiInDataBytes;
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++)
        if (pkt[i] != kMidiFill) out[k++] = pkt[i];
    return k;
}

// Builds one 42-byte OUT transfer from up to 39 raw MIDI bytes, padded with
// 0xFD, with the C-port byte last. Returns how many bytes of `data` it
// consumed; 0 means there is nothing to send.
inline uint32_t BuildMidiOutPacket(const uint8_t * data, uint32_t avail, uint8_t pkt[kMidiPacketBytes])
{
    if (avail == 0) return 0;
    const uint32_t n = avail < kMidiOutMaxBytes ? avail : kMidiOutMaxBytes;
    memset(pkt, kMidiFill, kMidiPacketBytes);
    memcpy(pkt, data, n);
    pkt[kMidiPacketBytes - 1] = kMidiCPort;
    return n;
}

// ── Raw MIDI 1.0 byte stream → UMP (group 0) ─────────────────────────────────

typedef void (*UmpCallback)(void * ctx, const uint32_t * words, size_t count);

class RawMidiToUmp {
public:
    void Push(const uint8_t * bytes, size_t n, UmpCallback cb, void * ctx)
    {
        for (size_t i = 0; i < n; i++) PushByte(bytes[i], cb, ctx);
    }

private:
    uint8_t  mStatus   = 0;      // running status, or current system common
    uint8_t  mData[2]  = {};
    uint8_t  mHave     = 0;
    uint8_t  mNeed     = 0;
    bool     mInSysEx  = false;
    bool     mSysExStarted = false;
    uint8_t  mSysEx[6] = {};
    uint8_t  mSysExLen = 0;

    static uint8_t DataBytesFor(uint8_t status)
    {
        switch (status & 0xF0) {
            case 0xC0: case 0xD0: return 1;
            case 0xF0: break;
            default:              return 2;
        }
        switch (status) {
            case 0xF1: case 0xF3: return 1;
            case 0xF2:            return 2;
            default:              return 0;
        }
    }

    void EmitSysEx(uint8_t kind, UmpCallback cb, void * ctx)
    {
        uint8_t b[6] = {};
        memcpy(b, mSysEx, mSysExLen);
        const uint32_t w[2] = {
            0x30000000u | (uint32_t(kind) << 20) | (uint32_t(mSysExLen) << 16)
                        | (uint32_t(b[0]) << 8) | b[1],
            (uint32_t(b[2]) << 24) | (uint32_t(b[3]) << 16) | (uint32_t(b[4]) << 8) | b[5],
        };
        cb(ctx, w, 2);
        mSysExLen = 0;
    }

    void EndSysEx(UmpCallback cb, void * ctx)
    {
        EmitSysEx(mSysExStarted ? 3 : 0, cb, ctx);   // end, or complete-in-one
        mInSysEx = false;
        mSysExStarted = false;
    }

    void PushByte(uint8_t b, UmpCallback cb, void * ctx)
    {
        if (b >= 0xF8) {                                 // real-time
            if (b == 0xF9 || b == 0xFD) return;
            const uint32_t w = 0x10000000u | (uint32_t(b) << 16);
            cb(ctx, &w, 1);
            return;
        }
        if (mInSysEx) {
            if (b < 0x80) {
                if (mSysExLen == 6) {
                    EmitSysEx(mSysExStarted ? 2 : 1, cb, ctx);
                    mSysExStarted = true;
                }
                mSysEx[mSysExLen++] = b;
                return;
            }
            EndSysEx(cb, ctx);
            if (b == 0xF7) return;
        }
        if (b >= 0x80) {
            mHave = 0;
            if (b == 0xF0) { mInSysEx = true; mSysExLen = 0; mStatus = 0; return; }
            if (b == 0xF4 || b == 0xF5 || b == 0xF7) { mStatus = 0; return; }
            mStatus = b;
            mNeed   = DataBytesFor(b);
            if (mNeed == 0) { EmitMessage(cb, ctx); mStatus = 0; }
            return;
        }
        if (mStatus == 0) return;                        // no status to apply
        mData[mHave++] = b;
        if (mHave < mNeed) return;
        EmitMessage(cb, ctx);
        mHave = 0;
        if (mStatus >= 0xF0) mStatus = 0;                // system common: no running status
    }

    void EmitMessage(UmpCallback cb, void * ctx)
    {
        const uint32_t type = mStatus >= 0xF0 ? 0x1 : 0x2;
        const uint32_t w = (type << 28) | (uint32_t(mStatus) << 16)
                         | (mNeed > 0 ? uint32_t(mData[0]) << 8 : 0)
                         | (mNeed > 1 ? uint32_t(mData[1]) : 0);
        cb(ctx, &w, 1);
    }
};

// ── UMP → raw MIDI 1.0 byte stream ───────────────────────────────────────────

typedef void (*BytesCallback)(void * ctx, const uint8_t * bytes, size_t count);

// Longest raw MIDI 1.0 rendering of one UMP message: a MIDI 2.0 RPN/NRPN
// becomes four 3-byte control changes.
constexpr size_t kMaxRawBytesPerUmp = 12;

// Translates one MIDI 2.0 channel voice message (UMP type 4) to MIDI 1.0
// bytes using the default translation of the MIDI Association's UMP and
// MIDI 2.0 Protocol Specification. Values are scaled down by taking their
// top bits. Returns the byte count; 0 for messages with no MIDI 1.0 form
// (per-note controllers, per-note pitch bend, per-note management and
// relative RPN/NRPN).
inline size_t Midi2ChannelVoiceToRaw(uint32_t w0, uint32_t w1, uint8_t out[kMaxRawBytesPerUmp])
{
    const uint8_t op = (w0 >> 20) & 0xF;
    const uint8_t ch = (w0 >> 16) & 0xF;
    const uint8_t b2 = (w0 >> 8) & 0x7F, b3 = w0 & 0x7F;
    const uint8_t cc = uint8_t(0xB0 | ch);
    size_t n = 0;
    auto put3 = [&](uint8_t a, uint8_t b, uint8_t c) { out[n++] = a; out[n++] = b; out[n++] = c; };
    switch (op) {
    case 0x8: put3(uint8_t(0x80 | ch), b2, uint8_t(w1 >> 25)); break;              // note off
    case 0x9: {                                                                    // note on
        uint8_t vel = uint8_t(w1 >> 25);
        if (vel == 0) vel = 1;       // velocity 0 would turn a MIDI 2.0 note on into a note off
        put3(uint8_t(0x90 | ch), b2, vel);
        break;
    }
    case 0xA: put3(uint8_t(0xA0 | ch), b2, uint8_t(w1 >> 25)); break;              // poly pressure
    case 0xB: put3(cc, b2, uint8_t(w1 >> 25)); break;                              // control change
    case 0xC:                                                                      // program change
        if (w0 & 0x01) {                                                           // bank valid
            put3(cc, 0x00, (w1 >> 8) & 0x7F);
            put3(cc, 0x20, w1 & 0x7F);
        }
        out[n++] = uint8_t(0xC0 | ch);
        out[n++] = (w1 >> 24) & 0x7F;
        break;
    case 0xD: out[n++] = uint8_t(0xD0 | ch); out[n++] = uint8_t(w1 >> 25); break;  // channel pressure
    case 0xE: {                                                                    // pitch bend
        const uint32_t v14 = w1 >> 18;
        put3(uint8_t(0xE0 | ch), v14 & 0x7F, (v14 >> 7) & 0x7F);
        break;
    }
    case 0x2:                                                                      // registered controller (RPN)
    case 0x3:                                                                      // assignable controller (NRPN)
        put3(cc, op == 0x2 ? 0x65 : 0x63, b2);
        put3(cc, op == 0x2 ? 0x64 : 0x62, b3);
        put3(cc, 0x06, uint8_t(w1 >> 25));
        put3(cc, 0x26, (w1 >> 18) & 0x7F);
        break;
    default: break;
    }
    return n;
}

// True for the status bytes a system (type 1) UMP may carry: defined system
// common and real-time messages, excluding the SysEx framing bytes F0/F7.
inline bool IsSystemUmpStatus(uint8_t status)
{
    switch (status) {
    case 0xF1: case 0xF2: case 0xF3: case 0xF6:
    case 0xF8: case 0xFA: case 0xFB: case 0xFC: case 0xFE: case 0xFF:
        return true;
    default:
        return false;
    }
}

// Converts system (type 1), MIDI 1.0 channel voice (type 2), 7-bit SysEx
// (type 3) and MIDI 2.0 channel voice (type 4, translated to MIDI 1.0 by
// Midi2ChannelVoiceToRaw) UMPs to raw bytes, calling `cb` once per message
// with its whole rendering. Other message types, type-4 messages with no
// MIDI 1.0 form, and type 1/2 messages whose status byte is not valid for
// their type, are skipped by length. Data bytes are masked to 7 bits so a
// malformed UMP can never put a status byte on the wire.
inline void UmpToRawMidi(const uint32_t * w, size_t count, BytesCallback cb, void * ctx)
{
    static const uint8_t kWords[16] = { 1,1,1,2,2,4,1,1,2,2,2,3,3,4,4,4 };
    for (size_t i = 0; i < count; ) {
        const uint32_t w0   = w[i];
        const uint32_t type = w0 >> 28;
        const size_t   len  = kWords[type];
        if (i + len > count) return;
        const uint8_t status = uint8_t(w0 >> 16), d0 = uint8_t(w0 >> 8), d1 = uint8_t(w0);
        uint8_t out[kMaxRawBytesPerUmp];
        size_t  n = 0;
        if (type == 0x1 || type == 0x2) {
            const bool valid = type == 0x2 ? (status >= 0x80 && status <= 0xEF)
                                           : IsSystemUmpStatus(status);
            if (valid) {
                out[n++] = status;
                uint8_t data;
                if (type == 0x2) data = ((status & 0xE0) == 0xC0) ? 1 : 2;
                else             data = (status == 0xF2) ? 2 : (status == 0xF1 || status == 0xF3) ? 1 : 0;
                if (data > 0) out[n++] = d0 & 0x7F;
                if (data > 1) out[n++] = d1 & 0x7F;
            }
        } else if (type == 0x3) {
            const uint8_t kind = (w0 >> 20) & 0xF;
            uint8_t nb = (w0 >> 16) & 0xF;
            if (nb > 6) nb = 6;
            const uint32_t w1 = w[i + 1];
            const uint8_t b[6] = { d0, d1, uint8_t(w1 >> 24), uint8_t(w1 >> 16),
                                   uint8_t(w1 >> 8), uint8_t(w1) };
            if (kind == 0 || kind == 1) out[n++] = 0xF0;
            for (uint8_t k = 0; k < nb; k++) out[n++] = b[k] & 0x7F;
            if (kind == 0 || kind == 3) out[n++] = 0xF7;
        } else if (type == 0x4) {
            n = Midi2ChannelVoiceToRaw(w0, w[i + 1], out);
        }
        if (n) cb(ctx, out, n);
        i += len;
    }
}

// ── MIDI out FIFO ────────────────────────────────────────────────────────────

// Single-producer/single-consumer byte ring. For MIDI out the producer is
// CoreMIDI's real-time thread and the consumer the USB queue. Writes are
// all-or-nothing so a MIDI message is never split by a full buffer. Indices
// run free and wrap at 2^32; N must be a power of two. A zero-filled object
// is a valid empty FIFO (the dext allocates it with IONewZero).
template <uint32_t N>
class ByteFifo {
    static_assert(N != 0 && (N & (N - 1)) == 0, "N must be a power of two");

public:
    // Call only from the producer or consumer thread; a third thread can see a torn head/tail pair.
    uint32_t Size() const
    {
        return mHead.load(std::memory_order_acquire) - mTail.load(std::memory_order_acquire);
    }

    // Total bytes ever written, wrapping at 2^32. For diagnostics; any thread
    // may read it (the value may be slightly stale).
    uint32_t BytesWritten() const
    {
        return mHead.load(std::memory_order_relaxed);
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

    // Consumer side. Drops everything queued so far; returns the count. The
    // producer writes whole messages, so the next Read starts at a message
    // boundary (a SysEx may still continue past it).
    uint32_t Discard()
    {
        const uint32_t tail = mTail.load(std::memory_order_relaxed);
        const uint32_t head = mHead.load(std::memory_order_acquire);
        mTail.store(head, std::memory_order_release);
        return head - tail;
    }

private:
    std::atomic<uint32_t> mHead { 0 };
    std::atomic<uint32_t> mTail { 0 };
    uint8_t               mBuf[N];
};

constexpr uint32_t kMidiOutFifoBytes = 4096;
typedef ByteFifo<kMidiOutFifoBytes> MidiOutFifo;

// Per-destination state for QueueUmpAsRawMidi, kept across calls because
// CoreMIDI may split one SysEx across IO-block invocations. Valid when zero.
// Reset it whenever the destination's FIFO is reset or streaming restarts;
// one state per destination assumes a single UMP group.
struct UmpOutState {
    bool sysExOpen;     // a SysEx start reached the FIFO and its end has not yet reached it.
    bool pendingF7;     // a SysEx was cut short; F7 must be queued before the next message, once there is room
};

// Writes one message's bytes, all or nothing, preceded by F7 if a SysEx is
// still open: any non-real-time message, including a new SysEx start, must
// close it first. Returns false, writing nothing, if they don't fit. The
// caller updates sysExOpen/pendingF7.
template <uint32_t N>
inline bool WriteClosingOpenSysEx(ByteFifo<N> & fifo, const UmpOutState & st,
                                  const uint8_t * bytes, size_t n)
{
    if (!st.sysExOpen) return fifo.Write(bytes, uint32_t(n));
    uint8_t withTerminator[1 + kMaxRawBytesPerUmp];
    if (n > kMaxRawBytesPerUmp) return false;   // cannot happen: UmpToRawMidi's limit
    withTerminator[0] = 0xF7;
    memcpy(withTerminator + 1, bytes, n);
    return fifo.Write(withTerminator, uint32_t(n + 1));
}

// Converts UMP messages to raw MIDI 1.0 bytes and queues each message whole.
// Types 1, 2 and 3 pass through; MIDI 2.0 channel voice (type 4) is
// translated to MIDI 1.0 first; other types are skipped. Returns how many UMP packets were
// dropped because the FIFO had no room (a SysEx counts per packet, not per
// whole message); a trailing partial UMP (too few words for its type) is
// ignored and not counted. Dropping a SysEx start drops the rest of that
// SysEx too, so no orphan data bytes reach the device; a SysEx cut short
// part-way through, or interrupted by another message (including a new
// SysEx start), is terminated with F7 as soon as room allows, so the device
// is never left waiting inside a SysEx. Real-time bytes (0xF8-0xFF) pass through a SysEx untouched. Real-
// time safe: no locks, no allocation.
template <uint32_t N>
inline uint32_t QueueUmpAsRawMidi(const uint32_t * words, size_t count, ByteFifo<N> & fifo,
                                  UmpOutState & state)
{
    struct Ctx { ByteFifo<N> * fifo; UmpOutState * state; uint32_t dropped; };
    Ctx ctx = { &fifo, &state, 0 };
    UmpToRawMidi(words, count, [](void * c, const uint8_t * bytes, size_t n) {
        auto * x = static_cast<Ctx *>(c);
        UmpOutState & st = *x->state;

        if (st.pendingF7) {
            const uint8_t f7 = 0xF7;
            if (x->fifo->Write(&f7, 1)) st.pendingF7 = false;
            else { x->dropped++; return; }
        }

        const bool isStart       = bytes[0] == 0xF0;
        const bool isEmptyEnd    = n == 1 && bytes[0] == 0xF7;   // kind 3, nb 0: CoreMIDI
                                                                   // emits this for a SysEx
                                                                   // whose length is a multiple of 6
        const bool isSysExPiece  = isStart || isEmptyEnd || bytes[0] < 0x80;
        const bool isEnd         = bytes[n - 1] == 0xF7;

        if (!isSysExPiece) {
            // A channel/system-common message (not real-time) interleaved
            // with an open SysEx must close it first; real-time bytes
            // (0xF8-0xFF) pass through without disturbing sysExOpen.
            if (st.sysExOpen && bytes[0] < 0xF8) {
                const bool ok = WriteClosingOpenSysEx(*x->fifo, st, bytes, n);
                st.sysExOpen = false;
                if (!ok) {
                    x->dropped++;
                    st.pendingF7 = true;
                }
            } else {
                if (!x->fifo->Write(bytes, uint32_t(n))) x->dropped++;
            }
            return;
        }

        if (isStart) {
            if (WriteClosingOpenSysEx(*x->fifo, st, bytes, n)) {
                st.sysExOpen = !isEnd;
            } else {
                x->dropped++;
                if (st.sysExOpen) st.pendingF7 = true;   // terminate the still-open older SysEx
                st.sysExOpen = false;
            }
            return;
        }

        // SysEx continue/end piece.
        if (!st.sysExOpen) {
            x->dropped++;   // orphan left over from a dropped start; never queue it
            return;
        }
        if (x->fifo->Write(bytes, uint32_t(n))) {
            if (isEnd) st.sysExOpen = false;
        } else {
            x->dropped++;
            st.sysExOpen = false;
            st.pendingF7 = true;
            const uint8_t f7 = 0xF7;
            if (x->fifo->Write(&f7, 1)) st.pendingF7 = false;
        }
    }, &ctx);
    return ctx.dropped;
}

// Fills one EP 0x04 packet with up to 39 bytes from the FIFO. Returns the
// number of MIDI bytes packed; 0 means the FIFO was empty and `pkt` is untouched.
// Packets deliberately split the byte stream at 39 bytes without regard to
// MIDI message boundaries: the device parses EP 0x04 as a continuous MIDI
// byte stream, as the original Ploytec driver did, so a message may span two
// packets.
template <uint32_t N>
inline uint32_t NextMidiOutPacket(ByteFifo<N> & fifo, uint8_t pkt[kMidiPacketBytes])
{
    uint8_t bytes[kMidiOutMaxBytes];
    const uint32_t n = fifo.Read(bytes, kMidiOutMaxBytes);
    return BuildMidiOutPacket(bytes, n, pkt);
}

// ── MIDI out transfer state (EP 0x04) ────────────────────────────────────────

constexpr uint32_t kMidiOutStallRetries      = 3;     // resends of one packet after a STALL
constexpr uint32_t kMidiOutStallBackoffTicks = 250;   // feedback ticks (~1 s) paused after giving up

// Decides what the driver does with EP 0x04, one packet in flight at a time.
// The driver performs the I/O and logging and reports back; this keeps the
// in-flight length, STALL retries, backoff and counters. Sequence:
//
//   CanStart()? -> fill a packet -> OnPacketReady(n) -> Send
//   Send: submit, then OnSendResult(result of the submit call):
//     Ok -> Idle (in flight)          Error -> Idle (dropped; next tick pumps)
//     Stalled -> ClearStallThenDrop (clear the STALL; packet lost, back off)
//   Completion: OnComplete(result of the transfer):
//     Ok -> Pump (counted sent)       Error (incl. timeout) -> Pump (dropped)
//     Stalled -> ClearStall; clear it, then OnStallCleared(cleared):
//       cleared and retries left -> Send (the same packet again)
//       otherwise -> StartBackoff (packet lost, back off; then pump, a no-op)
//   Every feedback tick: OnTick(), then try to start.
//
// After ClearStallThenDrop or StartBackoff, TakePauseLog() says whether to log
// that MIDI out paused (once per stats period; OnStatsLogged() starts a new
// period). The caller notes every non-Ok result as an error itself. After
// OnStop(), completions are ignored and nothing starts. Value-initialize it
// (`= {}`) or zero-fill it (the dext allocates it with IONewZero). Not thread
// safe: the dext drives it from the USB completion queue only.
class MidiOutStateMachine {
public:
    enum class Result : uint8_t { Ok, Stalled, Error };

    enum class Action : uint8_t {
        Idle,                 // nothing to do now
        Send,                 // submit the packet in the buffer; report with OnSendResult
        ClearStall,           // clear the pipe's STALL; report with OnStallCleared
        ClearStallThenDrop,   // clear the STALL (result unused); packet already dropped as lost
        StartBackoff,         // packet dropped as lost and MIDI out paused; then pump
        Pump,                 // the pipe is free: try to start the next packet
    };

    // True when a new packet may be filled and sent.
    bool CanStart() const { return !mStopping && mLength == 0 && mBackoffTicks == 0; }

    // A packet of `bytes` MIDI bytes is in the buffer (0: the FIFO was empty).
    Action OnPacketReady(uint32_t bytes)
    {
        if (bytes == 0) return Action::Idle;
        mLength  = bytes;
        mRetries = 0;
        return Action::Send;
    }

    // Result of submitting the packet (the AsyncIO call itself).
    Action OnSendResult(Result r)
    {
        if (r == Result::Ok) return Action::Idle;
        if (r == Result::Stalled) {
            GiveUp();
            return Action::ClearStallThenDrop;
        }
        mLength = 0;
        return Action::Idle;
    }

    // Result of the transfer, from its completion.
    Action OnComplete(Result r)
    {
        if (mStopping) return Action::Idle;
        if (r == Result::Stalled) return Action::ClearStall;
        if (r == Result::Ok) {
            mPacketsSent++;
            mBytesSent += mLength;
        }
        mLength = 0;
        return Action::Pump;
    }

    // After Action::ClearStall: whether ClearStall succeeded.
    Action OnStallCleared(bool cleared)
    {
        if (cleared && mRetries < kMidiOutStallRetries) {
            mRetries++;
            return Action::Send;
        }
        GiveUp();
        return Action::StartBackoff;
    }

    void OnTick()        { if (mBackoffTicks > 0) mBackoffTicks--; }
    void OnStatsLogged() { mPauseLogged = false; }
    void OnStop()        { mStopping = true; }

    // True once after a give-up that should be logged this stats period.
    bool TakePauseLog()
    {
        const bool log = mPauseLogPending;
        mPauseLogPending = false;
        return log;
    }

    uint32_t InFlightBytes() const { return mLength; }
    uint32_t Retries()       const { return mRetries; }
    uint32_t BackoffTicks()  const { return mBackoffTicks; }
    uint64_t PacketsSent()   const { return mPacketsSent; }
    uint64_t BytesSent()     const { return mBytesSent; }
    uint64_t PacketsLost()   const { return mPacketsLost; }   // dropped after a STALL it couldn't get past

private:
    void GiveUp()
    {
        mPacketsLost++;
        mLength = 0;
        if (!mPauseLogged) {
            mPauseLogged     = true;
            mPauseLogPending = true;
        }
        mBackoffTicks = kMidiOutStallBackoffTicks;
    }

    uint32_t mLength;           // MIDI bytes in the packet in flight; 0 = idle
    uint32_t mRetries;          // resends of the current packet after a STALL
    uint32_t mBackoffTicks;     // ticks to wait before sending again
    bool     mPauseLogged;      // pause logged this stats period
    bool     mPauseLogPending;  // ... and the driver has yet to log it
    bool     mStopping;
    uint64_t mPacketsSent;
    uint64_t mBytesSent;
    uint64_t mPacketsLost;
};

// ── Ring buffer copies ───────────────────────────────────────────────────────

inline void RingWrite(uint8_t * ring, uint32_t ringFrames, uint32_t frameBytes,
                      uint64_t sampleTime, const uint8_t * src, uint32_t frames)
{
    uint32_t pos = uint32_t(sampleTime % ringFrames);
    while (frames) {
        const uint32_t chunk = (ringFrames - pos) < frames ? (ringFrames - pos) : frames;
        memcpy(ring + size_t(pos) * frameBytes, src, size_t(chunk) * frameBytes);
        src += size_t(chunk) * frameBytes;
        frames -= chunk;
        pos = 0;
    }
}

inline void RingReadAndClear(uint8_t * ring, uint32_t ringFrames, uint32_t frameBytes,
                             uint64_t sampleTime, uint8_t * dst, uint32_t frames)
{
    uint32_t pos = uint32_t(sampleTime % ringFrames);
    while (frames) {
        const uint32_t chunk = (ringFrames - pos) < frames ? (ringFrames - pos) : frames;
        uint8_t * p = ring + size_t(pos) * frameBytes;
        memcpy(dst, p, size_t(chunk) * frameBytes);
        memset(p, 0, size_t(chunk) * frameBytes);
        dst += size_t(chunk) * frameBytes;
        frames -= chunk;
        pos = 0;
    }
}

// ── Zero-timestamp boundary detection ────────────────────────────────────────

// True if the sample count moved from `before` to `after` across a multiple
// of `period` (the ZTS period, normally the ring size). Reports that multiple.
inline bool CrossesZeroTimestamp(uint64_t before, uint64_t after, uint64_t period,
                                 uint64_t * boundary)
{
    const uint64_t next = (before / period + 1) * period;
    if (after < next) return false;
    *boundary = next;
    return true;
}


// ── Streaming request slots (EP 0x02, 0x81, 0x86, 0x83) ──────────────────────
//
// Frame for the one immediate retry after the controller rejected an iso
// request's start frame (IsoTooNew/IsoTooOld): now + lead, or the end of the
// pipe's furthest still-queued request if that is later and plausible, so
// the chain stays monotonic. A queue end implausibly far ahead of the bus
// is stale (the frame counter moved) and is ignored.
inline uint64_t IsoRetryFrame(uint64_t now, uint64_t lead, uint64_t queuedEnd)
{
    const uint64_t base = now + lead;
    return queuedEnd > base && !IsoFrameTooFarAhead(queuedEnd, now) ? queuedEnd : base;
}

// Policy for the request slots the driver keeps queued on its four streaming
// pipes. The driver does the I/O; this says what to (re)submit, when a STALL
// must be cleared first, when a failed slot may be retried, which iso pipe
// the watchdog should abort, and gates everything on system sleep. It exists
// because on hardware, after a Mac sleep/wake, every iso resubmit failed; a
// failed submission never completes, so the slots were lost and the chains
// died for good.
//
// Slot states:  Idle -> (Collect) -> Pending -> (OnSubmitted) -> Armed | Failed
//               Armed -> (OnCompletion) -> Pending (resubmit now) | Failed | Idle (asleep)
//               Failed -> (Collect on a tick at or after its due tick) -> Pending
// Rules:
//   - Every submission goes through Pending, and only Idle or due Failed
//     slots are collected, so a slot is never submitted twice.
//   - A failed submission (or a completion that should not be resubmitted in
//     place: iso "going away" statuses, bulk non-STALL errors) makes the slot
//     Failed. Failed slots are retried only from watchdog ticks, after 1, 2,
//     4, 8, then 16 ticks (100 ms .. 1.6 s at the driver's 100 ms tick). An
//     accepted submission or an OK completion resets the backoff.
//   - A bulk STALL (on completion, or from the submission) clears the STALL
//     before the next submission of that slot.
//   - A bulk STALL on completion is cleared and resubmitted in place once;
//     a slot that stalls again with no OK completion in between is Failed
//     instead and backs off by its streak (1, 2, 4, 8, then 16 ticks), so a
//     pipe that stalls every transfer cannot loop on the completion path.
//     An OK completion or a wake ends the streak.
//   - Per iso pipe, a tick with no completion and no accepted re-arm counts
//     as stuck; after kStuckTicks such ticks in a row, with at least one
//     request armed, EndTick asks for that pipe (only) to be aborted, so
//     requests parked at frames the bus won't reach come back and get retried.
//   - OnPowerOff: nothing is collected or aborted and completions leave
//     slots Idle until OnPowerOn, which also clears every backoff so the
//     next Collect re-arms every slot that came back exactly once.
class StreamKeeper {
public:
    enum Group : uint8_t { kPlayback, kFeedback, kCapture, kMidiIn, kGroupCount };
    static constexpr uint32_t kMaxSlots        = 16;
    static constexpr uint32_t kStuckTicks      = 10;   // ~1 s at 100 ms ticks
    static constexpr uint32_t kMaxBackoffTicks = 16;   // ~1.6 s

    enum class Submit : uint8_t { Ok, Stalled, Error };      // result of a submission
    enum class Next : uint8_t {
        Idle,                    // leave the slot; a tick or wake re-arms it
        Submit,                  // submit it again now; report with OnSubmitted
        ClearStallThenSubmit,    // clear the STALL, then submit; a failed clear is Submit::Stalled
    };
    struct Work { uint8_t group; uint8_t slot; bool clearStall; };

    static constexpr uint32_t kStatusUnderrun = 0xe00002e7;   // kIOReturnUnderrun: data ok
    static constexpr uint32_t kStatusStalled  = 0xe0005000;   // kUSBHostReturnPipeStalled

    void Init(uint32_t playback, uint32_t feedback, uint32_t capture, uint32_t midiIn)
    {
        *this = StreamKeeper();
        const uint32_t n[kGroupCount] = { playback, feedback, capture, midiIn };
        for (uint32_t g = 0; g < kGroupCount; g++) mCount[g] = n[g] < kMaxSlots ? n[g] : kMaxSlots;
    }

    // Hands out the slots to submit now (Idle, and Failed ones that are due)
    // in start order: feedback, playback, capture, MIDI in. Each becomes
    // Pending; report each with OnSubmitted. Nothing while asleep.
    uint32_t Collect(Work * out, uint32_t cap)
    {
        if (mSleeping) return 0;
        static constexpr Group kOrder[kGroupCount] = { kFeedback, kPlayback, kCapture, kMidiIn };
        uint32_t n = 0;
        for (Group g : kOrder) {
            for (uint32_t i = 0; i < mCount[g] && n < cap; i++) {
                Slot & s = mSlots[g][i];
                if (s.state == State::Idle || (s.state == State::Failed && mTick >= s.dueTick)) {
                    s.state = State::Pending;
                    out[n++] = { uint8_t(g), uint8_t(i), s.needClear };
                }
            }
        }
        return n;
    }

    // Result of submitting a Pending slot (after clearing its STALL if asked).
    void OnSubmitted(Group g, uint32_t i, Submit r)
    {
        if (!Valid(g, i) || mSlots[g][i].state != State::Pending) return;
        Slot & s = mSlots[g][i];
        if (r == Submit::Ok) {
            s.state = State::Armed;
            s.fails = 0;
            s.needClear = false;
            mSubmittedOk[g]++;
            return;
        }
        if (r == Submit::Stalled) s.needClear = true;
        Fail(s);
    }

    // A request completed with `status`. Returns what to do with its slot.
    Next OnCompletion(Group g, uint32_t i, uint32_t status)
    {
        if (!Valid(g, i) || mSlots[g][i].state != State::Armed) return Next::Idle;
        Slot & s = mSlots[g][i];
        const bool ok = status == 0 || status == kStatusUnderrun;
        const bool stalled = status == kStatusStalled;
        if (ok) s.fails = s.stalls = 0;
        if (stalled) s.needClear = true;
        if (mSleeping) {
            s.state = State::Idle;
            return Next::Idle;
        }
        mCompletions[g]++;
        if (g == kPlayback || g == kFeedback) {
            if (CompletionResubmitsNow(status)) {
                s.state = State::Pending;
                return Next::Submit;
            }
        } else if (stalled && s.stalls++ > 0) {
            // Stalled again with no OK completion since: retry from ticks,
            // backing off by the streak (the second STALL waits 1 tick).
            if (s.stalls > kMaxStallStreak) s.stalls = kMaxStallStreak;
            s.fails = uint8_t(s.stalls - 2);
        } else if (ok || stalled) {
            s.state = State::Pending;
            return stalled ? Next::ClearStallThenSubmit : Next::Submit;
        }
        Fail(s);
        return Next::Idle;
    }

    // Watchdog tick: BeginTick, Collect + submit, then EndTick, which returns
    // the iso pipes to abort as a mask of (1 << kPlayback) / (1 << kFeedback).
    void BeginTick() { mTick++; }
    uint32_t EndTick()
    {
        uint32_t abort = 0;
        for (uint32_t gi = kPlayback; gi <= kFeedback; gi++) {   // the iso pipes
            const Group g = Group(gi);
            if (mSleeping || mCompletions[g] || mSubmittedOk[g] || Armed(g) == 0) {
                mStuck[g] = 0;
            } else if (++mStuck[g] >= kStuckTicks) {
                abort |= 1u << g;
                mStuck[g] = 0;
            }
        }
        for (uint32_t g = 0; g < kGroupCount; g++) mCompletions[g] = mSubmittedOk[g] = 0;
        return abort;
    }

    void OnPowerOff() { mSleeping = true; }
    void OnPowerOn()
    {
        mSleeping = false;
        for (uint32_t g = 0; g < kGroupCount; g++) {
            mCompletions[g] = mSubmittedOk[g] = mStuck[g] = 0;
            for (uint32_t i = 0; i < mCount[g]; i++) {
                Slot & s = mSlots[g][i];
                s.fails = s.stalls = 0;
                if (s.state == State::Failed) s.state = State::Idle;
            }
        }
    }

    bool Sleeping() const { return mSleeping; }
    bool IsArmed(Group g, uint32_t i) const { return Valid(g, i) && mSlots[g][i].state == State::Armed; }
    uint32_t Armed(Group g) const
    {
        uint32_t n = 0;
        for (uint32_t i = 0; i < mCount[g]; i++) n += mSlots[g][i].state == State::Armed;
        return n;
    }
    uint32_t Failures(Group g, uint32_t i) const { return Valid(g, i) ? mSlots[g][i].fails : 0; }

private:
    enum class State : uint8_t { Idle, Pending, Armed, Failed };
    struct Slot {
        State    state     = State::Idle;
        uint8_t  fails     = 0;       // consecutive failures, for the backoff
        uint8_t  stalls    = 0;       // bulk: STALL completions since the last OK one
        bool     needClear = false;   // clear a STALL before the next submission
        uint64_t dueTick   = 0;       // Failed: first tick it may be retried on
    };

    static constexpr uint8_t kMaxStallStreak = 8;   // backoff is capped long before

    bool Valid(Group g, uint32_t i) const { return g < kGroupCount && i < mCount[g]; }

    void Fail(Slot & s)
    {
        if (s.fails < 8) s.fails++;
        const uint32_t delay = 1u << (s.fails - 1);
        s.dueTick = mTick + (delay < kMaxBackoffTicks ? delay : kMaxBackoffTicks);
        s.state = State::Failed;
    }

    Slot     mSlots[kGroupCount][kMaxSlots];
    uint32_t mCount[kGroupCount] = {};
    uint32_t mCompletions[kGroupCount] = {};   // since the last EndTick
    uint32_t mSubmittedOk[kGroupCount] = {};   // since the last EndTick
    uint32_t mStuck[kGroupCount] = {};         // iso: stuck ticks in a row
    uint64_t mTick = 0;
    bool     mSleeping = false;
};

} // namespace NS7

#endif // NS7Protocol_h
