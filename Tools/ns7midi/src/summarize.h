// summarize.h
// Turns the MIDI captured during one learn step into a short description per
// message group: note on/off pairs, CC buttons, absolute ranges, relative
// encoders, 14-bit pairs and pitch bend. Pure code, tested in tests/.

#pragma once

#include "json.h"
#include "midi_msg.h"

#include <set>
#include <string>
#include <vector>

enum class GroupClass {
    NotePair,        // note on (vel > 0) followed by note off or note on vel 0
    NoteOnOnly,      // note on without a release
    NoteOffOnly,
    CcButton,        // CC with only 00 and one "on" value
    CcSingle,        // CC with one value only (a button that never releases?)
    CcAbsolute,      // CC sweeping a range
    CcRelative2c,    // relative encoder: 01..3F up, 41..7F down (two's complement)
    CcRelativeOffset,// relative encoder: 41.. up, ..3F down (offset 64)
    PitchBend,       // 14-bit value
    Other,           // aftertouch, program change, pressure
};

const char * GroupClassName(GroupClass c);

struct MsgGroup {
    std::string key;          // MsgKey()
    uint8_t status = 0;       // canonical (note off folded into note on)
    int data1 = -1;           // -1 for pitch bend / pressure
    GroupClass cls = GroupClass::Other;
    size_t count = 0;
    int minValue = 0;         // data2, or the 14-bit value for pitch bend
    int maxValue = 0;
    size_t distinct = 0;
    size_t presses = 0;       // note on vel > 0
    size_t releases = 0;      // note off or vel 0
    size_t pairs = 0;         // completed press→release pairs
    size_t ups = 0;           // absolute: rising steps; relative: positive ticks
    size_t downs = 0;
    std::string pairedWith;   // 14-bit partner key ("B0:21" for MSB B0:01)
    bool isMsb = false;       // the partner is the LSB
    std::string detail;       // one-line human description
};

struct Summary {
    std::vector<MsgGroup> groups;   // ordered by first appearance
    size_t total = 0;
    size_t ignored = 0;             // messages dropped by the ignore set
};

// Summarizes messages; keys in `ignore` (e.g. background streams) are counted
// but left out of the groups.
Summary Summarize(const std::vector<MidiMsg> & msgs, const std::set<std::string> & ignore = {});

// One printable line per group.
std::vector<std::string> DescribeSummary(const Summary & summary);

// JSON form stored in learned.json: an array of group objects.
Json SummaryToJson(const Summary & summary);

// The first `limit` messages as "90 11 7F" strings.
Json SampleToJson(const std::vector<MidiMsg> & msgs, size_t limit, const std::set<std::string> & ignore = {});
