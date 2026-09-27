// midi_io.h
// Where MIDI comes from and goes to. The commands talk to the MidiInput and
// MidiOutput interfaces, so learn and ledscan run the same code against the
// real NS7 (CoreMIDI), a replay file, or a dry run that sends nothing.

#pragma once

#include "midi_msg.h"

#include <memory>
#include <string>
#include <vector>

extern const char * const kDeviceName;   // "Numark USB Audio Device"

class MidiInput {
public:
    virtual ~MidiInput() = default;
    // Starts a capture step: drops anything queued and restarts the clock.
    // Returns false when a replay has no blocks left.
    virtual bool BeginStep() = 0;
    // Messages that arrived since the last call.
    virtual std::vector<MidiMsg> Drain() = 0;
    // True when this step cannot receive more (a replay block fully delivered).
    virtual bool StepComplete() const = 0;
    virtual bool IsReplay() const = 0;
};

class MidiOutput {
public:
    virtual ~MidiOutput() = default;
    // Sends one message and prints exactly what was (or would be) sent.
    virtual bool Send(const MidiMsg & msg) = 0;
    virtual bool IsDryRun() const = 0;
};

// A replay file (see ParseReplay); each BeginStep consumes one block.
std::unique_ptr<MidiInput> OpenReplayInput(const std::string & path, std::string * error);

// Prints "[dry-run] B0 09 7F (not sent)" and never touches CoreMIDI.
std::unique_ptr<MidiOutput> MakeDryRunOutput();

// One CoreMIDI client for the whole session: MIDIServer launches per client
// and stops the driver IO when the last one leaves, so the session keeps
// its client (and an input connection) alive until it is destroyed.
class CoreMidiSession {
public:
    ~CoreMidiSession();
    // wantInput connects the NS7 source; wantOutput finds its destination.
    bool Open(bool wantInput, bool wantOutput, std::string * error);
    MidiInput * input() { return input_.get(); }
    MidiOutput * output() { return output_.get(); }

private:
    unsigned client_ = 0;
    std::unique_ptr<MidiInput> input_;
    std::unique_ptr<MidiOutput> output_;
};

// The original ns7midi commands.
int ListDevices();
int MonitorCommand(double seconds, const std::string & recordPath);
int SendCommand(const std::vector<std::string> & hexTokens);

// Sleeps; used for LED holds and gaps.
void SleepMs(int ms);
