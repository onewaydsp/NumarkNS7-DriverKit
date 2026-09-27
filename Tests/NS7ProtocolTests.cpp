// NS7ProtocolTests.cpp
// Host-side unit tests for the hardware-independent protocol code in
// NumarkNS7Driver/Sources/NS7Protocol.h. Run with:  make -C Tests
//
// Expected values come from the reverse-engineered Ploytec kext (see
// docs/PROTOCOL.md) and from descriptors read off a physical NS7.

#include "NS7Protocol.h"

#include <cstdio>
#include <cstring>
#include <vector>

using namespace NS7;

static int gFailures = 0;
static int gChecks   = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        gChecks++;                                                             \
        if (!(cond)) {                                                         \
            gFailures++;                                                       \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                      \
    } while (0)

#define CHECK_EQ(a, b)                                                         \
    do {                                                                       \
        gChecks++;                                                             \
        auto _a = (a); auto _b = (b);                                          \
        if (!(_a == _b)) {                                                     \
            gFailures++;                                                       \
            std::printf("  FAIL %s:%d: %s == %s  (0x%llx vs 0x%llx)\n",        \
                        __FILE__, __LINE__, #a, #b,                            \
                        (unsigned long long)_a, (unsigned long long)_b);       \
        }                                                                      \
    } while (0)

// ── Descriptor layout ────────────────────────────────────────────────────────

// Configuration descriptor read from a real NS7 (VID 15E4 PID 0071).
static const uint8_t kRealConfig[] = {
    0x09,0x02,0x50,0x00,0x02,0x01,0x00,0x40,0x00,
    0x09,0x04,0x00,0x00,0x00,0xff,0x00,0x00,0x00,
    0x09,0x04,0x00,0x01,0x03,0xff,0x00,0x00,0x00,
    0x07,0x05,0x02,0x05,0x9c,0x00,0x01,
    0x07,0x05,0x83,0x02,0x00,0x02,0x04,
    0x07,0x05,0x04,0x02,0x00,0x02,0x04,
    0x09,0x04,0x01,0x00,0x00,0xff,0x00,0x00,0x00,
    0x09,0x04,0x01,0x01,0x02,0xff,0x00,0x00,0x00,
    0x07,0x05,0x81,0x05,0x40,0x00,0x04,
    0x07,0x05,0x86,0x02,0x00,0x02,0x01,
};

static void test_layout_parses_real_ns7_descriptor()
{
    Layout L;
    CHECK(ParseLayout(kRealConfig, sizeof kRealConfig, &L));
    CHECK_EQ(L.playbackOut.maxPacketSize, 156);
    CHECK_EQ(L.playbackOut.interval, 1);
    CHECK_EQ(L.feedbackIn.maxPacketSize, 64);
    CHECK_EQ(L.feedbackIn.interval, 4);
    CHECK_EQ(L.captureIn.maxPacketSize, 512);
    CHECK_EQ(L.midiIn.maxPacketSize, 512);
    CHECK_EQ(L.midiOut.maxPacketSize, 512);
}

static void test_layout_rejects_missing_endpoint()
{
    std::vector<uint8_t> d(kRealConfig, kRealConfig + sizeof kRealConfig);
    d[36] = 0x85;                        // EP 0x83 → 0x85
    Layout L;
    CHECK(!ParseLayout(d.data(), d.size(), &L));
}

static void test_layout_rejects_endpoint_on_wrong_interface()
{
    std::vector<uint8_t> d(kRealConfig, kRealConfig + sizeof kRealConfig);
    d[20] = 0x01;                        // IF0 alt1 renumbered as IF1 alt1
    Layout L;
    CHECK(!ParseLayout(d.data(), d.size(), &L));
}

static void test_layout_rejects_endpoint_on_alt_setting_zero()
{
    std::vector<uint8_t> d(kRealConfig, kRealConfig + sizeof kRealConfig);
    d[60] = 0x00;                        // IF1 alt1 renumbered as a second alt0
    Layout L;
    CHECK(!ParseLayout(d.data(), d.size(), &L));
}

static void test_layout_rejects_wrong_transfer_type()
{
    std::vector<uint8_t> d(kRealConfig, kRealConfig + sizeof kRealConfig);
    d[30] = 0x02;                        // EP 0x02 iso → bulk
    Layout L;
    CHECK(!ParseLayout(d.data(), d.size(), &L));
}

static void test_layout_rejects_truncated_or_malformed()
{
    Layout L;
    CHECK(!ParseLayout(kRealConfig, 5, &L));
    CHECK(!ParseLayout(kRealConfig, sizeof kRealConfig - 3, &L));
    std::vector<uint8_t> zero(kRealConfig, kRealConfig + sizeof kRealConfig);
    zero[9] = 0;                         // zero-length descriptor must not loop
    CHECK(!ParseLayout(zero.data(), zero.size(), &L));
}

// ── Playback packetisation (EP 0x02) ─────────────────────────────────────────

static void test_playback_pattern_for_44_frames()
{
    const uint32_t want[8] = { 6,5,6,5,6,5,6,5 };
    for (uint32_t k = 0; k < 8; k++) CHECK_EQ(PlaybackFramesForMicroframe(44, k), want[k]);
}

static void test_playback_pattern_for_45_frames()
{
    const uint32_t want[8] = { 6,6,5,6,5,6,5,6 };
    for (uint32_t k = 0; k < 8; k++) CHECK_EQ(PlaybackFramesForMicroframe(45, k), want[k]);
}

static void test_playback_pattern_edges_42_and_46()
{
    const uint32_t w42[8] = { 6,5,5,5,6,5,5,5 };
    const uint32_t w46[8] = { 5,6,6,6,5,6,6,6 };
    for (uint32_t k = 0; k < 8; k++) {
        CHECK_EQ(PlaybackFramesForMicroframe(42, k), w42[k]);
        CHECK_EQ(PlaybackFramesForMicroframe(46, k), w46[k]);
    }
}

static void test_playback_pattern_sums_to_feedback_each_ms()
{
    for (int n = 42; n <= 46; n++) {
        uint32_t sum = 0;
        for (uint32_t k = 0; k < 8; k++) sum += PlaybackFramesForMicroframe(n, 40 + k);
        CHECK_EQ(sum, uint32_t(n));
    }
}

static void test_playback_default_pattern_without_valid_feedback()
{
    // 5 + (slot & 1) + (slot % 80 == 0): exactly 441 frames per 80 µframes
    CHECK_EQ(PlaybackFramesForMicroframe(-1, 0), 6u);
    CHECK_EQ(PlaybackFramesForMicroframe(-1, 1), 6u);
    CHECK_EQ(PlaybackFramesForMicroframe(-1, 2), 5u);
    CHECK_EQ(PlaybackFramesForMicroframe(0,  80), 6u);
    CHECK_EQ(PlaybackFramesForMicroframe(47, 81), 6u);
    uint32_t sum = 0;
    for (uint32_t s = 0; s < 80; s++) sum += PlaybackFramesForMicroframe(-1, s);
    CHECK_EQ(sum, 441u);
}

static void test_playback_packet_never_exceeds_max_packet()
{
    for (int n = -1; n <= 50; n++)
        for (uint32_t s = 0; s < 256; s++)
            CHECK(PlaybackFramesForMicroframe(n, s) * kPlaybackFrameBytes <= 156);
}

// ── Rate feedback (EP 0x81) ──────────────────────────────────────────────────

static void test_feedback_reads_first_byte_of_three()
{
    const uint8_t p[3] = { 45, 0x12, 0x34 };
    CHECK_EQ(ParseFeedback(p, 3), 45);
}

static void test_feedback_rejects_wrong_length()
{
    const uint8_t p[4] = { 44, 0, 0, 0 };
    CHECK_EQ(ParseFeedback(p, 0), -1);
    CHECK_EQ(ParseFeedback(p, 2), -1);
    CHECK_EQ(ParseFeedback(p, 4), -1);
}

// ── Capture decoding (EP 0x86, Ploytec bit-sliced) ───────────────────────────

// Test-side encoder: the inverse of the documented decoder, 16 wire channels.
static void EncodeCaptureFrame(const int32_t ch[4], uint8_t frame[64])
{
    std::memset(frame, 0, 64);
    for (int c = 0; c < 4; c++) {
        const int half = c & 1, bit = c >> 1;
        const uint32_t v = uint32_t(ch[c]) & 0xFFFFFF;
        for (int k = 0; k < 24; k++)
            if (v & (1u << (23 - k))) frame[half * 32 + k] |= uint8_t(1u << bit);
    }
}

static void test_capture_decodes_documented_example()
{
    const uint8_t frame[64] = {
        0x00,0x02,0x02,0x03,0x02,0x02,0x03,0x02,0x02,0x02,0x03,0x03,0x02,0x03,0x02,0x02,
        0x02,0x03,0x02,0x03,0x02,0x03,0x03,0x02,0,0,0,0,0,0,0,0,
        0x02,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    };
    uint8_t out[12] = {};
    DecodeCaptureFrames(frame, 1, out);
    const uint8_t want[12] = { 0x56,0x34,0x12, 0,0,0, 0xFF,0xFF,0x7F, 0x00,0x00,0x80 };
    CHECK(std::memcmp(out, want, 12) == 0);
}

static void test_capture_ignores_unused_wire_channels_and_pad_bytes()
{
    uint8_t frame[64];
    const int32_t ch[4] = { 0x000001, 0x7FFFFF, 0x800000, 0x2468AC };
    EncodeCaptureFrame(ch, frame);
    for (int i = 0; i < 64; i++) frame[i] |= 0xFC;    // wire ch 4..15 all ones
    for (int i = 24; i < 32; i++) frame[i] = 0xEF;    // pad bytes
    for (int i = 56; i < 64; i++) frame[i] = 0xAD;
    uint8_t out[12];
    DecodeCaptureFrames(frame, 1, out);
    const uint8_t want[12] = { 0x01,0x00,0x00, 0xFF,0xFF,0x7F, 0x00,0x00,0x80, 0xAC,0x68,0x24 };
    CHECK(std::memcmp(out, want, 12) == 0);
}

static void test_capture_decodes_consecutive_frames()
{
    uint8_t frames[64 * 8];
    for (int f = 0; f < 8; f++) {
        const int32_t ch[4] = { f, f << 8, f << 16, 0xFFFFFF - f };
        EncodeCaptureFrame(ch, frames + 64 * f);
    }
    uint8_t out[12 * 8];
    DecodeCaptureFrames(frames, 8, out);
    for (int f = 0; f < 8; f++) {
        const uint8_t * o = out + 12 * f;
        CHECK_EQ(o[0], uint8_t(f));   CHECK_EQ(o[1], 0);            CHECK_EQ(o[2], 0);
        CHECK_EQ(o[3], 0);            CHECK_EQ(o[4], uint8_t(f));   CHECK_EQ(o[5], 0);
        CHECK_EQ(o[6], 0);            CHECK_EQ(o[7], 0);            CHECK_EQ(o[8], uint8_t(f));
        CHECK_EQ(o[9], uint8_t(0xFF - f)); CHECK_EQ(o[10], 0xFF);   CHECK_EQ(o[11], 0xFF);
    }
}

// ── MIDI transport framing (EP 0x83 / 0x04) ──────────────────────────────────

static void test_midi_in_drops_fill_bytes()
{
    uint8_t pkt[42]; std::memset(pkt, 0xFD, 42);
    pkt[0] = 0xB0; pkt[1] = 0x07; pkt[2] = 0x64;
    pkt[20] = 0xF8;
    uint8_t out[41];
    CHECK_EQ(ExtractMidiIn(pkt, 42, out), 4u);
    const uint8_t want[4] = { 0xB0, 0x07, 0x64, 0xF8 };
    CHECK(std::memcmp(out, want, 4) == 0);
}

static void test_midi_in_ignores_byte_41()
{
    uint8_t pkt[42]; std::memset(pkt, 0xFD, 42);
    pkt[41] = 0x90;
    uint8_t out[41];
    CHECK_EQ(ExtractMidiIn(pkt, 42, out), 0u);
    pkt[40] = 0x90;
    CHECK_EQ(ExtractMidiIn(pkt, 42, out), 1u);
}

static void test_midi_in_short_transfer_reads_only_actual_bytes()
{
    const uint8_t pkt[5] = { 0x90, 0x40, 0x7F, 0xFD, 0xFD };
    uint8_t out[41];
    CHECK_EQ(ExtractMidiIn(pkt, 3, out), 3u);
    CHECK_EQ(ExtractMidiIn(pkt, 0, out), 0u);
}

static void test_midi_out_packet_matches_documented_example()
{
    const uint8_t msg[3] = { 0x90, 0x40, 0x7F };
    uint8_t pkt[42];
    CHECK_EQ(BuildMidiOutPacket(msg, 3, pkt), 3u);
    CHECK_EQ(pkt[0], 0x90); CHECK_EQ(pkt[1], 0x40); CHECK_EQ(pkt[2], 0x7F);
    for (int i = 3; i <= 40; i++) CHECK_EQ(pkt[i], 0xFD);
    CHECK_EQ(pkt[41], 0xE0);
}

static void test_midi_out_packet_carries_at_most_39_bytes()
{
    uint8_t msg[50];
    for (int i = 0; i < 50; i++) msg[i] = uint8_t(i);
    uint8_t pkt[42];
    CHECK_EQ(BuildMidiOutPacket(msg, 50, pkt), 39u);
    CHECK_EQ(pkt[38], 38);
    CHECK_EQ(pkt[39], 0xFD); CHECK_EQ(pkt[40], 0xFD); CHECK_EQ(pkt[41], 0xE0);
}

static void test_midi_out_packet_empty_input_builds_nothing()
{
    uint8_t pkt[42] = {};
    CHECK_EQ(BuildMidiOutPacket(nullptr, 0, pkt), 0u);
}

// ── Raw MIDI 1.0 byte stream → UMP ───────────────────────────────────────────

struct UmpSink {
    std::vector<uint32_t> words;
    static void cb(void * ctx, const uint32_t * w, size_t n) {
        auto * self = static_cast<UmpSink *>(ctx);
        self->words.insert(self->words.end(), w, w + n);
    }
};

static std::vector<uint32_t> ToUmp(std::initializer_list<uint8_t> bytes)
{
    RawMidiToUmp conv; UmpSink sink;
    std::vector<uint8_t> b(bytes);
    conv.Push(b.data(), b.size(), UmpSink::cb, &sink);
    return sink.words;
}

static void test_raw_note_on_becomes_ump_type2()
{
    CHECK(ToUmp({ 0x90, 0x40, 0x7F }) == std::vector<uint32_t>{ 0x2090407Fu });
}

static void test_raw_running_status_repeats_status()
{
    CHECK((ToUmp({ 0xB0, 0x07, 0x64, 0x07, 0x65 })
           == std::vector<uint32_t>{ 0x20B00764u, 0x20B00765u }));
}

static void test_raw_two_byte_channel_messages()
{
    CHECK((ToUmp({ 0xC3, 0x05, 0x06, 0xD1, 0x40 })
           == std::vector<uint32_t>{ 0x20C30500u, 0x20C30600u, 0x20D14000u }));
}

static void test_raw_message_split_across_pushes()
{
    RawMidiToUmp conv; UmpSink sink;
    const uint8_t a[2] = { 0x90, 0x40 };
    const uint8_t b[1] = { 0x7F };
    conv.Push(a, 2, UmpSink::cb, &sink);
    CHECK(sink.words.empty());
    conv.Push(b, 1, UmpSink::cb, &sink);
    CHECK(sink.words == std::vector<uint32_t>{ 0x2090407Fu });
}

static void test_raw_realtime_inside_message_is_emitted_immediately()
{
    CHECK((ToUmp({ 0x90, 0x40, 0xF8, 0x7F })
           == std::vector<uint32_t>{ 0x10F80000u, 0x2090407Fu }));
}

static void test_raw_system_common_messages()
{
    CHECK((ToUmp({ 0xF2, 0x12, 0x34, 0xF3, 0x05, 0xF1, 0x21, 0xF6 })
           == std::vector<uint32_t>{ 0x10F21234u, 0x10F30500u, 0x10F12100u, 0x10F60000u }));
}

static void test_raw_system_common_cancels_running_status()
{
    CHECK((ToUmp({ 0x90, 0x40, 0x7F, 0xF6, 0x41, 0x7F })
           == std::vector<uint32_t>{ 0x2090407Fu, 0x10F60000u }));
}

static void test_raw_system_common_has_no_running_status()
{
    // Extra data bytes after a complete F1/F3 are not a repeat of it.
    CHECK((ToUmp({ 0xF1, 0x21, 0x22, 0xF3, 0x05, 0x06 })
           == std::vector<uint32_t>{ 0x10F12100u, 0x10F30500u }));
}

static void test_raw_undefined_and_fill_bytes_are_dropped()
{
    CHECK((ToUmp({ 0xF9, 0xFD, 0xF4, 0xF5, 0x40, 0x90, 0x40, 0x7F })
           == std::vector<uint32_t>{ 0x2090407Fu }));
}

static void test_raw_orphan_data_bytes_are_dropped()
{
    CHECK(ToUmp({ 0x40, 0x7F, 0x00 }).empty());
}

static void test_raw_short_sysex_is_single_complete_ump()
{
    CHECK((ToUmp({ 0xF0, 0x01, 0x02, 0x03, 0x04, 0xF7 })
           == std::vector<uint32_t>{ 0x30040102u, 0x03040000u }));
}

static void test_raw_empty_sysex()
{
    CHECK((ToUmp({ 0xF0, 0xF7 }) == std::vector<uint32_t>{ 0x30000000u, 0x00000000u }));
}

static void test_raw_sysex_of_exactly_six_bytes_is_complete()
{
    CHECK((ToUmp({ 0xF0, 1, 2, 3, 4, 5, 6, 0xF7 })
           == std::vector<uint32_t>{ 0x30060102u, 0x03040506u }));
}

static void test_raw_long_sysex_splits_start_continue_end()
{
    // 14 data bytes → start(6) + continue(6) + end(2)
    CHECK((ToUmp({ 0xF0, 1,2,3,4,5,6, 7,8,9,10,11,12, 13,14, 0xF7 })
           == std::vector<uint32_t>{ 0x30160102u, 0x03040506u,
                                     0x30260708u, 0x090A0B0Cu,
                                     0x30320D0Eu, 0x00000000u }));
}

static void test_raw_sysex_with_realtime_interleaved()
{
    CHECK((ToUmp({ 0xF0, 1, 0xF8, 2, 0xF7 })
           == std::vector<uint32_t>{ 0x10F80000u, 0x30020102u, 0x00000000u }));
}

static void test_raw_sysex_terminated_by_status_byte()
{
    // A status byte inside SysEx ends it (as the original driver does), then
    // is processed normally.
    CHECK((ToUmp({ 0xF0, 1, 2, 0x90, 0x40, 0x7F })
           == std::vector<uint32_t>{ 0x30020102u, 0x00000000u, 0x2090407Fu }));
}

static void test_raw_sysex_split_across_pushes()
{
    RawMidiToUmp conv; UmpSink sink;
    const uint8_t a[4] = { 0xF0, 1, 2, 3 };
    const uint8_t b[5] = { 4, 5, 6, 7, 0xF7 };
    conv.Push(a, 4, UmpSink::cb, &sink);
    conv.Push(b, 5, UmpSink::cb, &sink);
    CHECK((sink.words == std::vector<uint32_t>{ 0x30160102u, 0x03040506u,
                                                0x30310700u, 0x00000000u }));
}

// ── UMP → raw MIDI 1.0 byte stream ───────────────────────────────────────────

struct ByteSink {
    std::vector<uint8_t> bytes;
    static void cb(void * ctx, const uint8_t * b, size_t n) {
        auto * self = static_cast<ByteSink *>(ctx);
        self->bytes.insert(self->bytes.end(), b, b + n);
    }
};

static std::vector<uint8_t> FromUmp(std::initializer_list<uint32_t> words)
{
    ByteSink sink;
    std::vector<uint32_t> w(words);
    UmpToRawMidi(w.data(), w.size(), ByteSink::cb, &sink);
    return sink.bytes;
}

static void test_ump_channel_voice_to_raw()
{
    CHECK((FromUmp({ 0x2090407Fu, 0x20C30500u, 0x20E10040u })
           == std::vector<uint8_t>{ 0x90,0x40,0x7F, 0xC3,0x05, 0xE1,0x00,0x40 }));
}

static void test_ump_system_messages_to_raw()
{
    CHECK((FromUmp({ 0x10F80000u, 0x10F21234u, 0x10F30500u, 0x10F60000u })
           == std::vector<uint8_t>{ 0xF8, 0xF2,0x12,0x34, 0xF3,0x05, 0xF6 }));
}

static void test_ump_complete_sysex_to_raw()
{
    CHECK((FromUmp({ 0x30040102u, 0x03040000u })
           == std::vector<uint8_t>{ 0xF0, 1, 2, 3, 4, 0xF7 }));
}

static void test_ump_multi_packet_sysex_to_raw()
{
    CHECK((FromUmp({ 0x30160102u, 0x03040506u, 0x30220708u, 0x00000000u,
                     0x30310900u, 0x00000000u })
           == std::vector<uint8_t>{ 0xF0, 1,2,3,4,5,6, 7,8, 9, 0xF7 }));
}

static void test_ump_other_types_skipped_by_length()
{
    // utility (1 word), MIDI 2.0 voice (2 words), 128-bit data (4 words), note on
    CHECK((FromUmp({ 0x00000000u, 0x40904000u, 0xFFFF0000u,
                     0x50000000u, 1u, 2u, 3u, 0x2090407Fu })
           == std::vector<uint8_t>{ 0x90, 0x40, 0x7F }));
}

static void test_ump_truncated_message_is_not_read_past_end()
{
    CHECK(FromUmp({ 0x30040102u }).empty());
}

// ── Ring buffer copies ───────────────────────────────────────────────────────

static void test_ring_write_wraps()
{
    uint8_t ring[4 * 2] = {};
    const uint8_t src[3 * 2] = { 1,1, 2,2, 3,3 };
    RingWrite(ring, 4, 2, /*sampleTime=*/3, src, 3);
    const uint8_t want[8] = { 2,2, 3,3, 0,0, 1,1 };
    for (int i = 0; i < 8; i++) CHECK_EQ(ring[i], want[i]);
}

static void test_ring_read_wraps_and_clears()
{
    uint8_t ring[4 * 2] = { 1,1, 2,2, 3,3, 4,4 };
    uint8_t dst[3 * 2] = {};
    RingReadAndClear(ring, 4, 2, /*sampleTime=*/7, dst, 3);  // 7 % 4 = 3
    const uint8_t want[6] = { 4,4, 1,1, 2,2 };
    for (int i = 0; i < 6; i++) CHECK_EQ(dst[i], want[i]);
    const uint8_t left[8] = { 0,0, 0,0, 3,3, 0,0 };
    for (int i = 0; i < 8; i++) CHECK_EQ(ring[i], left[i]);
}

// ── Zero-timestamp boundary detection ────────────────────────────────────────

static void test_zts_boundary_detected_when_crossed()
{
    uint64_t boundary = 0;
    CHECK(!CrossesZeroTimestamp(1000, 1020, 1024, &boundary));
    CHECK(CrossesZeroTimestamp(1020, 1030, 1024, &boundary));
    CHECK_EQ(boundary, 1024ull);
    CHECK(CrossesZeroTimestamp(2047, 2048, 1024, &boundary));
    CHECK_EQ(boundary, 2048ull);
    CHECK(!CrossesZeroTimestamp(2048, 2050, 1024, &boundary));
}

// ─────────────────────────────────────────────────────────────────────────────

// ── Control requests (kext: updateAjInputSelector, setAJDMAInputChannels) ────

static void test_reg0_internal_clock_sets_bit1()
{
    CHECK_EQ(Reg0WithInternalClock(0x00), 0x02);
    CHECK_EQ(Reg0WithInternalClock(0x02), 0x02);
    CHECK_EQ(Reg0WithInternalClock(0x31), 0x33);
}

static void test_reg0_stream_enable_up_to_16_inputs_sets_0x30()
{
    CHECK_EQ(Reg0WithStreamEnable(0x02, 2), 0x32);
    CHECK_EQ(Reg0WithStreamEnable(0x02, 16), 0x32);
}

static void test_reg0_stream_enable_over_16_inputs_clears_0x20()
{
    CHECK_EQ(Reg0WithStreamEnable(0x22, 17), 0x12);
}

static void test_sample_rate_is_3_byte_little_endian()
{
    uint8_t b[3];
    EncodeSampleRate(44100, b);
    CHECK_EQ(b[0], 0x44); CHECK_EQ(b[1], 0xAC); CHECK_EQ(b[2], 0x00);
    CHECK_EQ(DecodeSampleRate(b), 44100u);
    const uint8_t r96[3] = { 0x00, 0x77, 0x01 };
    CHECK_EQ(DecodeSampleRate(r96), 96000u);
}

// ── Streaming helpers ────────────────────────────────────────────────────────

static void test_packet_sizes_follow_pattern_and_advance_slot()
{
    uint32_t slot = 0, sizes[8];
    uint32_t total = FillPlaybackPacketSizes(44, &slot, 8, sizes);
    uint32_t expect = 0;
    for (uint32_t k = 0; k < 8; k++) {
        CHECK_EQ(sizes[k], PlaybackFramesForMicroframe(44, k) * kPlaybackFrameBytes);
        expect += sizes[k];
    }
    CHECK_EQ(total, expect);
    CHECK_EQ(total, 44u * kPlaybackFrameBytes);   // 528 bytes for one ms at 44 frames
    CHECK_EQ(slot, 8u);
}

static void test_packet_sizes_continue_from_slot()
{
    uint32_t slotA = 0, a[16];
    FillPlaybackPacketSizes(-1, &slotA, 16, a);
    uint32_t slotB = 0, b[16];
    FillPlaybackPacketSizes(-1, &slotB, 7, b);
    FillPlaybackPacketSizes(-1, &slotB, 9, b + 7);
    CHECK(memcmp(a, b, sizeof a) == 0);
    CHECK_EQ(slotB, 16u);
}

static void test_iso_frame_kept_when_still_in_future()
{
    CHECK_EQ(NextIsoFrame(1005, 1000, 10), 1005ull);
}

static void test_iso_frame_resyncs_when_due_or_past()
{
    CHECK_EQ(NextIsoFrame(1000, 1000, 10), 1010ull);   // due now: too late to schedule
    CHECK_EQ(NextIsoFrame(990,  1000, 10), 1010ull);
}

// Seen on a real NS7 during platter moves: a message split across two
// EP 0x83 transfers must still come out whole.
static void test_real_split_pitch_bend_reassembles()
{
    RawMidiToUmp p;
    std::vector<uint32_t> out;
    auto cb = [](void * ctx, const uint32_t * w, size_t n) {
        auto * v = static_cast<std::vector<uint32_t> *>(ctx);
        v->insert(v->end(), w, w + n);
    };
    const uint8_t a[] = { 0xB0, 0x00, 0x6A, 0xE0, 0x00 }, b[] = { 0x78 };
    p.Push(a, sizeof a, cb, &out);
    p.Push(b, sizeof b, cb, &out);
    CHECK_EQ(out.size(), size_t(2));
    if (out.size() == 2) {
        CHECK_EQ(out[0], 0x20B0006Au);
        CHECK_EQ(out[1], 0x20E00078u);
    }
}

int main()
{
    struct { const char * name; void (*fn)(); } tests[] = {
#define T(x) { #x, x }
        T(test_layout_parses_real_ns7_descriptor),
        T(test_layout_rejects_missing_endpoint),
        T(test_layout_rejects_endpoint_on_wrong_interface),
        T(test_layout_rejects_endpoint_on_alt_setting_zero),
        T(test_layout_rejects_wrong_transfer_type),
        T(test_layout_rejects_truncated_or_malformed),
        T(test_playback_pattern_for_44_frames),
        T(test_playback_pattern_for_45_frames),
        T(test_playback_pattern_edges_42_and_46),
        T(test_playback_pattern_sums_to_feedback_each_ms),
        T(test_playback_default_pattern_without_valid_feedback),
        T(test_playback_packet_never_exceeds_max_packet),
        T(test_feedback_reads_first_byte_of_three),
        T(test_feedback_rejects_wrong_length),
        T(test_capture_decodes_documented_example),
        T(test_capture_ignores_unused_wire_channels_and_pad_bytes),
        T(test_capture_decodes_consecutive_frames),
        T(test_midi_in_drops_fill_bytes),
        T(test_midi_in_ignores_byte_41),
        T(test_midi_in_short_transfer_reads_only_actual_bytes),
        T(test_midi_out_packet_matches_documented_example),
        T(test_midi_out_packet_carries_at_most_39_bytes),
        T(test_midi_out_packet_empty_input_builds_nothing),
        T(test_raw_note_on_becomes_ump_type2),
        T(test_raw_running_status_repeats_status),
        T(test_raw_two_byte_channel_messages),
        T(test_raw_message_split_across_pushes),
        T(test_raw_realtime_inside_message_is_emitted_immediately),
        T(test_raw_system_common_messages),
        T(test_raw_system_common_cancels_running_status),
        T(test_raw_system_common_has_no_running_status),
        T(test_raw_undefined_and_fill_bytes_are_dropped),
        T(test_raw_orphan_data_bytes_are_dropped),
        T(test_raw_short_sysex_is_single_complete_ump),
        T(test_raw_empty_sysex),
        T(test_raw_sysex_of_exactly_six_bytes_is_complete),
        T(test_raw_long_sysex_splits_start_continue_end),
        T(test_raw_sysex_with_realtime_interleaved),
        T(test_raw_sysex_terminated_by_status_byte),
        T(test_raw_sysex_split_across_pushes),
        T(test_ump_channel_voice_to_raw),
        T(test_ump_system_messages_to_raw),
        T(test_ump_complete_sysex_to_raw),
        T(test_ump_multi_packet_sysex_to_raw),
        T(test_ump_other_types_skipped_by_length),
        T(test_ump_truncated_message_is_not_read_past_end),
        T(test_ring_write_wraps),
        T(test_ring_read_wraps_and_clears),
        T(test_zts_boundary_detected_when_crossed),
        T(test_reg0_internal_clock_sets_bit1),
        T(test_reg0_stream_enable_up_to_16_inputs_sets_0x30),
        T(test_reg0_stream_enable_over_16_inputs_clears_0x20),
        T(test_sample_rate_is_3_byte_little_endian),
        T(test_packet_sizes_follow_pattern_and_advance_slot),
        T(test_packet_sizes_continue_from_slot),
        T(test_iso_frame_kept_when_still_in_future),
        T(test_iso_frame_resyncs_when_due_or_past),
        T(test_real_split_pitch_bend_reassembles),
#undef T
    };
    for (auto & t : tests) {
        int before = gFailures;
        t.fn();
        std::printf("%s %s\n", gFailures == before ? "ok  " : "FAIL", t.name);
    }
    std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
