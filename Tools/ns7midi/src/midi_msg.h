// midi_msg.h
// MIDI 1.0 channel voice messages as plain bytes, plus the strict hex parsing
// and formatting the ns7midi commands share. Nothing here touches CoreMIDI.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct MidiMsg {
    uint8_t status = 0;
    uint8_t data1 = 0;
    uint8_t data2 = 0;
    double time = 0;   // seconds since the capture started

    uint8_t Type() const { return status & 0xF0; }
    uint8_t Channel() const { return status & 0x0F; }
    bool HasData1() const { return Type() != 0xE0 && Type() != 0xD0; }   // pitch bend / pressure
    int DataBytes() const { return (Type() == 0xC0 || Type() == 0xD0) ? 1 : 2; }
};

// Number of data bytes for a channel voice status byte, or -1 if the byte is
// not a channel voice status.
int ChannelVoiceDataBytes(uint8_t status);

// "90 11 7F", "E0 12 40", "C0 05".
std::string FormatMsg(const MidiMsg & msg);

// The grouping key used everywhere: note on and note off share the note-on
// status ("90:11"), CCs are "B0:09", pitch bend and channel pressure have no
// data1 ("E0", "D0"), program change is "C0:05" style.
std::string MsgKey(const MidiMsg & msg);

// Splits a key back into its status and data1 (-1 when the key has none).
bool ParseKey(const std::string & key, uint8_t * status, int * data1);
std::string MakeKey(uint8_t status, int data1);

// Two uppercase hex digits.
std::string Hex2(unsigned value);

// Strict hex byte: one or two hex digits, optional 0x prefix, no sign, no
// trailing garbage, no value over 0xFF.
bool ParseHexByte(const std::string & token, uint8_t * out);

// Parses hex bytes into complete channel voice messages. Returns false for a
// data byte where a status is expected, system messages, running status or a
// truncated message.
bool ParseChannelVoiceBytes(const std::vector<std::string> & tokens, std::vector<MidiMsg> * out);

// Replay files feed recorded MIDI to learn instead of CoreMIDI. Format:
//   # comment
//   90 11 7F          one message per line (several are allowed per line)
//   ---               ends the block for one capture step
// Returns one vector per block. Errors name the line.
bool ParseReplay(const std::string & text, std::vector<std::vector<MidiMsg>> * blocks, std::string * error);
