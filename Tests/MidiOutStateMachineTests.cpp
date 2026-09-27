// MidiOutStateMachineTests.cpp
// Tests for NS7::MidiOutStateMachine, the EP 0x04 send/stall/retry/backoff
// decisions that NumarkNS7Device.cpp drives. The driver does the I/O (AsyncIO,
// ClearStall, logging); the state machine says what to do next and keeps the
// in-flight length, retry and backoff state and the sent/lost counters.
//
// Hardware context: the device NAKs EP 0x04 when idle and can STALL it; a
// STALL is cleared and the packet resent up to kMidiOutStallRetries times,
// after which it is dropped (counted lost) and MIDI out pauses for
// kMidiOutStallBackoffTicks feedback ticks (~1 s) so a pipe that keeps
// stalling cannot crowd the feedback chain.

#include "NS7Protocol.h"
#include "TestHarness.h"

#include <cstring>
#include <type_traits>

using namespace NS7;

namespace {

typedef MidiOutStateMachine SM;
typedef SM::Action Action;
typedef SM::Result Result;

// A machine with one 3-byte packet submitted and in flight.
SM InFlight(uint32_t bytes = 3)
{
    SM m = {};
    CHECK(m.CanStart());
    CHECK(m.OnPacketReady(bytes) == Action::Send);
    CHECK(m.OnSendResult(Result::Ok) == Action::Idle);
    return m;
}

// Drives a completion STALL through ClearStall, as the driver does.
Action StallComplete(SM & m, bool clearOk)
{
    const Action a = m.OnComplete(Result::Stalled);
    CHECK(a == Action::ClearStall);
    return m.OnStallCleared(clearOk);
}

} // namespace

TEST(test_sm_constants_match_driver_policy)
{
    CHECK_EQ(kMidiOutStallRetries, 3u);
    CHECK_EQ(kMidiOutStallBackoffTicks, 250u);
}

// The dext allocates its ivars with IONewZero: all-zero must be a valid idle machine.
TEST(test_sm_zero_filled_is_idle)
{
    static_assert(std::is_trivially_copyable<SM>::value, "zero-fillable");
    SM m;
    memset(static_cast<void *>(&m), 0, sizeof m);
    CHECK(m.CanStart());
    CHECK_EQ(m.InFlightBytes(), 0u);
    CHECK_EQ(m.BackoffTicks(), 0u);
    CHECK_EQ(m.Retries(), 0u);
    CHECK_EQ(m.PacketsSent(), 0ull);
    CHECK_EQ(m.BytesSent(), 0ull);
    CHECK_EQ(m.PacketsLost(), 0ull);
    CHECK(!m.TakePauseLog());
}

TEST(test_sm_empty_packet_sends_nothing)
{
    SM m = {};
    CHECK(m.OnPacketReady(0) == Action::Idle);
    CHECK(m.CanStart());
    CHECK_EQ(m.InFlightBytes(), 0u);
}

TEST(test_sm_packet_ready_sends_and_blocks_further_starts)
{
    SM m = {};
    CHECK(m.OnPacketReady(39) == Action::Send);
    CHECK_EQ(m.InFlightBytes(), 39u);
    CHECK(!m.CanStart());                          // one packet in flight at a time
    CHECK(m.OnSendResult(Result::Ok) == Action::Idle);
    CHECK_EQ(m.InFlightBytes(), 39u);
    CHECK(!m.CanStart());
}

TEST(test_sm_successful_completion_counts_and_pumps)
{
    SM m = InFlight(7);
    CHECK(m.OnComplete(Result::Ok) == Action::Pump);
    CHECK_EQ(m.PacketsSent(), 1ull);
    CHECK_EQ(m.BytesSent(), 7ull);
    CHECK_EQ(m.PacketsLost(), 0ull);
    CHECK_EQ(m.InFlightBytes(), 0u);
    CHECK(m.CanStart());

    CHECK(m.OnPacketReady(39) == Action::Send);
    CHECK(m.OnSendResult(Result::Ok) == Action::Idle);
    CHECK(m.OnComplete(Result::Ok) == Action::Pump);
    CHECK_EQ(m.PacketsSent(), 2ull);
    CHECK_EQ(m.BytesSent(), 46ull);
}

// A transfer error other than STALL (timeout after kMidiOutTimeoutMs,
// aborted outside Stop, device error): the packet is dropped without being
// counted sent or lost, no backoff, and the pipe is pumped again.
TEST(test_sm_completion_error_or_timeout_drops_and_pumps)
{
    SM m = InFlight(5);
    CHECK(m.OnComplete(Result::Error) == Action::Pump);
    CHECK_EQ(m.PacketsSent(), 0ull);
    CHECK_EQ(m.BytesSent(), 0ull);
    CHECK_EQ(m.PacketsLost(), 0ull);
    CHECK_EQ(m.BackoffTicks(), 0u);
    CHECK(m.CanStart());
    CHECK(!m.TakePauseLog());
}

// AsyncIO itself failing (not a STALL): nothing is in flight any more, but
// the driver does not pump again from this path; the next feedback tick does.
TEST(test_sm_submit_error_drops_packet_without_backoff)
{
    SM m = {};
    CHECK(m.OnPacketReady(3) == Action::Send);
    CHECK(m.OnSendResult(Result::Error) == Action::Idle);
    CHECK_EQ(m.InFlightBytes(), 0u);
    CHECK_EQ(m.PacketsLost(), 0ull);
    CHECK_EQ(m.BackoffTicks(), 0u);
    CHECK(m.CanStart());
    CHECK(!m.TakePauseLog());
}

// AsyncIO reporting a STALL synchronously: clear it, drop the packet as
// lost and back off straight away (no resend from this path).
TEST(test_sm_submit_stall_clears_drops_and_backs_off)
{
    SM m = {};
    CHECK(m.OnPacketReady(3) == Action::Send);
    CHECK(m.OnSendResult(Result::Stalled) == Action::ClearStallThenDrop);
    CHECK_EQ(m.InFlightBytes(), 0u);
    CHECK_EQ(m.PacketsLost(), 1ull);
    CHECK_EQ(m.BackoffTicks(), kMidiOutStallBackoffTicks);
    CHECK(!m.CanStart());
    CHECK(m.TakePauseLog());
    CHECK(!m.TakePauseLog());                    // taken once
}

TEST(test_sm_stall_on_completion_clears_then_resends_same_packet)
{
    SM m = InFlight(9);
    CHECK(StallComplete(m, true) == Action::Send);
    CHECK_EQ(m.Retries(), 1u);
    CHECK_EQ(m.InFlightBytes(), 9u);             // same packet, still in flight
    CHECK(!m.CanStart());
    CHECK(m.OnSendResult(Result::Ok) == Action::Idle);
    CHECK(m.OnComplete(Result::Ok) == Action::Pump);
    CHECK_EQ(m.PacketsSent(), 1ull);
    CHECK_EQ(m.BytesSent(), 9ull);
    CHECK_EQ(m.PacketsLost(), 0ull);
}

TEST(test_sm_stall_retries_exhausted_drops_and_backs_off)
{
    SM m = InFlight();
    for (uint32_t k = 1; k <= kMidiOutStallRetries; k++) {
        CHECK(StallComplete(m, true) == Action::Send);
        CHECK_EQ(m.Retries(), k);
        CHECK(m.OnSendResult(Result::Ok) == Action::Idle);
    }
    CHECK(StallComplete(m, true) == Action::StartBackoff);   // 4th STALL: give up
    CHECK_EQ(m.PacketsLost(), 1ull);
    CHECK_EQ(m.PacketsSent(), 0ull);
    CHECK_EQ(m.InFlightBytes(), 0u);
    CHECK_EQ(m.BackoffTicks(), kMidiOutStallBackoffTicks);
    CHECK(!m.CanStart());
    CHECK(m.TakePauseLog());
}

TEST(test_sm_retries_reset_for_each_new_packet)
{
    SM m = InFlight();
    CHECK(StallComplete(m, true) == Action::Send);
    CHECK(StallComplete(m, true) == Action::Send);
    CHECK_EQ(m.Retries(), 2u);
    CHECK(m.OnComplete(Result::Ok) == Action::Pump);
    CHECK(m.OnPacketReady(4) == Action::Send);
    CHECK_EQ(m.Retries(), 0u);
    for (uint32_t k = 0; k < kMidiOutStallRetries; k++)
        CHECK(StallComplete(m, true) == Action::Send);
}

// ClearStall failing: the pipe can't be trusted; give up on the packet
// even though retries remain.
TEST(test_sm_clear_stall_failure_gives_up_immediately)
{
    SM m = InFlight();
    CHECK(StallComplete(m, false) == Action::StartBackoff);
    CHECK_EQ(m.Retries(), 0u);
    CHECK_EQ(m.PacketsLost(), 1ull);
    CHECK_EQ(m.BackoffTicks(), kMidiOutStallBackoffTicks);
    CHECK(m.TakePauseLog());
}

// A resend that AsyncIO rejects with a STALL goes down the synchronous path:
// the packet is lost and MIDI out backs off without using the other retries.
TEST(test_sm_sync_stall_on_resend_drops_and_backs_off)
{
    SM m = InFlight();
    CHECK(StallComplete(m, true) == Action::Send);
    CHECK(m.OnSendResult(Result::Stalled) == Action::ClearStallThenDrop);
    CHECK_EQ(m.PacketsLost(), 1ull);
    CHECK_EQ(m.InFlightBytes(), 0u);
    CHECK_EQ(m.BackoffTicks(), kMidiOutStallBackoffTicks);
    CHECK(m.TakePauseLog());
}

TEST(test_sm_submit_error_on_resend_drops_without_backoff)
{
    SM m = InFlight();
    CHECK(StallComplete(m, true) == Action::Send);
    CHECK(m.OnSendResult(Result::Error) == Action::Idle);
    CHECK_EQ(m.InFlightBytes(), 0u);
    CHECK_EQ(m.PacketsLost(), 0ull);
    CHECK_EQ(m.BackoffTicks(), 0u);
    CHECK(m.CanStart());
}

TEST(test_sm_backoff_counts_down_one_per_tick)
{
    SM m = InFlight();
    CHECK(StallComplete(m, false) == Action::StartBackoff);
    for (uint32_t k = 0; k < kMidiOutStallBackoffTicks - 1; k++) {
        m.OnTick();
        CHECK(!m.CanStart());
    }
    CHECK_EQ(m.BackoffTicks(), 1u);
    m.OnTick();
    CHECK_EQ(m.BackoffTicks(), 0u);
    CHECK(m.CanStart());
    m.OnTick();                                   // stays at zero
    CHECK_EQ(m.BackoffTicks(), 0u);
    CHECK(m.OnPacketReady(3) == Action::Send);
}

TEST(test_sm_tick_while_idle_or_in_flight_changes_nothing)
{
    SM m = {};
    m.OnTick();
    CHECK(m.CanStart());
    m = InFlight(6);
    m.OnTick();
    CHECK_EQ(m.InFlightBytes(), 6u);
    CHECK(!m.CanStart());
}

// Waits out a backoff.
static void Ticks(SM & m, uint32_t n) { for (uint32_t k = 0; k < n; k++) m.OnTick(); }

// The "MIDI out paused" line is logged at most once per stats period.
TEST(test_sm_pause_logged_once_per_stats_period)
{
    SM m = InFlight();
    CHECK(StallComplete(m, false) == Action::StartBackoff);
    CHECK(m.TakePauseLog());

    Ticks(m, kMidiOutStallBackoffTicks);
    CHECK(m.OnPacketReady(3) == Action::Send);
    CHECK(m.OnSendResult(Result::Stalled) == Action::ClearStallThenDrop);
    CHECK_EQ(m.PacketsLost(), 2ull);
    CHECK(!m.TakePauseLog());                    // same stats period: not again

    m.OnStatsLogged();                           // a new period
    Ticks(m, kMidiOutStallBackoffTicks);
    CHECK(m.OnPacketReady(3) == Action::Send);
    CHECK(m.OnSendResult(Result::Ok) == Action::Idle);
    CHECK(StallComplete(m, false) == Action::StartBackoff);
    CHECK_EQ(m.PacketsLost(), 3ull);
    CHECK(m.TakePauseLog());
}

TEST(test_sm_stats_logged_without_pause_changes_nothing)
{
    SM m = InFlight(4);
    m.OnStatsLogged();
    CHECK(!m.TakePauseLog());
    CHECK_EQ(m.InFlightBytes(), 4u);
}

// Once stopping, completions (typically kIOReturnAborted from Stop's Abort)
// change nothing and ask for nothing, and no new packet may start.
TEST(test_sm_abort_while_stopping_is_ignored)
{
    SM m = InFlight(8);
    m.OnStop();
    CHECK(!m.CanStart());
    CHECK(m.OnComplete(Result::Error) == Action::Idle);
    CHECK(m.OnComplete(Result::Stalled) == Action::Idle);
    CHECK(m.OnComplete(Result::Ok) == Action::Idle);
    CHECK_EQ(m.InFlightBytes(), 8u);
    CHECK_EQ(m.PacketsSent(), 0ull);
    CHECK_EQ(m.PacketsLost(), 0ull);
    CHECK_EQ(m.BackoffTicks(), 0u);
    m.OnTick();
    CHECK(!m.CanStart());
}

TEST(test_sm_stopping_blocks_start_even_when_idle)
{
    SM m = {};
    m.OnStop();
    CHECK(!m.CanStart());
}

// End to end, as the driver runs it: a pipe that stalls every packet loses
// one packet per backoff period and never sends more often than that.
TEST(test_sm_permanently_stalling_pipe_is_rate_limited)
{
    SM m = {};
    uint32_t sends = 0;
    const uint32_t ticks = 10 * kMidiOutStallBackoffTicks;
    for (uint32_t t = 0; t < ticks; t++) {
        m.OnTick();
        if (!m.CanStart()) continue;
        Action a = m.OnPacketReady(3);
        while (a == Action::Send) {
            sends++;
            if (m.OnSendResult(Result::Ok) != Action::Idle) break;
            a = StallComplete(m, true);
        }
        CHECK(a == Action::StartBackoff);
    }
    // Each period: 1 send + 3 resends, then 250 ticks of silence.
    CHECK_EQ(m.PacketsLost(), uint64_t(ticks / kMidiOutStallBackoffTicks));
    CHECK_EQ(sends, uint32_t(m.PacketsLost() * (1 + kMidiOutStallRetries)));
    CHECK_EQ(m.PacketsSent(), 0ull);
}
