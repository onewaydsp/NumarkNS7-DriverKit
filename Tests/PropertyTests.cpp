// PropertyTests.cpp
// Randomized property tests for NS7Protocol.h. Deterministic: every test
// seeds its own std::mt19937 from test::Seed() (logged by TestMain, fixed
// unless NS7_TEST_SEED is set) and a per-test salt. A property that fails
// reports the iteration so it can be replayed; each test stops at its first
// failing iteration to keep the output readable. Buffers are sized exactly
// (std::vector) so ASan catches any out-of-bounds access.

#include "NS7Protocol.h"
#include "TestFixtures.h"
#include "TestHarness.h"

#include <algorithm>
#include <cstring>
#include <random>
#include <vector>

using namespace NS7;

namespace {

typedef std::vector<uint8_t>  Bytes;
typedef std::vector<uint32_t> Words;

std::mt19937 Rng(uint32_t salt) { return std::mt19937(test::Seed() ^ salt); }

uint32_t Uniform(std::mt19937 & r, uint32_t lo, uint32_t hi)   // inclusive
{
    return std::uniform_int_distribution<uint32_t>(lo, hi)(r);
}

bool Chance(std::mt19937 & r, uint32_t percent) { return Uniform(r, 0, 99) < percent; }

// Records a failed property once, with the iteration that broke it.
#define PROPERTY(cond, iter)                                                   \
    do {                                                                       \
        gChecks++;                                                             \
        if (!(cond)) {                                                         \
            gFailures++;                                                       \
            std::printf("  FAIL %s:%d: %s  (iteration %u, seed 0x%08x)\n",     \
                        __FILE__, __LINE__, #cond, unsigned(iter),             \
                        test::Seed());                                         \
            return;                                                            \
        }                                                                      \
    } while (0)

// Data bytes that follow a MIDI 1.0 status byte (channel or system common).
int DataBytesFor(uint8_t status)
{
    switch (status & 0xF0) {
    case 0xC0: case 0xD0: return 1;
    case 0x80: case 0x90: case 0xA0: case 0xB0: case 0xE0: return 2;
    default: break;
    }
    switch (status) {
    case 0xF1: case 0xF3: return 1;
    case 0xF2:            return 2;
    default:              return 0;
    }
}

bool IsRealTime(uint8_t b) { return b >= 0xF8; }

// ── Output stream checker ────────────────────────────────────────────────────
// Validates a raw MIDI 1.0 byte stream as the NS7 would see it on EP 0x04,
// strictly: no running status (the encoder never relies on it), every data
// byte belongs to the message or SysEx in progress, a SysEx is closed by F7
// before any other non-real-time status, and there are no stray F7s.
struct StreamChecker {
    int  remaining = 0;     // data bytes still owed to the current message
    bool inSysEx   = false;
    bool ok        = true;
    const char * why = "";

    void Fail(const char * reason) { if (ok) { ok = false; why = reason; } }

    void Feed(uint8_t b)
    {
        if (IsRealTime(b)) {
            if (b == 0xF9 || b == 0xFD) Fail("undefined real-time byte");
            return;
        }
        if (b < 0x80) {
            if (inSysEx) return;
            if (remaining == 0) { Fail("data byte with no status or SysEx open"); return; }
            remaining--;
            return;
        }
        if (remaining != 0) { Fail("status byte cut a message short"); return; }
        if (inSysEx) {
            if (b == 0xF7) { inSysEx = false; return; }
            Fail("SysEx not closed by F7 before a status byte");
            return;
        }
        if (b == 0xF7) { Fail("stray F7"); return; }
        if (b == 0xF0) { inSysEx = true; return; }
        if (b == 0xF4 || b == 0xF5) { Fail("undefined system common status"); return; }
        remaining = DataBytesFor(b);
    }

    void Feed(const Bytes & bs) { for (uint8_t b : bs) Feed(b); }
};

// ── Random MIDI generators ───────────────────────────────────────────────────

// A raw MIDI 1.0 byte stream that exercises every RawMidiToUmp path:
// complete and truncated messages, running status, system common, real-time
// bytes anywhere, SysEx of any length (terminated by F7, by a status byte, or
// empty), undefined status bytes and orphan data bytes.
Bytes RandomRawMidi(std::mt19937 & r, uint32_t messages)
{
    Bytes out;
    auto data = [&] { return uint8_t(Uniform(r, 0, 0x7F)); };
    for (uint32_t m = 0; m < messages; m++) {
        switch (Uniform(r, 0, 9)) {
        case 0: case 1: case 2: {                         // channel message
            const uint8_t st = uint8_t(Uniform(r, 0x80, 0xEF));
            out.push_back(st);
            int n = DataBytesFor(st);
            if (Chance(r, 10)) n = int(Uniform(r, 0, uint32_t(n)));   // truncated
            for (int k = 0; k < n; k++) out.push_back(data());
            break;
        }
        case 3:                                           // running status
            for (uint32_t k = 0, n = Uniform(r, 1, 4); k < n; k++) out.push_back(data());
            break;
        case 4: {                                         // system common
            static const uint8_t kSys[] = { 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7 };
            const uint8_t st = kSys[Uniform(r, 0, 6)];
            out.push_back(st);
            for (int k = 0, n = DataBytesFor(st); k < n; k++) out.push_back(data());
            break;
        }
        case 5: case 6:                                   // real-time, possibly mid-message
            out.push_back(uint8_t(Uniform(r, 0xF8, 0xFF)));
            break;
        case 7: case 8: {                                 // SysEx
            out.push_back(0xF0);
            for (uint32_t k = 0, n = Uniform(r, 0, 20); k < n; k++) {
                if (Chance(r, 5)) out.push_back(uint8_t(Uniform(r, 0xF8, 0xFF)));
                out.push_back(data());
            }
            if (Chance(r, 80)) out.push_back(0xF7);       // else ended by the next status
            break;
        }
        default:                                          // orphan data
            out.push_back(data());
            break;
        }
    }
    return out;
}

// One UMP message (types 1-4) of the kinds CoreMIDI delivers to a
// destination, plus SysEx pieces out of order (orphans, restarts, empty
// ends). `sysExOpen` tracks the generator's own SysEx so most sequences are
// well-formed.
Words RandomUmpMessage(std::mt19937 & r, bool & sysExOpen)
{
    static const uint8_t kSysStatus[] = { 0xF1, 0xF2, 0xF3, 0xF6, 0xF8, 0xFA, 0xFB, 0xFC, 0xFE, 0xFF };
    auto d7 = [&] { return Uniform(r, 0, 0x7F); };
    auto sysex = [&](uint32_t kind, uint32_t nb) {
        uint32_t b[6] = {};
        for (uint32_t k = 0; k < nb; k++) b[k] = d7();
        return Words{ 0x30000000u | kind << 20 | nb << 16 | b[0] << 8 | b[1],
                      b[2] << 24 | b[3] << 16 | b[4] << 8 | b[5] };
    };
    const uint32_t pick = Uniform(r, 0, 99);
    if (pick < 20) {                                      // MIDI 1.0 channel voice
        const uint32_t st = Uniform(r, 0x80, 0xEF);
        const uint32_t d1 = DataBytesFor(uint8_t(st)) > 1 ? d7() : 0;
        return Words{ 0x20000000u | st << 16 | d7() << 8 | d1 };
    }
    if (pick < 35) {                                      // system common / real-time
        const uint32_t st = kSysStatus[Uniform(r, 0, sizeof kSysStatus - 1)];
        const int n = DataBytesFor(uint8_t(st));
        return Words{ 0x10000000u | st << 16 | (n > 0 ? d7() << 8 : 0) | (n > 1 ? d7() : 0) };
    }
    if (pick < 55) {                                      // MIDI 2.0 channel voice
        const uint32_t op = Uniform(r, 0, 15), ch = Uniform(r, 0, 15);
        return Words{ 0x40000000u | op << 20 | ch << 16 | (r() & 0xFFFF), uint32_t(r()) };
    }
    // SysEx: mostly well-formed; sometimes orphans and restarts.
    const bool chaos = Chance(r, 15);
    if (!sysExOpen || (chaos && Chance(r, 50))) {
        if (Chance(r, 40)) { sysExOpen = false; return sysex(0, Uniform(r, 0, 6)); }   // complete
        if (chaos && Chance(r, 50)) return sysex(Uniform(r, 2, 3), Uniform(r, 0, 6));    // orphan
        sysExOpen = true;
        return sysex(1, Uniform(r, 0, 6));                                               // start
    }
    if (Chance(r, 60)) return sysex(2, 6);                                               // continue
    sysExOpen = false;
    return sysex(3, Uniform(r, 0, 6));                                                   // end (maybe empty)
}

// Raw rendering of one UMP message, as UmpToRawMidi produces it.
Bytes Render(const Words & msg)
{
    ByteSink sink;
    UmpToRawMidi(msg.data(), msg.size(), ByteSink::cb, &sink);
    return sink.bytes;
}

// ── Reference normalisation for the raw → UMP → raw round trip ───────────────
// What the round trip is expected to produce from any raw input, stated at
// the byte level:
//  * real-time bytes are compared as their own stream: F9/FD are removed and
//    the rest must come out in order, but they are emitted immediately while
//    message and SysEx bytes are buffered, so their position relative to the
//    other bytes is not preserved;
//  * running status is expanded: every channel message carries its status;
//  * a message cut short by a status byte, orphan data bytes, F4/F5 and a
//    stray F7 vanish (F4/F5/F7 also cancel running status);
//  * system common messages keep their bytes and cancel running status;
//  * a SysEx comes out as F0, its data bytes, F7, even when the input ended
//    it with another status byte instead of F7 (UMP SysEx chunking into
//    6-byte packets is invisible after the second conversion).
// The input always ends with F6 so nothing is left buffered.
void Normalize(const Bytes & in, Bytes & msgs, Bytes & realTime)
{
    uint8_t running = 0;
    Bytes pending;           // current channel/system common message
    bool inSysEx = false;
    for (uint8_t b : in) {
        if (IsRealTime(b)) {
            if (b != 0xF9 && b != 0xFD) realTime.push_back(b);
            continue;
        }
        if (inSysEx) {
            if (b < 0x80) { msgs.push_back(b); continue; }
            msgs.push_back(0xF7);
            inSysEx = false;
            if (b == 0xF7) continue;
        }
        if (b >= 0x80) {
            pending.clear();
            running = 0;
            if (b == 0xF0) { msgs.push_back(0xF0); inSysEx = true; continue; }
            if (b == 0xF4 || b == 0xF5 || b == 0xF7) continue;
            if (DataBytesFor(b) == 0) { msgs.push_back(b); continue; }
            running = b;
            pending.push_back(b);
            continue;
        }
        if (running == 0) continue;
        if (pending.empty()) pending.push_back(running);
        pending.push_back(b);
        if (int(pending.size()) == 1 + DataBytesFor(running)) {
            msgs.insert(msgs.end(), pending.begin(), pending.end());
            pending.clear();
            if (running >= 0xF0) running = 0;
        }
    }
}

// Slow, obviously-correct capture decoder: sample bit (23 - k) of channel c
// is bit (c / 2) of byte k in half (c % 2) of the 64-byte frame.
void ReferenceDecodeCapture(const Bytes & src, uint32_t frames, Bytes & dst)
{
    for (uint32_t f = 0; f < frames; f++) {
        for (uint32_t c = 0; c < 4; c++) {
            uint32_t v = 0;
            for (uint32_t k = 0; k < 24; k++) {
                const uint8_t byte = src[f * 64 + (c % 2) * 32 + k];
                if (byte & (1u << (c / 2))) v |= 1u << (23 - k);
            }
            dst.push_back(uint8_t(v));
            dst.push_back(uint8_t(v >> 8));
            dst.push_back(uint8_t(v >> 16));
        }
    }
}

// Independent statement of what ParseLayout accepts: split the configuration
// into descriptors first, then check the endpoints against the table.
bool ReferenceLayout(const Bytes & d, Layout & out)
{
    out = Layout{};
    if (d.size() < 9 || d[0] < 9 || d[1] != 0x02) return false;
    const size_t total = d[2] | size_t(d[3]) << 8;
    if (total > d.size()) return false;

    struct Desc { size_t off; uint8_t len, type; };
    std::vector<Desc> descs;
    for (size_t off = 0; off < total; off += d[off]) {
        if (d[off] < 2 || off + d[off] > total) return false;
        descs.push_back({ off, d[off], d[off + 1] });
    }
    struct Want { uint8_t addr, iface, type; Endpoint Layout::*ep; };
    const Want wants[] = {
        { 0x02, 0, 1, &Layout::playbackOut }, { 0x83, 0, 2, &Layout::midiIn },
        { 0x04, 0, 2, &Layout::midiOut },     { 0x81, 1, 1, &Layout::feedbackIn },
        { 0x86, 1, 2, &Layout::captureIn },
    };
    int iface = -1, alt = -1;
    for (const Desc & x : descs) {
        const uint8_t * p = &d[x.off];
        if (x.type == 0x04 && x.len >= 9) { iface = p[2]; alt = p[3]; continue; }
        if (x.type != 0x05 || x.len < 7) continue;
        for (const Want & w : wants) {
            if (p[2] != w.addr) continue;
            if (iface != w.iface || alt != 1 || (p[3] & 3) != w.type) return false;
            (out.*w.ep).maxPacketSize = uint16_t((p[4] | p[5] << 8) & 0x7FF);
            (out.*w.ep).interval = p[6];
            (out.*w.ep).found = true;
        }
    }
    for (const Want & w : wants)
        if (!(out.*w.ep).found) return false;
    return true;
}

bool SameEndpoint(const Endpoint & a, const Endpoint & b)
{
    return a.found == b.found && a.maxPacketSize == b.maxPacketSize && a.interval == b.interval;
}

// Queues `msgs` in calls of random size (at UMP boundaries, as CoreMIDI
// delivers them), and checks each call against the same messages queued one
// at a time from an identical FIFO and state. Returns the total dropped.
template <uint32_t N>
struct QueueHarness {
    ByteFifo<N> fifo;
    UmpOutState st = {};
    ByteFifo<N> shadow;        // replays each call message by message
    UmpOutState shadowSt = {};
    Bytes drained;
    uint32_t dropped = 0;
};

} // namespace

// ── Raw MIDI → UMP → raw MIDI round trip ─────────────────────────────────────

TEST(prop_raw_ump_raw_round_trip_equals_normalized_input)
{
    auto r = Rng(0x0001);
    for (uint32_t it = 0; it < 3000; it++) {
        Bytes in = RandomRawMidi(r, Uniform(r, 0, 30));
        in.push_back(0xF6);   // flush: ends any SysEx or partial message

        RawMidiToUmp conv;
        UmpSink ump;
        for (size_t i = 0; i < in.size(); ) {                  // random Push boundaries
            const size_t n = std::min<size_t>(in.size() - i, Uniform(r, 1, 50));
            conv.Push(in.data() + i, n, UmpSink::cb, &ump);
            i += n;
        }
        ByteSink out;
        UmpToRawMidi(ump.words.data(), ump.words.size(), ByteSink::cb, &out);

        Bytes wantMsgs, wantRt, gotMsgs, gotRt;
        Normalize(in, wantMsgs, wantRt);
        for (uint8_t b : out.bytes) (IsRealTime(b) ? gotRt : gotMsgs).push_back(b);
        PROPERTY(gotMsgs == wantMsgs, it);
        PROPERTY(gotRt == wantRt, it);

        StreamChecker chk;
        chk.Feed(out.bytes);
        PROPERTY(chk.ok, it);
        PROPERTY(!chk.inSysEx && chk.remaining == 0, it);
    }
}

// ── QueueUmpAsRawMidi ────────────────────────────────────────────────────────

template <uint32_t N>
static void RunQueueProperty(std::mt19937 & r, uint32_t iterations)
{
    for (uint32_t it = 0; it < iterations; it++) {
        QueueHarness<N> h;
        // Random starting fill so the free space varies; the prefill is read
        // back and discarded before checking the stream.
        Bytes prefill(Uniform(r, 0, N), 0xAA);
        h.fifo.Write(prefill.data(), uint32_t(prefill.size()));
        h.shadow.Write(prefill.data(), uint32_t(prefill.size()));
        uint32_t skip = uint32_t(prefill.size());

        bool genSysEx = false;
        const uint32_t calls = Uniform(r, 1, 40);
        for (uint32_t c = 0; c < calls; c++) {
            Words words;
            std::vector<Words> msgs;
            for (uint32_t m = 0, n = Uniform(r, 0, 6); m < n; m++) {
                msgs.push_back(RandomUmpMessage(r, genSysEx));
                words.insert(words.end(), msgs.back().begin(), msgs.back().end());
            }
            const bool partialTail = Chance(r, 5);          // a trailing word short of a UMP
            if (partialTail) words.push_back(0x30160102u);

            const uint32_t d = QueueUmpAsRawMidi(words.data(), words.size(), h.fifo, h.st);
            PROPERTY(d <= msgs.size(), it);
            h.dropped += d;

            // Same messages one at a time: each is either queued (its bytes,
            // possibly after one F7 closing an earlier SysEx) or dropped (at
            // most one F7 gets through); the batch drops the same total.
            uint32_t perMessage = 0;
            for (const Words & m : msgs) {
                const uint32_t before = h.shadow.Size();
                const uint32_t md = QueueUmpAsRawMidi(m.data(), m.size(), h.shadow, h.shadowSt);
                PROPERTY(md <= 1, it);
                perMessage += md;
                const uint32_t grew = h.shadow.Size() - before;
                const Bytes want = Render(m);
                if (md == 1) PROPERTY(grew <= 1, it);
                else         PROPERTY(grew == want.size() || grew == want.size() + 1, it);
            }
            PROPERTY(perMessage == d, it);
            PROPERTY(h.st.sysExOpen == h.shadowSt.sysExOpen && h.st.pendingF7 == h.shadowSt.pendingF7, it);
            PROPERTY(!(h.st.sysExOpen && h.st.pendingF7), it);

            // Consumer: drain a random amount from both, which must match.
            const uint32_t want = Uniform(r, 0, N);
            Bytes a(want), b(want);
            const uint32_t na = h.fifo.Read(a.data(), want), nb = h.shadow.Read(b.data(), want);
            PROPERTY(na == nb, it);
            a.resize(na); b.resize(nb);
            PROPERTY(a == b, it);
            const uint32_t s = std::min(skip, na);
            skip -= s;
            h.drained.insert(h.drained.end(), a.begin() + s, a.end());
        }
        Bytes rest(N);
        rest.resize(h.fifo.Read(rest.data(), N));
        const uint32_t s = std::min(skip, uint32_t(rest.size()));
        h.drained.insert(h.drained.end(), rest.begin() + s, rest.end());

        StreamChecker chk;
        chk.Feed(h.drained);
        if (!chk.ok) std::printf("  stream check: %s\n", chk.why);
        PROPERTY(chk.ok, it);
        PROPERTY(chk.remaining == 0, it);
        // A SysEx still open on the wire is one the state knows it owes an F7 for.
        PROPERTY(chk.inSysEx == (h.st.sysExOpen || h.st.pendingF7), it);
    }
}

TEST(prop_queue_ump_stream_is_well_formed_for_any_fifo_size)
{
    auto r = Rng(0x0002);
    RunQueueProperty<8>(r, 400);
    RunQueueProperty<16>(r, 400);
    RunQueueProperty<32>(r, 400);
    RunQueueProperty<64>(r, 300);
    RunQueueProperty<4096>(r, 100);
}

// Arbitrary 32-bit words (not just valid UMPs) must never produce a byte
// stream the device could misparse, nor read past the end of the input.
TEST(prop_queue_ump_random_words_are_harmless)
{
    auto r = Rng(0x0003);
    for (uint32_t it = 0; it < 2000; it++) {
        ByteFifo<64> f;
        UmpOutState st = {};
        Bytes drained;
        for (uint32_t c = 0, calls = Uniform(r, 1, 10); c < calls; c++) {
            Words w(Uniform(r, 0, 8));
            for (uint32_t & x : w) {
                x = uint32_t(r());
                if (Chance(r, 70)) x = (x & 0x0FFFFFFFu) | Uniform(r, 1, 4) << 28;
            }
            QueueUmpAsRawMidi(w.data(), w.size(), f, st);
            Bytes out(64);
            out.resize(f.Read(out.data(), Uniform(r, 0, 64)));
            drained.insert(drained.end(), out.begin(), out.end());
        }
        Bytes rest(64);
        rest.resize(f.Read(rest.data(), 64));
        drained.insert(drained.end(), rest.begin(), rest.end());
        StreamChecker chk;
        chk.Feed(drained);
        if (!chk.ok) std::printf("  stream check: %s\n", chk.why);
        PROPERTY(chk.ok, it);
    }
}

// ── UmpToRawMidi / Midi2ChannelVoiceToRaw ────────────────────────────────────

TEST(prop_midi2_channel_voice_output_is_bounded_midi1)
{
    auto r = Rng(0x0004);
    for (uint32_t it = 0; it < 4000; it++) {
        const uint32_t op = it % 16;
        const uint32_t w0 = 0x40000000u | op << 20 | (uint32_t(r()) & 0x000FFFFFu);
        const uint32_t w1 = uint32_t(r());
        uint8_t out[kMaxRawBytesPerUmp + 4];
        memset(out, 0xEE, sizeof out);
        const size_t n = Midi2ChannelVoiceToRaw(w0, w1, out);
        PROPERTY(n <= kMaxRawBytesPerUmp, it);
        for (size_t k = kMaxRawBytesPerUmp; k < sizeof out; k++) PROPERTY(out[k] == 0xEE, it);

        const bool translatable = (op >= 0x8 && op <= 0xE) || op == 0x2 || op == 0x3;
        PROPERTY((n > 0) == translatable, it);

        // A sequence of complete MIDI 1.0 channel messages on the UMP's channel.
        size_t i = 0;
        while (i < n) {
            const uint8_t st = out[i++];
            PROPERTY(st >= 0x80 && st <= 0xEF, it);
            PROPERTY((st & 0x0F) == ((w0 >> 16) & 0x0F), it);
            for (int k = 0; k < DataBytesFor(st); k++, i++) {
                PROPERTY(i < n, it);
                PROPERTY(out[i] <= 0x7F, it);
            }
        }
        if (op == 0x9) PROPERTY(out[2] != 0, it);   // never a MIDI 1.0 note off
    }
}

TEST(prop_ump_to_raw_never_reads_past_input)
{
    auto r = Rng(0x0005);
    for (uint32_t it = 0; it < 3000; it++) {
        Words w(Uniform(r, 0, 12));
        for (uint32_t & x : w) x = uint32_t(r());
        ByteSink sink;
        UmpToRawMidi(w.data(), w.size(), ByteSink::cb, &sink);   // ASan: no overread
        PROPERTY(sink.bytes.size() <= w.size() * kMaxRawBytesPerUmp, it);
    }
}

// ── EP 0x83 / EP 0x04 framing ────────────────────────────────────────────────

TEST(prop_extract_midi_in_keeps_non_fill_bytes_of_first_41)
{
    auto r = Rng(0x0006);
    for (uint32_t it = 0; it < 5000; it++) {
        Bytes pkt(Uniform(r, 0, 64));
        for (uint8_t & b : pkt) b = Chance(r, 40) ? kMidiFill : uint8_t(r());
        uint8_t out[kMidiInDataBytes];
        const uint32_t n = ExtractMidiIn(pkt.data(), uint32_t(pkt.size()), out);
        Bytes want;
        for (size_t i = 0; i < pkt.size() && i < kMidiInDataBytes; i++)
            if (pkt[i] != kMidiFill) want.push_back(pkt[i]);
        PROPERTY(n <= kMidiInDataBytes, it);
        PROPERTY(Bytes(out, out + n) == want, it);
    }
}

TEST(prop_build_midi_out_packet_layout)
{
    auto r = Rng(0x0007);
    for (uint32_t it = 0; it < 5000; it++) {
        Bytes data(Uniform(r, 0, 100));
        for (uint8_t & b : data) b = uint8_t(Uniform(r, 0, 0xFC));   // MIDI never sends FD
        uint8_t pkt[kMidiPacketBytes];
        memset(pkt, 0x55, sizeof pkt);
        const uint32_t n = BuildMidiOutPacket(data.data(), uint32_t(data.size()), pkt);
        PROPERTY(n == std::min<uint32_t>(uint32_t(data.size()), kMidiOutMaxBytes), it);
        if (n == 0) {
            for (uint8_t b : pkt) PROPERTY(b == 0x55, it);   // untouched
            continue;
        }
        PROPERTY(memcmp(pkt, data.data(), n) == 0, it);
        for (uint32_t i = n; i < kMidiPacketBytes - 1; i++) PROPERTY(pkt[i] == kMidiFill, it);
        PROPERTY(pkt[kMidiPacketBytes - 1] == kMidiCPort, it);
        // The device-side view: the same bytes come back out of the IN framing.
        uint8_t back[kMidiInDataBytes];
        const uint32_t m = ExtractMidiIn(pkt, kMidiPacketBytes, back);
        PROPERTY(Bytes(back, back + m) == Bytes(data.begin(), data.begin() + n), it);
    }
}

TEST(prop_next_midi_out_packet_drains_fifo_in_order)
{
    auto r = Rng(0x0008);
    for (uint32_t it = 0; it < 500; it++) {
        MidiOutFifo f;
        Bytes in;
        for (uint32_t c = 0, n = Uniform(r, 0, 30); c < n; c++) {
            Bytes chunk(Uniform(r, 1, 200));
            for (uint8_t & b : chunk) b = uint8_t(Uniform(r, 0, 0xFC));
            if (f.Write(chunk.data(), uint32_t(chunk.size())))
                in.insert(in.end(), chunk.begin(), chunk.end());
        }
        Bytes out;
        uint8_t pkt[kMidiPacketBytes];
        uint32_t n, packets = 0;
        while ((n = NextMidiOutPacket(f, pkt)) != 0) {
            PROPERTY(n <= kMidiOutMaxBytes, it);
            PROPERTY(pkt[kMidiPacketBytes - 1] == kMidiCPort, it);
            out.insert(out.end(), pkt, pkt + n);
            packets++;
            // Every packet but the last is full.
            if (out.size() < in.size()) PROPERTY(n == kMidiOutMaxBytes, it);
        }
        PROPERTY(out == in, it);
        PROPERTY(packets == (in.size() + kMidiOutMaxBytes - 1) / kMidiOutMaxBytes, it);
        PROPERTY(f.Size() == 0, it);
    }
}

// ── Descriptor parsing ───────────────────────────────────────────────────────

TEST(prop_parse_layout_on_mutated_descriptors_matches_reference)
{
    auto r = Rng(0x0009);
    const Bytes real(kRealConfig, kRealConfig + sizeof kRealConfig);
    uint32_t accepted = 0;
    for (uint32_t it = 0; it < 20000; it++) {
        Bytes d = real;
        for (uint32_t k = 0, n = Uniform(r, 0, 4); k < n; k++) {
            switch (Uniform(r, 0, 6)) {
            case 0: d[Uniform(r, 0, uint32_t(d.size()) - 1)] = uint8_t(r()); break;   // byte
            case 1: d[Uniform(r, 0, uint32_t(d.size()) - 1)] ^= uint8_t(1u << Uniform(r, 0, 7)); break;
            case 2: d.resize(Uniform(r, 0, uint32_t(d.size()))); break;              // truncate
            case 3: {                                                                // wTotalLength
                const uint32_t t = Chance(r, 50) ? Uniform(r, 0, 0xFFFF)
                                                 : uint32_t(d.size()) + Uniform(r, 0, 4) - 2;
                if (d.size() >= 4) { d[2] = uint8_t(t); d[3] = uint8_t(t >> 8); }
                break;
            }
            case 4:                                                                  // insert junk
                d.insert(d.begin() + Uniform(r, 0, uint32_t(d.size())), Uniform(r, 1, 9), uint8_t(r()));
                break;
            case 5:                                                                  // bLength
                if (!d.empty()) d[Uniform(r, 0, uint32_t(d.size()) - 1) / 7 * 7 % d.size()] = uint8_t(Uniform(r, 0, 12));
                break;
            default:                                                                 // pad + fix total
                d.insert(d.end(), { 0x03, 0x24, 0x00 });
                if (d.size() >= 4) { d[2] = uint8_t(d.size()); d[3] = uint8_t(d.size() >> 8); }
                break;
            }
            if (d.empty()) break;
        }
        // Exactly-sized heap copy so ASan flags any read past `len`.
        std::vector<uint8_t> exact(d.begin(), d.end());
        Layout got, want;
        const bool ok = ParseLayout(exact.empty() ? nullptr : exact.data(), exact.size(), &got);
        const bool ref = ReferenceLayout(d, want);
        PROPERTY(ok == ref, it);
        if (ok) {
            accepted++;
            PROPERTY(SameEndpoint(got.playbackOut, want.playbackOut), it);
            PROPERTY(SameEndpoint(got.midiIn, want.midiIn), it);
            PROPERTY(SameEndpoint(got.midiOut, want.midiOut), it);
            PROPERTY(SameEndpoint(got.feedbackIn, want.feedbackIn), it);
            PROPERTY(SameEndpoint(got.captureIn, want.captureIn), it);
        }
    }
    CHECK(accepted > 0);   // the mutations leave some valid layouts to compare
}

TEST(prop_parse_layout_on_random_bytes_never_overreads)
{
    auto r = Rng(0x000A);
    for (uint32_t it = 0; it < 20000; it++) {
        Bytes d(Uniform(r, 0, 96));
        for (uint8_t & b : d) b = uint8_t(r());
        if (d.size() >= 4 && Chance(r, 70)) { d[1] = 0x02; d[0] = 9; }
        if (d.size() >= 4 && Chance(r, 50)) { d[2] = uint8_t(d.size()); d[3] = 0; }
        Layout got, want;
        const bool ok = ParseLayout(d.empty() ? nullptr : d.data(), d.size(), &got);
        PROPERTY(ok == ReferenceLayout(d, want), it);
    }
}

// ── Capture decoding ─────────────────────────────────────────────────────────

TEST(prop_capture_decode_matches_reference)
{
    auto r = Rng(0x000B);
    for (uint32_t it = 0; it < 2000; it++) {
        const uint32_t frames = Uniform(r, 0, 16);
        Bytes src(frames * kCaptureFrameBytes);
        for (uint8_t & b : src) b = uint8_t(r());
        Bytes got(frames * kPlaybackFrameBytes), want;
        DecodeCaptureFrames(src.data(), frames, got.data());
        ReferenceDecodeCapture(src, frames, want);
        PROPERTY(got == want, it);
    }
}
