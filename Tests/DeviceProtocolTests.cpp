// DeviceProtocolTests.cpp
// Host-side unit tests for the audio/control parts of NS7Protocol.h:
// descriptor layout, playback packetisation, feedback, capture decoding,
// control requests, iso scheduling, ring buffers and zero timestamps.
//
// Expected values come from the reverse-engineered Ploytec kext (see
// docs/PROTOCOL.md) and from descriptors read off a physical NS7.

#include "NS7Protocol.h"
#include "TestFixtures.h"
#include "TestHarness.h"

#include <cstring>
#include <vector>

using namespace NS7;

// ── Descriptor layout ────────────────────────────────────────────────────────

TEST(test_layout_parses_real_ns7_descriptor)
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

TEST(test_layout_rejects_missing_endpoint)
{
    std::vector<uint8_t> d(kRealConfig, kRealConfig + sizeof kRealConfig);
    d[36] = 0x85;                        // EP 0x83 → 0x85
    Layout L;
    CHECK(!ParseLayout(d.data(), d.size(), &L));
}

TEST(test_layout_rejects_endpoint_on_wrong_interface)
{
    std::vector<uint8_t> d(kRealConfig, kRealConfig + sizeof kRealConfig);
    d[20] = 0x01;                        // IF0 alt1 renumbered as IF1 alt1
    Layout L;
    CHECK(!ParseLayout(d.data(), d.size(), &L));
}

TEST(test_layout_rejects_endpoint_on_alt_setting_zero)
{
    std::vector<uint8_t> d(kRealConfig, kRealConfig + sizeof kRealConfig);
    d[60] = 0x00;                        // IF1 alt1 renumbered as a second alt0
    Layout L;
    CHECK(!ParseLayout(d.data(), d.size(), &L));
}

TEST(test_layout_rejects_wrong_transfer_type)
{
    std::vector<uint8_t> d(kRealConfig, kRealConfig + sizeof kRealConfig);
    d[30] = 0x02;                        // EP 0x02 iso → bulk
    Layout L;
    CHECK(!ParseLayout(d.data(), d.size(), &L));
}

TEST(test_layout_rejects_truncated_or_malformed)
{
    Layout L;
    CHECK(!ParseLayout(kRealConfig, 5, &L));
    CHECK(!ParseLayout(kRealConfig, sizeof kRealConfig - 3, &L));
    std::vector<uint8_t> zero(kRealConfig, kRealConfig + sizeof kRealConfig);
    zero[9] = 0;                         // zero-length descriptor must not loop
    CHECK(!ParseLayout(zero.data(), zero.size(), &L));
}

TEST(test_layout_rejects_bad_configuration_header)
{
    Layout L;
    std::vector<uint8_t> d(kRealConfig, kRealConfig + sizeof kRealConfig);
    d[0] = 8;                            // bLength shorter than a configuration descriptor
    CHECK(!ParseLayout(d.data(), d.size(), &L));
    d[0] = 9;
    d[1] = 0x04;                         // not a configuration descriptor
    CHECK(!ParseLayout(d.data(), d.size(), &L));
}

TEST(test_layout_rejects_descriptor_running_past_total_length)
{
    Layout L;
    std::vector<uint8_t> d(kRealConfig, kRealConfig + sizeof kRealConfig);
    d[2] = uint8_t(sizeof kRealConfig - 3);   // wTotalLength ends inside the last endpoint
    CHECK(!ParseLayout(d.data(), d.size(), &L));
}

TEST(test_layout_ignores_too_short_interface_and_endpoint_descriptors)
{
    // A 4-byte "interface" descriptor claiming IF1 alt1, and a 4-byte
    // "endpoint" descriptor naming EP 0x02, both inserted before IF0's
    // endpoints: honoring either would reject the layout.
    std::vector<uint8_t> d(kRealConfig, kRealConfig + sizeof kRealConfig);
    const uint8_t junk[] = { 0x04, 0x04, 0x01, 0x01, 0x04, 0x05, 0x02, 0x02 };
    d.insert(d.begin() + 27, junk, junk + sizeof junk);
    d[2] = uint8_t(d.size());
    Layout L;
    CHECK(ParseLayout(d.data(), d.size(), &L));
    CHECK_EQ(L.playbackOut.maxPacketSize, 156);
}

// ── Playback packetisation (EP 0x02) ─────────────────────────────────────────

TEST(test_playback_pattern_for_44_frames)
{
    const uint32_t want[8] = { 6,5,6,5,6,5,6,5 };
    for (uint32_t k = 0; k < 8; k++) CHECK_EQ(PlaybackFramesForMicroframe(44, k), want[k]);
}

TEST(test_playback_pattern_for_45_frames)
{
    const uint32_t want[8] = { 6,6,5,6,5,6,5,6 };
    for (uint32_t k = 0; k < 8; k++) CHECK_EQ(PlaybackFramesForMicroframe(45, k), want[k]);
}

TEST(test_playback_pattern_edges_42_and_46)
{
    const uint32_t w42[8] = { 6,5,5,5,6,5,5,5 };
    const uint32_t w46[8] = { 5,6,6,6,5,6,6,6 };
    for (uint32_t k = 0; k < 8; k++) {
        CHECK_EQ(PlaybackFramesForMicroframe(42, k), w42[k]);
        CHECK_EQ(PlaybackFramesForMicroframe(46, k), w46[k]);
    }
}

TEST(test_playback_pattern_sums_to_feedback_each_ms)
{
    for (int n = 42; n <= 46; n++) {
        uint32_t sum = 0;
        for (uint32_t k = 0; k < 8; k++) sum += PlaybackFramesForMicroframe(n, 40 + k);
        CHECK_EQ(sum, uint32_t(n));
    }
}

TEST(test_playback_default_pattern_without_valid_feedback)
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

TEST(test_playback_packet_never_exceeds_max_packet)
{
    for (int n = -1; n <= 50; n++)
        for (uint32_t s = 0; s < 256; s++)
            CHECK(PlaybackFramesForMicroframe(n, s) * kPlaybackFrameBytes <= 156);
}

// ── Rate feedback (EP 0x81) ──────────────────────────────────────────────────

TEST(test_feedback_reads_first_byte_of_three)
{
    const uint8_t p[3] = { 45, 0x12, 0x34 };
    CHECK_EQ(ParseFeedback(p, 3), 45);
}

TEST(test_feedback_rejects_wrong_length)
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

TEST(test_capture_decodes_documented_example)
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

TEST(test_capture_ignores_unused_wire_channels_and_pad_bytes)
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

TEST(test_capture_decodes_consecutive_frames)
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

// ── Ring buffer copies ───────────────────────────────────────────────────────

TEST(test_ring_write_wraps)
{
    uint8_t ring[4 * 2] = {};
    const uint8_t src[3 * 2] = { 1,1, 2,2, 3,3 };
    RingWrite(ring, 4, 2, /*sampleTime=*/3, src, 3);
    const uint8_t want[8] = { 2,2, 3,3, 0,0, 1,1 };
    for (int i = 0; i < 8; i++) CHECK_EQ(ring[i], want[i]);
}

TEST(test_ring_read_wraps_and_clears)
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

TEST(test_zts_boundary_detected_when_crossed)
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

TEST(test_reg0_internal_clock_sets_bit1)
{
    CHECK_EQ(Reg0WithInternalClock(0x00), 0x02);
    CHECK_EQ(Reg0WithInternalClock(0x02), 0x02);
    CHECK_EQ(Reg0WithInternalClock(0x31), 0x33);
}

TEST(test_reg0_stream_enable_up_to_16_inputs_sets_0x30)
{
    CHECK_EQ(Reg0WithStreamEnable(0x02, 2), 0x32);
    CHECK_EQ(Reg0WithStreamEnable(0x02, 16), 0x32);
}

TEST(test_reg0_stream_enable_over_16_inputs_clears_0x20)
{
    CHECK_EQ(Reg0WithStreamEnable(0x22, 17), 0x12);
}

TEST(test_sample_rate_is_3_byte_little_endian)
{
    uint8_t b[3];
    EncodeSampleRate(44100, b);
    CHECK_EQ(b[0], 0x44); CHECK_EQ(b[1], 0xAC); CHECK_EQ(b[2], 0x00);
    CHECK_EQ(DecodeSampleRate(b), 44100u);
    const uint8_t r96[3] = { 0x00, 0x77, 0x01 };
    CHECK_EQ(DecodeSampleRate(r96), 96000u);
}

// ── Streaming helpers ────────────────────────────────────────────────────────

TEST(test_packet_sizes_follow_pattern_and_advance_slot)
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

TEST(test_packet_sizes_continue_from_slot)
{
    uint32_t slotA = 0, a[16];
    FillPlaybackPacketSizes(-1, &slotA, 16, a);
    uint32_t slotB = 0, b[16];
    FillPlaybackPacketSizes(-1, &slotB, 7, b);
    FillPlaybackPacketSizes(-1, &slotB, 9, b + 7);
    CHECK(memcmp(a, b, sizeof a) == 0);
    CHECK_EQ(slotB, 16u);
}

TEST(test_iso_frame_kept_when_still_in_future)
{
    CHECK_EQ(NextIsoFrame(1005, 1000, 10), 1005ull);
}

TEST(test_iso_frame_resyncs_when_due_or_past)
{
    CHECK_EQ(NextIsoFrame(1000, 1000, 10), 1010ull);   // due now: too late to schedule
    CHECK_EQ(NextIsoFrame(990,  1000, 10), 1010ull);
}
