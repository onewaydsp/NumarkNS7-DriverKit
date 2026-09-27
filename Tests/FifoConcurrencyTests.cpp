// FifoConcurrencyTests.cpp
// ByteFifo with a real producer thread and a real consumer thread, as in the
// dext (CoreMIDI's real-time thread writes, the USB queue reads). Run under
// ASan/UBSan by `make -C Tests` and under ThreadSanitizer by
// `make -C Tests tsan`, which checks the acquire/release pairing.

#include "NS7Protocol.h"
#include "TestHarness.h"

#include <atomic>
#include <chrono>
#include <random>
#include <thread>
#include <vector>

using namespace NS7;

namespace {

// Message on the wire: length byte, 32-bit sequence number, then payload
// bytes derived from (sequence, index), so any loss, duplication, reorder or
// torn write shows up.
uint8_t PayloadByte(uint32_t seq, uint32_t i) { return uint8_t(seq * 31u + i * 7u + 1u); }

template <uint32_t N>
void RunProducerConsumer(uint32_t maxMessages, std::chrono::milliseconds budget, uint32_t salt)
{
    ByteFifo<N> fifo;
    std::atomic<bool>     producerDone { false };
    std::atomic<uint32_t> produced { 0 };
    const auto deadline = std::chrono::steady_clock::now() + budget;

    std::thread producer([&] {
        std::mt19937 r(test::Seed() ^ salt);
        uint8_t msg[64];
        uint32_t seq = 0;
        for (; seq < maxMessages; seq++) {
            if ((seq & 255) == 0 && std::chrono::steady_clock::now() > deadline) break;
            const uint32_t len = 5 + r() % 40;              // 5..44 bytes, header included
            msg[0] = uint8_t(len);
            msg[1] = uint8_t(seq); msg[2] = uint8_t(seq >> 8);
            msg[3] = uint8_t(seq >> 16); msg[4] = uint8_t(seq >> 24);
            for (uint32_t i = 5; i < len; i++) msg[i] = PayloadByte(seq, i);
            while (!fifo.Write(msg, len)) std::this_thread::yield();   // all or nothing
            produced.store(seq + 1, std::memory_order_release);
        }
        producerDone.store(true, std::memory_order_release);
    });

    // Consumer: random read sizes, reassembling messages across reads.
    std::mt19937 r(test::Seed() ^ salt ^ 0x5A5A5A5Au);
    std::vector<uint8_t> pending;
    uint8_t chunk[N];
    uint32_t expectSeq = 0, badSize = 0, badMsg = 0;
    bool stop = false;
    while (!stop) {
        const bool done = producerDone.load(std::memory_order_acquire);
        const uint32_t size = fifo.Size();
        if (size > N) badSize++;
        const uint32_t n = fifo.Read(chunk, 1 + r() % N);
        pending.insert(pending.end(), chunk, chunk + n);
        size_t off = 0;
        while (pending.size() - off >= 5 && pending.size() - off >= pending[off]) {
            const uint8_t * m = pending.data() + off;
            const uint32_t len = m[0];
            const uint32_t seq = uint32_t(m[1]) | uint32_t(m[2]) << 8 | uint32_t(m[3]) << 16
                               | uint32_t(m[4]) << 24;
            bool ok = len >= 5 && seq == expectSeq;
            for (uint32_t i = 5; ok && i < len; i++) ok = m[i] == PayloadByte(seq, i);
            if (!ok) { badMsg++; stop = true; break; }
            expectSeq++;
            off += len;
        }
        pending.erase(pending.begin(), pending.begin() + off);
        if (n == 0) {
            if (done && fifo.Size() == 0) stop = true;
            else std::this_thread::yield();
        }
    }
    producer.join();

    CHECK_EQ(badMsg, 0u);
    CHECK_EQ(badSize, 0u);
    CHECK_EQ(expectSeq, produced.load());          // nothing lost or duplicated
    CHECK(pending.empty());                          // no torn trailing message
    CHECK(expectSeq > 1000u);                        // the run did real work
    std::printf("  ByteFifo<%u>: %u messages across threads\n", N, expectSeq);
}

} // namespace

// A small FIFO keeps the producer blocked on "full" and wraps constantly.
TEST(test_fifo_two_threads_small_buffer_no_loss_in_order)
{
    RunProducerConsumer<64>(100000, std::chrono::milliseconds(400), 0x0101);
}

// The dext's size.
TEST(test_fifo_two_threads_midi_out_size_no_loss_in_order)
{
    RunProducerConsumer<kMidiOutFifoBytes>(100000, std::chrono::milliseconds(400), 0x0202);
}

// The consumer drops everything queued now and then (as the dext does on
// wake) while the producer keeps writing. Whole messages are written, so
// after a discard the next byte read starts a message: every message read
// must be intact and in order, with only whole messages skipped.
TEST(test_fifo_two_threads_consumer_discard_keeps_message_boundaries)
{
    constexpr uint32_t N = 256;
    ByteFifo<N> fifo;
    std::atomic<bool> producerDone { false };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);

    std::thread producer([&] {
        std::mt19937 r(test::Seed() ^ 0x0303);
        uint8_t msg[64];
        for (uint32_t seq = 0; seq < 200000; seq++) {
            if ((seq & 255) == 0 && std::chrono::steady_clock::now() > deadline) break;
            const uint32_t len = 5 + r() % 40;
            msg[0] = uint8_t(len);
            msg[1] = uint8_t(seq); msg[2] = uint8_t(seq >> 8);
            msg[3] = uint8_t(seq >> 16); msg[4] = uint8_t(seq >> 24);
            for (uint32_t i = 5; i < len; i++) msg[i] = PayloadByte(seq, i);
            while (!fifo.Write(msg, len)) std::this_thread::yield();
        }
        producerDone.store(true, std::memory_order_release);
    });

    std::mt19937 r(test::Seed() ^ 0x0404);
    std::vector<uint8_t> pending;
    uint8_t chunk[N];
    uint32_t nextMin = 0, good = 0, discards = 0, badMsg = 0;
    bool stop = false;
    while (!stop) {
        const bool done = producerDone.load(std::memory_order_acquire);
        if (r() % 64 == 0) {
            fifo.Discard();
            pending.clear();          // the rest of a partly read message is gone too
            discards++;
            continue;
        }
        const uint32_t n = fifo.Read(chunk, 1 + r() % N);
        pending.insert(pending.end(), chunk, chunk + n);
        size_t off = 0;
        while (pending.size() - off >= 5 && pending.size() - off >= pending[off]) {
            const uint8_t * m = pending.data() + off;
            const uint32_t len = m[0];
            const uint32_t seq = uint32_t(m[1]) | uint32_t(m[2]) << 8 | uint32_t(m[3]) << 16
                               | uint32_t(m[4]) << 24;
            bool ok = len >= 5 && seq >= nextMin;
            for (uint32_t i = 5; ok && i < len; i++) ok = m[i] == PayloadByte(seq, i);
            if (!ok) { badMsg++; stop = true; break; }
            nextMin = seq + 1;
            good++;
            off += len;
        }
        pending.erase(pending.begin(), pending.begin() + off);
        if (n == 0) {
            if (done && fifo.Size() == 0) stop = true;
            else std::this_thread::yield();
        }
    }
    producer.join();

    CHECK_EQ(badMsg, 0u);
    CHECK(pending.empty());
    CHECK(discards > 10u);
    CHECK(good > 1000u);
    std::printf("  ByteFifo<%u>: %u messages, %u discards across threads\n", N, good, discards);
}
