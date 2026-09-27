// MidiTests.cpp
// Host-side unit tests for the MIDI parts of NS7Protocol.h: EP 0x83/0x04
// framing, raw MIDI <-> UMP conversion, MIDI 2.0 translation, the MIDI out
// FIFO and QueueUmpAsRawMidi / NextMidiOutPacket.

#include "NS7Protocol.h"
#include "TestFixtures.h"
#include "TestHarness.h"

#include <cstring>
#include <vector>

using namespace NS7;

// ── MIDI transport framing (EP 0x83 / 0x04) ──────────────────────────────────

TEST(test_midi_in_drops_fill_bytes)
{
    uint8_t pkt[42]; std::memset(pkt, 0xFD, 42);
    pkt[0] = 0xB0; pkt[1] = 0x07; pkt[2] = 0x64;
    pkt[20] = 0xF8;
    uint8_t out[41];
    CHECK_EQ(ExtractMidiIn(pkt, 42, out), 4u);
    const uint8_t want[4] = { 0xB0, 0x07, 0x64, 0xF8 };
    CHECK(std::memcmp(out, want, 4) == 0);
}

TEST(test_midi_in_ignores_byte_41)
{
    uint8_t pkt[42]; std::memset(pkt, 0xFD, 42);
    pkt[41] = 0x90;
    uint8_t out[41];
    CHECK_EQ(ExtractMidiIn(pkt, 42, out), 0u);
    pkt[40] = 0x90;
    CHECK_EQ(ExtractMidiIn(pkt, 42, out), 1u);
}

TEST(test_midi_in_short_transfer_reads_only_actual_bytes)
{
    const uint8_t pkt[5] = { 0x90, 0x40, 0x7F, 0xFD, 0xFD };
    uint8_t out[41];
    CHECK_EQ(ExtractMidiIn(pkt, 3, out), 3u);
    CHECK_EQ(ExtractMidiIn(pkt, 0, out), 0u);
}

TEST(test_midi_out_packet_matches_documented_example)
{
    const uint8_t msg[3] = { 0x90, 0x40, 0x7F };
    uint8_t pkt[42];
    CHECK_EQ(BuildMidiOutPacket(msg, 3, pkt), 3u);
    CHECK_EQ(pkt[0], 0x90); CHECK_EQ(pkt[1], 0x40); CHECK_EQ(pkt[2], 0x7F);
    for (int i = 3; i <= 40; i++) CHECK_EQ(pkt[i], 0xFD);
    CHECK_EQ(pkt[41], 0xE0);
}

TEST(test_midi_out_packet_carries_at_most_39_bytes)
{
    uint8_t msg[50];
    for (int i = 0; i < 50; i++) msg[i] = uint8_t(i);
    uint8_t pkt[42];
    CHECK_EQ(BuildMidiOutPacket(msg, 50, pkt), 39u);
    CHECK_EQ(pkt[38], 38);
    CHECK_EQ(pkt[39], 0xFD); CHECK_EQ(pkt[40], 0xFD); CHECK_EQ(pkt[41], 0xE0);
}

TEST(test_midi_out_packet_empty_input_builds_nothing)
{
    uint8_t pkt[42] = {};
    CHECK_EQ(BuildMidiOutPacket(nullptr, 0, pkt), 0u);
}

// ── Raw MIDI 1.0 byte stream → UMP ───────────────────────────────────────────

static std::vector<uint32_t> ToUmp(std::initializer_list<uint8_t> bytes)
{
    RawMidiToUmp conv; UmpSink sink;
    std::vector<uint8_t> b(bytes);
    conv.Push(b.data(), b.size(), UmpSink::cb, &sink);
    return sink.words;
}

TEST(test_raw_note_on_becomes_ump_type2)
{
    CHECK(ToUmp({ 0x90, 0x40, 0x7F }) == std::vector<uint32_t>{ 0x2090407Fu });
}

TEST(test_raw_running_status_repeats_status)
{
    CHECK((ToUmp({ 0xB0, 0x07, 0x64, 0x07, 0x65 })
           == std::vector<uint32_t>{ 0x20B00764u, 0x20B00765u }));
}

TEST(test_raw_two_byte_channel_messages)
{
    CHECK((ToUmp({ 0xC3, 0x05, 0x06, 0xD1, 0x40 })
           == std::vector<uint32_t>{ 0x20C30500u, 0x20C30600u, 0x20D14000u }));
}

TEST(test_raw_message_split_across_pushes)
{
    RawMidiToUmp conv; UmpSink sink;
    const uint8_t a[2] = { 0x90, 0x40 };
    const uint8_t b[1] = { 0x7F };
    conv.Push(a, 2, UmpSink::cb, &sink);
    CHECK(sink.words.empty());
    conv.Push(b, 1, UmpSink::cb, &sink);
    CHECK(sink.words == std::vector<uint32_t>{ 0x2090407Fu });
}

TEST(test_raw_realtime_inside_message_is_emitted_immediately)
{
    CHECK((ToUmp({ 0x90, 0x40, 0xF8, 0x7F })
           == std::vector<uint32_t>{ 0x10F80000u, 0x2090407Fu }));
}

TEST(test_raw_system_common_messages)
{
    CHECK((ToUmp({ 0xF2, 0x12, 0x34, 0xF3, 0x05, 0xF1, 0x21, 0xF6 })
           == std::vector<uint32_t>{ 0x10F21234u, 0x10F30500u, 0x10F12100u, 0x10F60000u }));
}

TEST(test_raw_system_common_cancels_running_status)
{
    CHECK((ToUmp({ 0x90, 0x40, 0x7F, 0xF6, 0x41, 0x7F })
           == std::vector<uint32_t>{ 0x2090407Fu, 0x10F60000u }));
}

TEST(test_raw_system_common_has_no_running_status)
{
    // Extra data bytes after a complete F1/F3 are not a repeat of it.
    CHECK((ToUmp({ 0xF1, 0x21, 0x22, 0xF3, 0x05, 0x06 })
           == std::vector<uint32_t>{ 0x10F12100u, 0x10F30500u }));
}

TEST(test_raw_undefined_and_fill_bytes_are_dropped)
{
    CHECK((ToUmp({ 0xF9, 0xFD, 0xF4, 0xF5, 0x40, 0x90, 0x40, 0x7F })
           == std::vector<uint32_t>{ 0x2090407Fu }));
}

TEST(test_raw_stray_f7_cancels_running_status)
{
    CHECK((ToUmp({ 0x90, 0x40, 0x7F, 0xF7, 0x41, 0x7F })
           == std::vector<uint32_t>{ 0x2090407Fu }));
}

TEST(test_raw_orphan_data_bytes_are_dropped)
{
    CHECK(ToUmp({ 0x40, 0x7F, 0x00 }).empty());
}

TEST(test_raw_short_sysex_is_single_complete_ump)
{
    CHECK((ToUmp({ 0xF0, 0x01, 0x02, 0x03, 0x04, 0xF7 })
           == std::vector<uint32_t>{ 0x30040102u, 0x03040000u }));
}

TEST(test_raw_empty_sysex)
{
    CHECK((ToUmp({ 0xF0, 0xF7 }) == std::vector<uint32_t>{ 0x30000000u, 0x00000000u }));
}

TEST(test_raw_sysex_of_exactly_six_bytes_is_complete)
{
    CHECK((ToUmp({ 0xF0, 1, 2, 3, 4, 5, 6, 0xF7 })
           == std::vector<uint32_t>{ 0x30060102u, 0x03040506u }));
}

TEST(test_raw_long_sysex_splits_start_continue_end)
{
    // 14 data bytes → start(6) + continue(6) + end(2)
    CHECK((ToUmp({ 0xF0, 1,2,3,4,5,6, 7,8,9,10,11,12, 13,14, 0xF7 })
           == std::vector<uint32_t>{ 0x30160102u, 0x03040506u,
                                     0x30260708u, 0x090A0B0Cu,
                                     0x30320D0Eu, 0x00000000u }));
}

TEST(test_raw_sysex_with_realtime_interleaved)
{
    CHECK((ToUmp({ 0xF0, 1, 0xF8, 2, 0xF7 })
           == std::vector<uint32_t>{ 0x10F80000u, 0x30020102u, 0x00000000u }));
}

TEST(test_raw_sysex_terminated_by_status_byte)
{
    // A status byte inside SysEx ends it (as the original driver does), then
    // is processed normally.
    CHECK((ToUmp({ 0xF0, 1, 2, 0x90, 0x40, 0x7F })
           == std::vector<uint32_t>{ 0x30020102u, 0x00000000u, 0x2090407Fu }));
}

TEST(test_raw_sysex_split_across_pushes)
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

static std::vector<uint8_t> FromUmp(std::initializer_list<uint32_t> words)
{
    ByteSink sink;
    std::vector<uint32_t> w(words);
    UmpToRawMidi(w.data(), w.size(), ByteSink::cb, &sink);
    return sink.bytes;
}

TEST(test_ump_channel_voice_to_raw)
{
    CHECK((FromUmp({ 0x2090407Fu, 0x20C30500u, 0x20E10040u })
           == std::vector<uint8_t>{ 0x90,0x40,0x7F, 0xC3,0x05, 0xE1,0x00,0x40 }));
}

TEST(test_ump_system_messages_to_raw)
{
    CHECK((FromUmp({ 0x10F80000u, 0x10F21234u, 0x10F30500u, 0x10F60000u })
           == std::vector<uint8_t>{ 0xF8, 0xF2,0x12,0x34, 0xF3,0x05, 0xF6 }));
}

TEST(test_ump_complete_sysex_to_raw)
{
    CHECK((FromUmp({ 0x30040102u, 0x03040000u })
           == std::vector<uint8_t>{ 0xF0, 1, 2, 3, 4, 0xF7 }));
}

TEST(test_ump_multi_packet_sysex_to_raw)
{
    CHECK((FromUmp({ 0x30160102u, 0x03040506u, 0x30220708u, 0x00000000u,
                     0x30310900u, 0x00000000u })
           == std::vector<uint8_t>{ 0xF0, 1,2,3,4,5,6, 7,8, 9, 0xF7 }));
}

TEST(test_ump_other_types_skipped_by_length)
{
    // utility (1 word), MIDI 2.0 per-note controller (2 words), 128-bit data (4 words), note on
    CHECK((FromUmp({ 0x00000000u, 0x40004000u, 0xFFFF0000u,
                     0x50000000u, 1u, 2u, 3u, 0x2090407Fu })
           == std::vector<uint8_t>{ 0x90, 0x40, 0x7F }));
}

TEST(test_ump_truncated_message_is_not_read_past_end)
{
    CHECK(FromUmp({ 0x30040102u }).empty());
}

TEST(test_ump_song_select_and_mtc_quarter_frame_to_raw)
{
    CHECK((FromUmp({ 0x10F30500u, 0x10F12100u })
           == std::vector<uint8_t>{ 0xF3, 0x05, 0xF1, 0x21 }));
}

TEST(test_ump_sysex_byte_count_over_six_is_clamped)
{
    // nb = 9 is invalid; at most the six payload bytes are rendered.
    CHECK((FromUmp({ 0x30090102u, 0x03040506u })
           == std::vector<uint8_t>{ 0xF0, 1, 2, 3, 4, 5, 6, 0xF7 }));
}

// Found by prop_queue_ump_random_words_are_harmless: a type 1/2 UMP whose
// status byte is not valid for its type, or whose data bytes have bit 7 set,
// used to be rendered as-is, so a malformed UMP could put a status byte (even
// F0 or FF) in the middle of a message on the wire.
TEST(test_ump_invalid_status_for_type_is_skipped)
{
    CHECK(FromUmp({ 0x20050102u }).empty());   // type 2, data byte as status
    CHECK(FromUmp({ 0x20F80000u }).empty());   // type 2, real-time status
    CHECK(FromUmp({ 0x20F20102u }).empty());   // type 2, system common status
    CHECK(FromUmp({ 0x10903C7Fu }).empty());   // type 1, channel status
    CHECK(FromUmp({ 0x10F00000u }).empty());   // type 1, SysEx start
    CHECK(FromUmp({ 0x10F70000u }).empty());   // type 1, SysEx end
    CHECK(FromUmp({ 0x10F40000u, 0x10F50000u, 0x10F90000u, 0x10FD0000u }).empty());   // undefined
    // Skipping keeps alignment: the following valid messages still render.
    CHECK((FromUmp({ 0x20050102u, 0x10F80000u, 0x2090407Fu })
           == std::vector<uint8_t>{ 0xF8, 0x90, 0x40, 0x7F }));
}

TEST(test_ump_data_bytes_are_masked_to_7_bits)
{
    CHECK((FromUmp({ 0x2090F0FFu, 0x10F2FFFFu, 0x10F3F7AAu })
           == std::vector<uint8_t>{ 0x90, 0x70, 0x7F, 0xF2, 0x7F, 0x7F, 0xF3, 0x77 }));
}

// ── MIDI 2.0 channel voice (UMP type 4) → MIDI 1.0 ──────────────────────────
// Default translation from the UMP and MIDI 2.0 Protocol Specification.

typedef std::vector<uint8_t> Bytes;

TEST(test_ump2_note_on_scales_velocity)
{
    CHECK((FromUmp({ 0x40903C00u, 0xFFFF0000u }) == Bytes{ 0x90, 0x3C, 0x7F }));
    CHECK((FromUmp({ 0x40953C00u, 0x80000000u }) == Bytes{ 0x95, 0x3C, 0x40 }));
}

TEST(test_ump2_note_on_tiny_velocity_is_not_note_off)
{
    // vel16 0x0100 >> 9 == 0; a MIDI 2.0 note on must stay a note on.
    CHECK((FromUmp({ 0x40903C00u, 0x01000000u }) == Bytes{ 0x90, 0x3C, 0x01 }));
    CHECK((FromUmp({ 0x40903C00u, 0x00000000u }) == Bytes{ 0x90, 0x3C, 0x01 }));
}

TEST(test_ump2_note_off_scales_velocity)
{
    CHECK((FromUmp({ 0x40803C00u, 0x80000000u }) == Bytes{ 0x80, 0x3C, 0x40 }));
    CHECK((FromUmp({ 0x40803C00u, 0x0000FFFFu }) == Bytes{ 0x80, 0x3C, 0x00 }));   // attribute ignored
}

TEST(test_ump2_control_change_scales_value)
{
    CHECK((FromUmp({ 0x40B01100u, 0xFE000000u }) == Bytes{ 0xB0, 0x11, 0x7F }));
    CHECK((FromUmp({ 0x40B01100u, 0x00000000u }) == Bytes{ 0xB0, 0x11, 0x00 }));
    CHECK((FromUmp({ 0x40B51100u, 0x80000000u }) == Bytes{ 0xB5, 0x11, 0x40 }));
}

TEST(test_ump2_poly_and_channel_pressure)
{
    CHECK((FromUmp({ 0x40A03C00u, 0x80000000u }) == Bytes{ 0xA0, 0x3C, 0x40 }));
    CHECK((FromUmp({ 0x40D00000u, 0x80000000u }) == Bytes{ 0xD0, 0x40 }));
}

TEST(test_ump2_pitch_bend_takes_top_14_bits)
{
    CHECK((FromUmp({ 0x40E00000u, 0x80000000u }) == Bytes{ 0xE0, 0x00, 0x40 }));
    CHECK((FromUmp({ 0x40E30000u, 0xFFFFFFFFu }) == Bytes{ 0xE3, 0x7F, 0x7F }));
    CHECK((FromUmp({ 0x40E00000u, 0x00000000u }) == Bytes{ 0xE0, 0x00, 0x00 }));
}

TEST(test_ump2_program_change_without_bank)
{
    CHECK((FromUmp({ 0x40C00000u, 0x05000A0Bu }) == Bytes{ 0xC0, 0x05 }));
}

TEST(test_ump2_program_change_with_bank)
{
    CHECK((FromUmp({ 0x40C00001u, 0x05000A0Bu })
           == Bytes{ 0xB0, 0x00, 0x0A, 0xB0, 0x20, 0x0B, 0xC0, 0x05 }));
}

TEST(test_ump2_rpn_becomes_cc_sequence)
{
    CHECK((FromUmp({ 0x40200001u, 0x80000000u })
           == Bytes{ 0xB0,0x65,0x00, 0xB0,0x64,0x01, 0xB0,0x06,0x40, 0xB0,0x26,0x00 }));
}

TEST(test_ump2_nrpn_becomes_cc_sequence)
{
    CHECK((FromUmp({ 0x40320203u, 0xFFFFFFFFu })
           == Bytes{ 0xB2,0x63,0x02, 0xB2,0x62,0x03, 0xB2,0x06,0x7F, 0xB2,0x26,0x7F }));
}

TEST(test_ump2_emits_one_callback_per_message)
{
    struct Count { int calls = 0; size_t last = 0; } c;
    const uint32_t w[] = { 0x40200001u, 0x80000000u };
    UmpToRawMidi(w, 2, [](void * ctx, const uint8_t *, size_t n) {
        auto * x = static_cast<Count *>(ctx); x->calls++; x->last = n;
    }, &c);
    CHECK_EQ(c.calls, 1);
    CHECK_EQ(c.last, size_t(12));
}

TEST(test_ump2_untranslatable_statuses_skipped_aligned)
{
    // per-note RC, per-note AC, relative RPN, relative NRPN, per-note pitch bend,
    // per-note management, then a MIDI 2.0 CC that must still convert.
    CHECK((FromUmp({ 0x40003C01u, 0xFFFFFFFFu,
                     0x40103C01u, 0xFFFFFFFFu,
                     0x40400001u, 0xFFFFFFFFu,
                     0x40500001u, 0xFFFFFFFFu,
                     0x40603C00u, 0xFFFFFFFFu,
                     0x40F03C03u, 0xFFFFFFFFu,
                     0x40B00700u, 0xFE000000u })
           == Bytes{ 0xB0, 0x07, 0x7F }));
}

// Seen on a real NS7 during platter moves: a message split across two
// EP 0x83 transfers must still come out whole.
TEST(test_real_split_pitch_bend_reassembles)
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

// ── MIDI out FIFO ────────────────────────────────────────────────────────────

TEST(test_fifo_read_returns_written_bytes_in_order)
{
    ByteFifo<16> f;
    const uint8_t in[] = { 0x90, 0x3C, 0x7F };
    CHECK(f.Write(in, 3));
    CHECK_EQ(f.Size(), 3u);
    uint8_t out[8] = {};
    CHECK_EQ(f.Read(out, sizeof out), 3u);
    CHECK(memcmp(in, out, 3) == 0);
    CHECK_EQ(f.Size(), 0u);
}

TEST(test_fifo_read_respects_max_and_keeps_rest)
{
    ByteFifo<16> f;
    const uint8_t in[] = { 1, 2, 3, 4, 5 };
    f.Write(in, 5);
    uint8_t out[5] = {};
    CHECK_EQ(f.Read(out, 2), 2u);
    CHECK_EQ(out[0], 1); CHECK_EQ(out[1], 2);
    CHECK_EQ(f.Read(out, 5), 3u);
    CHECK_EQ(out[0], 3); CHECK_EQ(out[2], 5);
}

TEST(test_fifo_wraps_around)
{
    ByteFifo<8> f;
    uint8_t next = 0, expect = 0;
    for (int round = 0; round < 50; round++) {
        uint8_t in[5];
        for (auto & b : in) b = next++;
        CHECK(f.Write(in, 5));
        uint8_t out[5] = {};
        CHECK_EQ(f.Read(out, 5), 5u);
        for (auto b : out) CHECK_EQ(b, expect++);
    }
}

TEST(test_fifo_rejects_message_that_does_not_fit_whole)
{
    ByteFifo<8> f;
    const uint8_t six[6] = { 1, 2, 3, 4, 5, 6 }, three[3] = { 7, 8, 9 };
    CHECK(f.Write(six, 6));
    CHECK(!f.Write(three, 3));      // only 2 bytes free
    CHECK_EQ(f.Size(), 6u);         // nothing partial was written
    CHECK(f.Write(three, 2));       // 2 bytes still fit
    CHECK_EQ(f.Size(), 8u);

    uint8_t out[8] = {};
    CHECK_EQ(f.Read(out, sizeof out), 8u);
    const uint8_t expect[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    CHECK(memcmp(out, expect, 8) == 0);
}

TEST(test_fifo_rejects_write_larger_than_capacity)
{
    ByteFifo<8> f;
    const uint8_t nine[9] = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };
    CHECK(!f.Write(nine, 9));
    CHECK_EQ(f.Size(), 0u);
}

TEST(test_fifo_read_empty_returns_zero)
{
    ByteFifo<8> f;
    uint8_t out[4];
    CHECK_EQ(f.Read(out, sizeof out), 0u);
}

TEST(test_fifo_bytes_written_counts_accepted_writes_only)
{
    ByteFifo<8> f;
    CHECK_EQ(f.BytesWritten(), 0u);
    const uint8_t six[6] = {}, three[3] = {};
    CHECK(f.Write(six, 6));
    CHECK(!f.Write(three, 3));      // rejected: not counted
    uint8_t out[8];
    f.Read(out, sizeof out);        // reading does not change it
    CHECK(f.Write(three, 3));
    CHECK_EQ(f.BytesWritten(), 9u);
}

TEST(test_fifo_discard_drops_everything_queued)
{
    // Consumer side, as after a wake: what CoreMIDI queued while the NS7 was
    // asleep is dropped, not sent as a stale burst.
    ByteFifo<8> f;
    const uint8_t a[5] = { 1, 2, 3, 4, 5 }, b[3] = { 6, 7, 8 };
    CHECK_EQ(f.Discard(), 0u);                  // empty: nothing to drop
    CHECK(f.Write(a, 5));
    uint8_t out[8] = {};
    CHECK_EQ(f.Read(out, 2), 2u);
    CHECK(f.Write(b, 3));                       // wraps
    CHECK_EQ(f.Discard(), 6u);
    CHECK_EQ(f.Size(), 0u);
    CHECK_EQ(f.Read(out, sizeof out), 0u);
    CHECK_EQ(f.BytesWritten(), 8u);             // diagnostics unchanged
    // The whole buffer is free again, and later writes read back in order.
    const uint8_t full[8] = { 9, 10, 11, 12, 13, 14, 15, 16 };
    CHECK(f.Write(full, 8));
    CHECK_EQ(f.Read(out, sizeof out), 8u);
    CHECK(memcmp(out, full, 8) == 0);
}

TEST(test_next_midi_out_packet_after_discard_is_empty)
{
    MidiOutFifo f;
    const uint8_t note[3] = { 0x90, 0x3C, 0x7F };
    for (int j = 0; j < 40; j++) CHECK(f.Write(note, 3));
    f.Discard();
    uint8_t pkt[kMidiPacketBytes];
    CHECK_EQ(NextMidiOutPacket(f, pkt), 0u);
}

TEST(test_queue_ump_channel_voice_as_raw_bytes)
{
    ByteFifo<64> f;
    UmpOutState st = {};
    const uint32_t ump[] = { 0x20903C7Fu, 0x20C00500u };   // note on, program change
    CHECK_EQ(QueueUmpAsRawMidi(ump, 2, f, st), 0u);
    uint8_t out[8] = {};
    CHECK_EQ(f.Read(out, sizeof out), 5u);
    const uint8_t expect[] = { 0x90, 0x3C, 0x7F, 0xC0, 0x05 };
    CHECK(memcmp(out, expect, 5) == 0);
}

TEST(test_queue_ump_sysex_as_raw_bytes)
{
    ByteFifo<64> f;
    UmpOutState st = {};
    const uint32_t ump[] = { 0x30027E7Fu, 0x00000000u };   // complete SysEx, 2 bytes
    CHECK_EQ(QueueUmpAsRawMidi(ump, 2, f, st), 0u);
    uint8_t out[8] = {};
    CHECK_EQ(f.Read(out, sizeof out), 4u);
    const uint8_t expect[] = { 0xF0, 0x7E, 0x7F, 0xF7 };
    CHECK(memcmp(out, expect, 4) == 0);
}

TEST(test_queue_ump_skips_unsupported_types)
{
    ByteFifo<64> f;
    UmpOutState st = {};
    // MIDI 2.0 per-note management (type 4, two words, no MIDI 1.0 form), then a MIDI 1.0 note off.
    const uint32_t ump[] = { 0x40F03C00u, 0x00000000u, 0x20803C00u };
    CHECK_EQ(QueueUmpAsRawMidi(ump, 3, f, st), 0u);
    uint8_t out[8] = {};
    CHECK_EQ(f.Read(out, sizeof out), 3u);
    const uint8_t expect[] = { 0x80, 0x3C, 0x00 };
    CHECK(memcmp(out, expect, 3) == 0);
}

TEST(test_queue_ump_drops_whole_message_when_full)
{
    ByteFifo<4> f;
    UmpOutState st = {};
    const uint32_t ump[] = { 0x20903C7Fu, 0x20903D7Fu };   // 3 bytes each, room for one
    CHECK_EQ(QueueUmpAsRawMidi(ump, 2, f, st), 1u);
    CHECK_EQ(f.Size(), 3u);
}

// SysEx UMP words used below: start(2), continue(2), end(2) forming a single
// 14-byte SysEx (F0 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E F7, 16 bytes).
static const uint32_t kSysExStart[2]    = { 0x30160102u, 0x03040506u };   // kind 1, 6 bytes: 01..06
static const uint32_t kSysExContinue[2] = { 0x30260708u, 0x090A0B0Cu };   // kind 2, 6 bytes: 07..0C
static const uint32_t kSysExEnd[2]      = { 0x30320D0Eu, 0x00000000u };  // kind 3, 2 bytes: 0D 0E

TEST(test_queue_ump_multi_packet_sysex_fits)
{
    ByteFifo<64> f;
    UmpOutState st = {};
    uint32_t ump[6];
    memcpy(ump,     kSysExStart,    sizeof kSysExStart);
    memcpy(ump + 2, kSysExContinue, sizeof kSysExContinue);
    memcpy(ump + 4, kSysExEnd,      sizeof kSysExEnd);
    CHECK_EQ(QueueUmpAsRawMidi(ump, 6, f, st), 0u);
    uint8_t out[16] = {};
    CHECK_EQ(f.Read(out, sizeof out), 16u);
    const uint8_t expect[16] = { 0xF0, 1,2,3,4,5,6, 7,8,9,0xA,0xB,0xC, 0xD,0xE, 0xF7 };
    CHECK(memcmp(out, expect, 16) == 0);
}

TEST(test_queue_ump_dropped_sysex_start_drops_rest)
{
    ByteFifo<8> f;
    UmpOutState st = {};
    const uint8_t filler[4] = { 0xAA, 0xAA, 0xAA, 0xAA };
    CHECK(f.Write(filler, 4));          // only 4 bytes free; a 7-byte start won't fit

    uint32_t ump[6];
    memcpy(ump,     kSysExStart,    sizeof kSysExStart);
    memcpy(ump + 2, kSysExContinue, sizeof kSysExContinue);
    memcpy(ump + 4, kSysExEnd,      sizeof kSysExEnd);
    CHECK_EQ(QueueUmpAsRawMidi(ump, 6, f, st), 3u);   // all three pieces dropped
    CHECK_EQ(f.Size(), 4u);                            // no orphan bytes queued

    const uint32_t noteOn[] = { 0x20903C7Fu };
    CHECK_EQ(QueueUmpAsRawMidi(noteOn, 1, f, st), 0u);   // unrelated message still queues fine
    CHECK_EQ(f.Size(), 7u);
}

TEST(test_queue_ump_sysex_cut_short_is_terminated)
{
    ByteFifo<8> f;
    UmpOutState st = {};
    uint32_t ump[6];
    memcpy(ump,     kSysExStart,    sizeof kSysExStart);
    memcpy(ump + 2, kSysExContinue, sizeof kSysExContinue);
    memcpy(ump + 4, kSysExEnd,      sizeof kSysExEnd);
    // start (7 bytes) fits; continue (6 bytes) doesn't (1 byte free) and is
    // replaced by a terminating F7; end arrives as an orphan and is dropped.
    CHECK_EQ(QueueUmpAsRawMidi(ump, 6, f, st), 2u);
    CHECK_EQ(f.Size(), 8u);
    uint8_t out[8] = {};
    CHECK_EQ(f.Read(out, sizeof out), 8u);
    const uint8_t expect[8] = { 0xF0, 1,2,3,4,5,6, 0xF7 };
    CHECK(memcmp(out, expect, 8) == 0);
}

TEST(test_queue_ump_state_spans_calls)
{
    ByteFifo<64> f;
    UmpOutState st = {};
    CHECK_EQ(QueueUmpAsRawMidi(kSysExStart, 2, f, st), 0u);
    CHECK_EQ(QueueUmpAsRawMidi(kSysExContinue, 2, f, st), 0u);
    CHECK_EQ(QueueUmpAsRawMidi(kSysExEnd, 2, f, st), 0u);
    uint8_t out[16] = {};
    CHECK_EQ(f.Read(out, sizeof out), 16u);
    const uint8_t expect[16] = { 0xF0, 1,2,3,4,5,6, 7,8,9,0xA,0xB,0xC, 0xD,0xE, 0xF7 };
    CHECK(memcmp(out, expect, 16) == 0);
}

TEST(test_queue_ump_trailing_partial_ump_ignored)
{
    ByteFifo<64> f;
    UmpOutState st = {};
    const uint32_t ump[] = { 0x30160102u };   // SysEx start needs a second word
    CHECK_EQ(QueueUmpAsRawMidi(ump, 1, f, st), 0u);
    CHECK_EQ(f.Size(), 0u);
}

// A SysEx end with zero data bytes (kind 3, nb 0): CoreMIDI emits this when
// the payload length is an exact multiple of 6, though our own RawMidiToUmp
// encoder never produces it. UmpToRawMidi renders it as a bare 0xF7.
static const uint32_t kSysExEmptyEnd[2] = { 0x30300000u, 0x00000000u };

TEST(test_queue_ump_empty_sysex_end_closes)
{
    ByteFifo<64> f;
    UmpOutState st = {};
    uint32_t ump[4];
    memcpy(ump,     kSysExStart,    sizeof kSysExStart);
    memcpy(ump + 2, kSysExEmptyEnd, sizeof kSysExEmptyEnd);
    CHECK_EQ(QueueUmpAsRawMidi(ump, 4, f, st), 0u);
    uint8_t out[8] = {};
    CHECK_EQ(f.Read(out, sizeof out), 8u);
    const uint8_t expect[8] = { 0xF0, 1,2,3,4,5,6, 0xF7 };
    CHECK(memcmp(out, expect, 8) == 0);
}

TEST(test_queue_ump_orphan_empty_end_dropped)
{
    ByteFifo<64> f;
    UmpOutState st = {};
    CHECK_EQ(QueueUmpAsRawMidi(kSysExEmptyEnd, 2, f, st), 1u);
    CHECK_EQ(f.Size(), 0u);
}

// A channel/system-common message arriving while a SysEx is still open must
// close it out (F7) before going through itself; later pieces of that SysEx
// then become orphans.
TEST(test_queue_ump_channel_message_closes_open_sysex)
{
    ByteFifo<64> f;
    UmpOutState st = {};
    uint32_t ump[7];
    memcpy(ump,     kSysExStart, sizeof kSysExStart);
    const uint32_t noteOn[] = { 0x20903C7Fu };
    memcpy(ump + 2, noteOn, sizeof noteOn);
    memcpy(ump + 3, kSysExContinue, sizeof kSysExContinue);
    memcpy(ump + 5, kSysExEnd, sizeof kSysExEnd);
    CHECK_EQ(QueueUmpAsRawMidi(ump, 7, f, st), 2u);   // continue and end are orphans
    uint8_t out[16] = {};
    CHECK_EQ(f.Read(out, sizeof out), 11u);
    const uint8_t expect[11] = { 0xF0, 1,2,3,4,5,6, 0xF7, 0x90,0x3C,0x7F };
    CHECK(memcmp(out, expect, 11) == 0);
}

// Real-time bytes (F8-FF) inside an open SysEx pass straight through without
// touching sysExOpen, unlike a channel/system-common message.
TEST(test_queue_ump_realtime_inside_sysex_passes_through)
{
    ByteFifo<64> f;
    UmpOutState st = {};
    uint32_t ump[5];
    memcpy(ump,     kSysExStart, sizeof kSysExStart);
    const uint32_t timingClock[] = { 0x10F80000u };   // type 1, real-time 0xF8
    memcpy(ump + 2, timingClock, sizeof timingClock);
    memcpy(ump + 3, kSysExEnd, sizeof kSysExEnd);
    CHECK_EQ(QueueUmpAsRawMidi(ump, 5, f, st), 0u);
    uint8_t out[16] = {};
    CHECK_EQ(f.Read(out, sizeof out), 11u);
    const uint8_t expect[11] = { 0xF0, 1,2,3,4,5,6, 0xF8, 0xD,0xE, 0xF7 };
    CHECK(memcmp(out, expect, 11) == 0);
}

TEST(test_queue_ump_pending_f7_flushed_when_room)
{
    ByteFifo<8> f;
    UmpOutState st = {};
    const uint8_t filler[1] = { 0xAA };
    CHECK(f.Write(filler, 1));   // 7 bytes free: exactly the start's size

    CHECK_EQ(QueueUmpAsRawMidi(kSysExStart, 2, f, st), 0u);
    CHECK_EQ(f.Size(), 8u);      // FIFO now completely full

    // The continue doesn't fit, and neither does the immediate F7 retry.
    CHECK_EQ(QueueUmpAsRawMidi(kSysExContinue, 2, f, st), 1u);
    CHECK_EQ(f.Size(), 8u);

    const uint32_t noteOn[] = { 0x20903C7Fu };
    CHECK_EQ(QueueUmpAsRawMidi(noteOn, 1, f, st), 1u);   // still full: dropped, order preserved
    CHECK_EQ(f.Size(), 8u);

    uint8_t drain[8] = {};
    CHECK_EQ(f.Read(drain, sizeof drain), 8u);   // free up room

    CHECK_EQ(QueueUmpAsRawMidi(noteOn, 1, f, st), 0u);   // pending F7 flushes, then the note-on queues
    uint8_t out[8] = {};
    CHECK_EQ(f.Read(out, sizeof out), 4u);
    const uint8_t expect[4] = { 0xF7, 0x90, 0x3C, 0x7F };
    CHECK(memcmp(out, expect, 4) == 0);
}

// A channel message that closes an open SysEx is written together with its
// F7 prefix; when that doesn't fit, the message is dropped and the F7 owed.
TEST(test_queue_ump_closing_message_that_does_not_fit_leaves_f7_pending)
{
    ByteFifo<8> f;
    UmpOutState st = {};
    CHECK_EQ(QueueUmpAsRawMidi(kSysExStart, 2, f, st), 0u);   // 7 bytes, 1 free
    const uint32_t noteOn[] = { 0x20903C7Fu };
    CHECK_EQ(QueueUmpAsRawMidi(noteOn, 1, f, st), 1u);         // F7 + 3 bytes won't fit
    CHECK(!st.sysExOpen);
    CHECK(st.pendingF7);
    CHECK_EQ(f.Size(), 7u);

    uint8_t drain[8];
    CHECK_EQ(f.Read(drain, sizeof drain), 7u);
    CHECK_EQ(QueueUmpAsRawMidi(noteOn, 1, f, st), 0u);
    CHECK(!st.pendingF7);
    uint8_t out[8] = {};
    CHECK_EQ(f.Read(out, sizeof out), 4u);
    const uint8_t expect[4] = { 0xF7, 0x90, 0x3C, 0x7F };
    CHECK(memcmp(out, expect, 4) == 0);
}

// A new SysEx start that doesn't fit while an older SysEx is still open
// leaves an F7 owed for the older one.
TEST(test_queue_ump_dropped_start_while_sysex_open_leaves_f7_pending)
{
    ByteFifo<8> f;
    UmpOutState st = {};
    CHECK_EQ(QueueUmpAsRawMidi(kSysExStart, 2, f, st), 0u);   // 7 bytes, 1 free
    CHECK_EQ(QueueUmpAsRawMidi(kSysExStart, 2, f, st), 1u);   // second start won't fit
    CHECK(!st.sysExOpen);
    CHECK(st.pendingF7);
    CHECK_EQ(QueueUmpAsRawMidi(kSysExEnd, 2, f, st), 1u);     // its end is an orphan now
    CHECK_EQ(f.Size(), 8u);                                    // ...though the owed F7 got in
    CHECK(!st.pendingF7);
    uint8_t out[8] = {};
    CHECK_EQ(f.Read(out, sizeof out), 8u);
    const uint8_t expect[8] = { 0xF0, 1,2,3,4,5,6, 0xF7 };
    CHECK(memcmp(out, expect, 8) == 0);
}

// Found by prop_queue_ump_stream_is_well_formed_for_any_fifo_size: a new
// SysEx start arriving while another SysEx is still open used to go out
// without closing the older one, leaving F0 .. F0 .. on the wire. Like a
// channel message, it must be preceded by F7.
TEST(test_queue_ump_new_sysex_start_closes_open_sysex)
{
    ByteFifo<64> f;
    UmpOutState st = {};
    CHECK_EQ(QueueUmpAsRawMidi(kSysExStart, 2, f, st), 0u);
    CHECK_EQ(QueueUmpAsRawMidi(kSysExStart, 2, f, st), 0u);
    CHECK(st.sysExOpen);                                     // the second one is open now
    const uint32_t complete[] = { 0x30027E7Fu, 0x00000000u };   // complete SysEx, 2 bytes
    CHECK_EQ(QueueUmpAsRawMidi(complete, 2, f, st), 0u);
    CHECK(!st.sysExOpen);
    uint8_t out[32] = {};
    CHECK_EQ(f.Read(out, sizeof out), 20u);
    const uint8_t expect[20] = { 0xF0, 1,2,3,4,5,6, 0xF7, 0xF0, 1,2,3,4,5,6, 0xF7,
                                 0xF0, 0x7E, 0x7F, 0xF7 };
    CHECK(memcmp(out, expect, 20) == 0);
}

// The F7 and the new start go in one write; if they don't fit, the start is
// dropped and the F7 is owed, as when any other message closes a SysEx.
TEST(test_queue_ump_new_sysex_start_that_does_not_fit_leaves_f7_pending)
{
    ByteFifo<16> f;
    UmpOutState st = {};
    const uint8_t filler[2] = { 0xAA, 0xAA };
    CHECK(f.Write(filler, 2));
    CHECK_EQ(QueueUmpAsRawMidi(kSysExStart, 2, f, st), 0u);   // 9 used, 7 free
    CHECK_EQ(QueueUmpAsRawMidi(kSysExStart, 2, f, st), 1u);   // F7 + 7 bytes = 8: no room
    CHECK(!st.sysExOpen);
    CHECK(st.pendingF7);
    CHECK_EQ(f.Size(), 9u);
}

TEST(test_queue_ump2_note_on_as_raw_bytes)
{
    ByteFifo<64> f;
    UmpOutState st = {};
    const uint32_t ump[] = { 0x40903C00u, 0xFFFF0000u, 0x40B01100u, 0xFE000000u };
    CHECK_EQ(QueueUmpAsRawMidi(ump, 4, f, st), 0u);
    uint8_t out[8] = {};
    CHECK_EQ(f.Read(out, sizeof out), 6u);
    const uint8_t expect[] = { 0x90, 0x3C, 0x7F, 0xB0, 0x11, 0x7F };
    CHECK(memcmp(out, expect, 6) == 0);
}

TEST(test_queue_ump2_message_closes_open_sysex)
{
    ByteFifo<64> f;
    UmpOutState st = {};
    uint32_t ump[4];
    memcpy(ump, kSysExStart, sizeof kSysExStart);
    const uint32_t cc[] = { 0x40B01100u, 0xFE000000u };
    memcpy(ump + 2, cc, sizeof cc);
    CHECK_EQ(QueueUmpAsRawMidi(ump, 4, f, st), 0u);
    CHECK(!st.sysExOpen);
    uint8_t out[16] = {};
    CHECK_EQ(f.Read(out, sizeof out), 11u);
    const uint8_t expect[11] = { 0xF0, 1,2,3,4,5,6, 0xF7, 0xB0,0x11,0x7F };
    CHECK(memcmp(out, expect, 11) == 0);
}

TEST(test_queue_ump2_rpn_closes_open_sysex)
{
    // Longest translation (12 bytes) plus the F7 prefix: 13 bytes in one write.
    ByteFifo<64> f;
    UmpOutState st = {};
    uint32_t ump[4];
    memcpy(ump, kSysExStart, sizeof kSysExStart);
    const uint32_t rpn[] = { 0x40200001u, 0x80000000u };
    memcpy(ump + 2, rpn, sizeof rpn);
    CHECK_EQ(QueueUmpAsRawMidi(ump, 4, f, st), 0u);
    uint8_t out[32] = {};
    CHECK_EQ(f.Read(out, sizeof out), 20u);
    const uint8_t expect[20] = { 0xF0, 1,2,3,4,5,6, 0xF7,
                                 0xB0,0x65,0x00, 0xB0,0x64,0x01, 0xB0,0x06,0x40, 0xB0,0x26,0x00 };
    CHECK(memcmp(out, expect, 20) == 0);
}

TEST(test_next_packet_takes_at_most_39_bytes)
{
    ByteFifo<64> f;
    uint8_t in[50];
    for (uint8_t i = 0; i < 50; i++) in[i] = i;
    f.Write(in, 50);

    uint8_t pkt[kMidiPacketBytes];
    CHECK_EQ(NextMidiOutPacket(f, pkt), 39u);
    CHECK(memcmp(pkt, in, 39) == 0);
    CHECK_EQ(pkt[39], kMidiFill);
    CHECK_EQ(pkt[40], kMidiFill);
    CHECK_EQ(pkt[41], kMidiCPort);

    CHECK_EQ(NextMidiOutPacket(f, pkt), 11u);
    CHECK(memcmp(pkt, in + 39, 11) == 0);
    CHECK_EQ(pkt[11], kMidiFill);
    CHECK_EQ(pkt[41], kMidiCPort);
}

TEST(test_next_packet_empty_fifo_returns_zero)
{
    ByteFifo<64> f;
    uint8_t pkt[kMidiPacketBytes];
    memset(pkt, 0xAA, sizeof pkt);
    CHECK_EQ(NextMidiOutPacket(f, pkt), 0u);
    for (uint32_t i = 0; i < kMidiPacketBytes; i++) CHECK_EQ(pkt[i], 0xAA);
}
