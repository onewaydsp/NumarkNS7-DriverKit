// StreamKeeperTests.cpp
// Tests for NS7::StreamKeeper, the request-slot policy NumarkNS7Device.cpp
// drives for the four streaming endpoints that keep requests queued: EP 0x02
// (playback, iso), EP 0x81 (feedback, iso), EP 0x86 (capture, bulk) and
// EP 0x83 (MIDI in, bulk). The driver does the I/O; the keeper decides which
// slots to (re)submit, when a STALL must be cleared first, how failed slots
// back off, which iso pipe the watchdog aborts, and gates everything on
// system sleep.
//
// Hardware context: after a Mac sleep/wake every iso resubmit failed with
// kIOReturnIsoTooNew; a failed submission never completes, so the slots were
// lost and both chains (and with them the stats line and MIDI) died.

#include "NS7Protocol.h"
#include "TestHarness.h"

#include <cstring>
#include <vector>

using namespace NS7;

namespace {

typedef StreamKeeper SK;
typedef SK::Next Next;
typedef SK::Submit Submit;

constexpr uint32_t kOk      = 0;
constexpr uint32_t kStall   = 0xe0005000;   // kUSBHostReturnPipeStalled
constexpr uint32_t kErr     = 0xe00002bc;   // kIOReturnError

constexpr uint32_t kCounts[SK::kGroupCount] = { 4, 8, 3, 16 };
constexpr uint32_t kTotal = 4 + 8 + 3 + 16;

SK Fresh()
{
    SK k;
    k.Init(kCounts[0], kCounts[1], kCounts[2], kCounts[3]);
    return k;
}

std::vector<SK::Work> Collect(SK & k)
{
    SK::Work w[SK::kGroupCount * SK::kMaxSlots];
    const uint32_t n = k.Collect(w, sizeof w / sizeof w[0]);
    return std::vector<SK::Work>(w, w + n);
}

// Submits every collected slot with result `r`.
uint32_t SubmitAll(SK & k, Submit r = Submit::Ok)
{
    const auto work = Collect(k);
    for (const auto & w : work) k.OnSubmitted(SK::Group(w.group), w.slot, r);
    return uint32_t(work.size());
}

// A keeper with every slot armed, as after StartStreaming.
SK Running()
{
    SK k = Fresh();
    CHECK_EQ(SubmitAll(k), kTotal);
    return k;
}

uint32_t Tick(SK & k, Submit r, uint32_t * submitted);

// Running, after the first tick. The start submissions count as a re-arm,
// so a pipe's stuck count starts from the tick after them.
SK Settled()
{
    SK k = Running();
    CHECK_EQ(Tick(k, Submit::Ok, nullptr), 0u);
    return k;
}

uint32_t ArmedTotal(const SK & k)
{
    uint32_t n = 0;
    for (uint32_t g = 0; g < SK::kGroupCount; g++) n += k.Armed(SK::Group(g));
    return n;
}

// One watchdog tick: begin, collect and submit with `r`, end. Returns the
// abort mask; *submitted gets how many slots were submitted.
uint32_t Tick(SK & k, Submit r = Submit::Ok, uint32_t * submitted = nullptr);
uint32_t Tick(SK & k, Submit r, uint32_t * submitted)
{
    k.BeginTick();
    const uint32_t n = SubmitAll(k, r);
    if (submitted) *submitted = n;
    return k.EndTick();
}

} // namespace

TEST(test_keeper_start_collects_every_slot_once_in_order)
{
    SK k = Fresh();
    const auto work = Collect(k);
    CHECK_EQ(uint32_t(work.size()), kTotal);
    if (work.size() != kTotal) return;
    // Feedback first so playback follows the device's rate from the start.
    CHECK_EQ(work[0].group, uint8_t(SK::kFeedback));
    CHECK_EQ(work[8].group, uint8_t(SK::kPlayback));
    CHECK_EQ(work[12].group, uint8_t(SK::kCapture));
    CHECK_EQ(work[15].group, uint8_t(SK::kMidiIn));
    for (const auto & w : work) CHECK(!w.clearStall);
    // Pending slots are never handed out twice.
    CHECK_EQ(uint32_t(Collect(k).size()), 0u);
}

TEST(test_keeper_ok_completion_resubmits_same_slot)
{
    SK k = Running();
    CHECK(k.OnCompletion(SK::kFeedback, 3, kOk) == Next::Submit);
    CHECK(!k.IsArmed(SK::kFeedback, 3));
    CHECK_EQ(uint32_t(Collect(k).size()), 0u);          // pending: no double submit
    k.OnSubmitted(SK::kFeedback, 3, Submit::Ok);
    CHECK(k.IsArmed(SK::kFeedback, 3));
    CHECK(k.OnCompletion(SK::kCapture, 1, 0xe00002e7) == Next::Submit);   // underrun is ok
}

TEST(test_keeper_completion_for_unarmed_slot_is_ignored)
{
    SK k = Running();
    CHECK(k.OnCompletion(SK::kPlayback, 0, kOk) == Next::Submit);
    CHECK(k.OnCompletion(SK::kPlayback, 0, kOk) == Next::Idle);   // pending, not armed
    CHECK(k.OnCompletion(SK::kPlayback, 99, kOk) == Next::Idle);  // out of range
}

TEST(test_keeper_failed_submit_is_retried_only_from_ticks)
{
    SK k = Running();
    CHECK(k.OnCompletion(SK::kFeedback, 0, kOk) == Next::Submit);
    k.OnSubmitted(SK::kFeedback, 0, Submit::Error);           // e.g. IsoTooNew twice
    CHECK_EQ(k.Failures(SK::kFeedback, 0), 1u);
    // Completions elsewhere never retry it.
    for (int j = 0; j < 100; j++) {
        CHECK(k.OnCompletion(SK::kFeedback, 1, kOk) == Next::Submit);
        k.OnSubmitted(SK::kFeedback, 1, Submit::Ok);
        CHECK_EQ(uint32_t(Collect(k).size()), 0u);
    }
    uint32_t n = 0;
    Tick(k, Submit::Ok, &n);
    CHECK_EQ(n, 1u);
    CHECK(k.IsArmed(SK::kFeedback, 0));
}

TEST(test_keeper_backoff_doubles_and_caps)
{
    SK k = Running();
    CHECK(k.OnCompletion(SK::kPlayback, 2, kOk) == Next::Submit);
    k.OnSubmitted(SK::kPlayback, 2, Submit::Error);
    // Retry gaps in ticks: 1, 2, 4, 8, 16, 16, 16.
    const uint32_t expect[] = { 1, 2, 4, 8, 16, 16, 16 };
    for (uint32_t gap : expect) {
        uint32_t waited = 0, n = 0;
        do { Tick(k, Submit::Error, &n); waited++; } while (n == 0 && waited < 100);
        CHECK_EQ(waited, gap);
        CHECK_EQ(n, 1u);                                  // one attempt per due tick
    }
    CHECK_EQ(SK::kMaxBackoffTicks, 16u);
}

TEST(test_keeper_ok_completion_resets_backoff)
{
    SK k = Running();
    CHECK(k.OnCompletion(SK::kMidiIn, 5, kErr) == Next::Idle);     // bulk error: idle, backed off
    CHECK_EQ(k.Failures(SK::kMidiIn, 5), 1u);
    uint32_t n = 0;
    Tick(k, Submit::Ok, &n);
    CHECK_EQ(n, 1u);
    CHECK(k.OnCompletion(SK::kMidiIn, 5, kOk) == Next::Submit);
    CHECK_EQ(k.Failures(SK::kMidiIn, 5), 0u);
}

TEST(test_keeper_bulk_stall_on_completion_clears_then_submits)
{
    SK k = Running();
    CHECK(k.OnCompletion(SK::kCapture, 0, kStall) == Next::ClearStallThenSubmit);
    k.OnSubmitted(SK::kCapture, 0, Submit::Ok);
    CHECK(k.IsArmed(SK::kCapture, 0));
}

TEST(test_keeper_stalled_submit_retries_with_clear_stall_and_backoff)
{
    SK k = Running();
    CHECK(k.OnCompletion(SK::kMidiIn, 0, kStall) == Next::ClearStallThenSubmit);
    k.OnSubmitted(SK::kMidiIn, 0, Submit::Stalled);   // ClearStall failed, or AsyncIO said stalled
    CHECK_EQ(uint32_t(Collect(k).size()), 0u);         // not before a tick
    k.BeginTick();
    auto work = Collect(k);
    CHECK_EQ(uint32_t(work.size()), 1u);
    if (work.size() != 1) return;
    CHECK(work[0].clearStall);
    k.OnSubmitted(SK::kMidiIn, 0, Submit::Stalled);
    k.EndTick();
    // Next attempt two ticks later, still clearing the STALL first.
    k.BeginTick(); CHECK_EQ(uint32_t(Collect(k).size()), 0u); k.EndTick();
    k.BeginTick();
    work = Collect(k);
    CHECK_EQ(uint32_t(work.size()), 1u);
    if (work.size() != 1) return;
    CHECK(work[0].clearStall);
    k.OnSubmitted(SK::kMidiIn, 0, Submit::Ok);
    k.EndTick();
    CHECK(k.IsArmed(SK::kMidiIn, 0));
    // Once through, later retries don't clear a STALL that isn't there.
    CHECK(k.OnCompletion(SK::kMidiIn, 0, kErr) == Next::Idle);
    k.BeginTick();
    work = Collect(k);
    CHECK_EQ(uint32_t(work.size()), 1u);
    if (work.size() != 1) return;
    CHECK(!work[0].clearStall);
}

TEST(test_keeper_iso_errors_resubmit_but_going_away_waits_for_tick)
{
    SK k = Running();
    CHECK(k.OnCompletion(SK::kPlayback, 0, kErr) == Next::Submit);               // as before
    k.OnSubmitted(SK::kPlayback, 0, Submit::Ok);
    CHECK(k.OnCompletion(SK::kPlayback, 1, kReturnAborted) == Next::Idle);
    CHECK(k.OnCompletion(SK::kFeedback, 1, kReturnNotResponding) == Next::Idle);
    CHECK_EQ(uint32_t(Collect(k).size()), 0u);
    uint32_t n = 0;
    Tick(k, Submit::Ok, &n);
    CHECK_EQ(n, 2u);
}

TEST(test_keeper_bulk_errors_wait_for_tick)
{
    SK k = Running();
    CHECK(k.OnCompletion(SK::kCapture, 2, kReturnAborted) == Next::Idle);
    CHECK(k.OnCompletion(SK::kMidiIn, 7, kErr) == Next::Idle);
    uint32_t n = 0;
    Tick(k, Submit::Ok, &n);
    CHECK_EQ(n, 2u);
    CHECK_EQ(ArmedTotal(k), kTotal);
}

TEST(test_keeper_no_slot_lost_under_random_events)
{
    // Random completions and submit results; after sleep-free recovery ticks
    // with every submission succeeding, every slot is armed again.
    SK k = Running();
    uint32_t rng = test::Seed();
    auto next = [&rng] { rng = rng * 1664525u + 1013904223u; return rng >> 8; };
    const uint32_t statuses[] = { kOk, kErr, kStall, kReturnAborted, kReturnNoDevice, 0xe00002e7 };
    for (int step = 0; step < 20000; step++) {
        const auto g = SK::Group(next() % SK::kGroupCount);
        const uint32_t i = next() % kCounts[g];
        switch (next() % 4) {
        case 0: case 1: {
            if (!k.IsArmed(g, i)) break;
            const Next a = k.OnCompletion(g, i, statuses[next() % 6]);
            if (a != Next::Idle)
                k.OnSubmitted(g, i, next() % 3 ? Submit::Ok : (next() % 2 ? Submit::Error : Submit::Stalled));
            break;
        }
        case 2: {
            k.BeginTick();
            for (const auto & w : Collect(k))
                k.OnSubmitted(SK::Group(w.group), w.slot, next() % 2 ? Submit::Ok : Submit::Error);
            k.EndTick();
            break;
        }
        default: break;
        }
        CHECK(ArmedTotal(k) <= kTotal);
    }
    for (int t = 0; t < 40; t++) Tick(k, Submit::Ok);
    CHECK_EQ(ArmedTotal(k), kTotal);
}

TEST(test_keeper_stuck_iso_pipe_aborted_after_stuck_ticks_only_that_pipe)
{
    SK k = Settled();
    for (uint32_t t = 1; t < SK::kStuckTicks; t++) {
        // Feedback keeps completing; playback is silent.
        CHECK(k.OnCompletion(SK::kFeedback, t % 8, kOk) == Next::Submit);
        k.OnSubmitted(SK::kFeedback, t % 8, Submit::Ok);
        CHECK_EQ(Tick(k), 0u);
    }
    CHECK(k.OnCompletion(SK::kFeedback, 0, kOk) == Next::Submit);
    k.OnSubmitted(SK::kFeedback, 0, Submit::Ok);
    CHECK_EQ(Tick(k), 1u << SK::kPlayback);
    // The count restarts after an abort.
    CHECK_EQ(Tick(k) & (1u << SK::kPlayback), 0u);
}

TEST(test_keeper_both_iso_pipes_stuck_both_aborted)
{
    SK k = Settled();
    uint32_t mask = 0;
    for (uint32_t t = 0; t < SK::kStuckTicks; t++) mask = Tick(k);
    CHECK_EQ(mask, (1u << SK::kPlayback) | (1u << SK::kFeedback));
}

TEST(test_keeper_no_abort_when_nothing_armed)
{
    SK k = Running();
    // Every playback request comes back aborted, then every retry fails.
    for (uint32_t i = 0; i < 4; i++) CHECK(k.OnCompletion(SK::kPlayback, i, kReturnAborted) == Next::Idle);
    CHECK_EQ(k.Armed(SK::kPlayback), 0u);
    for (uint32_t t = 0; t < 5 * SK::kStuckTicks; t++) {
        CHECK(k.OnCompletion(SK::kFeedback, t % 8, kOk) == Next::Submit);
        k.OnSubmitted(SK::kFeedback, t % 8, Submit::Ok);
        CHECK_EQ(Tick(k, Submit::Error), 0u);
    }
}

TEST(test_keeper_rearm_restarts_the_stuck_count)
{
    // Feedback slot 0 comes back before tick 1 and its resubmit fails; the
    // retry at tick 1 fails too, the one at tick 3 succeeds. A pipe that was
    // just re-armed gets a full kStuckTicks before it can be aborted, while
    // silent playback is aborted on schedule (its count starts after tick 1,
    // which saw the start submissions).
    SK k = Running();
    CHECK(k.OnCompletion(SK::kFeedback, 0, kOk) == Next::Submit);
    k.OnSubmitted(SK::kFeedback, 0, Submit::Error);
    for (uint32_t t = 1; t <= 3 + SK::kStuckTicks; t++) {
        uint32_t n = 0;
        const uint32_t mask = Tick(k, t == 1 ? Submit::Error : Submit::Ok, &n);
        if (t == 1 || t == 3) CHECK_EQ(n, 1u);
        CHECK_EQ(bool(mask & (1u << SK::kPlayback)), t == 1 + SK::kStuckTicks);
        CHECK_EQ(bool(mask & (1u << SK::kFeedback)), t == 3 + SK::kStuckTicks);
    }
}

TEST(test_keeper_sleep_leaves_everything_idle_until_wake)
{
    SK k = Running();
    k.OnPowerOff();
    CHECK(k.Sleeping());
    // Everything comes back aborted (the driver aborts all pipes), OK or stalled.
    for (uint32_t g = 0; g < SK::kGroupCount; g++)
        for (uint32_t i = 0; i < kCounts[g]; i++)
            CHECK(k.OnCompletion(SK::Group(g), i, i == 0 ? kOk : (i == 1 ? kStall : kReturnAborted)) == Next::Idle);
    CHECK_EQ(ArmedTotal(k), 0u);
    for (uint32_t t = 0; t < 5 * SK::kStuckTicks; t++) {
        uint32_t n = 99;
        CHECK_EQ(Tick(k, Submit::Ok, &n), 0u);   // no aborts while asleep
        CHECK_EQ(n, 0u);                         // no submits while asleep
    }
}

TEST(test_keeper_wake_rearms_every_slot_once)
{
    SK k = Running();
    // Some slots already failed with long backoffs before sleep.
    CHECK(k.OnCompletion(SK::kFeedback, 2, kReturnAborted) == Next::Idle);
    for (int t = 0; t < 3; t++) Tick(k, Submit::Error);
    CHECK(k.Failures(SK::kFeedback, 2) > 1);
    k.OnPowerOff();
    for (uint32_t g = 0; g < SK::kGroupCount; g++)
        for (uint32_t i = 0; i < kCounts[g]; i++)
            if (k.IsArmed(SK::Group(g), i)) k.OnCompletion(SK::Group(g), i, kReturnAborted);
    k.OnPowerOn();
    CHECK(!k.Sleeping());
    CHECK_EQ(k.Failures(SK::kFeedback, 2), 0u);
    const auto work = Collect(k);
    CHECK_EQ(uint32_t(work.size()), kTotal);
    bool seen[SK::kGroupCount][SK::kMaxSlots] = {};
    for (const auto & w : work) {
        CHECK(!seen[w.group][w.slot]);
        seen[w.group][w.slot] = true;
        k.OnSubmitted(SK::Group(w.group), w.slot, Submit::Ok);
    }
    CHECK_EQ(uint32_t(Collect(k).size()), 0u);
    CHECK_EQ(ArmedTotal(k), kTotal);
}

TEST(test_keeper_wake_does_not_resubmit_requests_still_in_flight)
{
    SK k = Running();
    k.OnPowerOff();
    CHECK(k.OnCompletion(SK::kPlayback, 0, kReturnAborted) == Next::Idle);
    k.OnPowerOn();
    // Only the one that came back; the rest are still armed (their abort
    // completions are on the way).
    CHECK_EQ(uint32_t(Collect(k).size()), 1u);
    // A late abort completion after wake is retried from the next tick.
    CHECK(k.OnCompletion(SK::kPlayback, 1, kReturnAborted) == Next::Idle);
    uint32_t n = 0;
    k.OnSubmitted(SK::kPlayback, 0, Submit::Ok);
    Tick(k, Submit::Ok, &n);
    CHECK_EQ(n, 1u);
}

TEST(test_keeper_wake_keeps_pending_stall_clear)
{
    SK k = Running();
    CHECK(k.OnCompletion(SK::kMidiIn, 3, kStall) == Next::ClearStallThenSubmit);
    k.OnSubmitted(SK::kMidiIn, 3, Submit::Stalled);
    k.OnPowerOff();
    k.OnPowerOn();
    const auto work = Collect(k);
    CHECK_EQ(uint32_t(work.size()), 1u);
    if (work.size() != 1) return;
    CHECK(work[0].clearStall);
}

TEST(test_keeper_collect_respects_capacity)
{
    SK k = Fresh();
    SK::Work w[5];
    CHECK_EQ(k.Collect(w, 5), 5u);
    for (const auto & x : w) k.OnSubmitted(SK::Group(x.group), x.slot, Submit::Ok);
    SK::Work rest[64];
    CHECK_EQ(k.Collect(rest, 64), kTotal - 5);
}

// ── Retry frame (EP 0x02/0x81 IsoTooNew/IsoTooOld) ───────────────────────────

TEST(test_iso_retry_frame_follows_queue_end_when_plausible)
{
    CHECK_EQ(IsoRetryFrame(1000, 10, 0), 1010ull);        // nothing queued
    CHECK_EQ(IsoRetryFrame(1000, 10, 1005), 1010ull);     // queue ends before now + lead
    CHECK_EQ(IsoRetryFrame(1000, 10, 1030), 1030ull);     // stay monotonic after the queue
    CHECK_EQ(IsoRetryFrame(1000, 10, 1000 + kIsoMaxAheadFrames), 1000ull + kIsoMaxAheadFrames);
    CHECK_EQ(IsoRetryFrame(1000, 10, 1001 + kIsoMaxAheadFrames), 1010ull);   // stale: bus moved
    CHECK_EQ(IsoRetryFrame(3, 10, 5000000), 13ull);                           // counter reset
}
