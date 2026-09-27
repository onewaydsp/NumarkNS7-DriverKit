// midi_msg.cpp

#include "midi_msg.h"

#include <cstdio>
#include <cstdlib>
#include <sstream>

int ChannelVoiceDataBytes(uint8_t status)
{
    if (status < 0x80 || status >= 0xF0) return -1;
    const uint8_t type = status & 0xF0;
    return (type == 0xC0 || type == 0xD0) ? 1 : 2;
}

std::string Hex2(unsigned value)
{
    char buf[8];
    std::snprintf(buf, sizeof buf, "%02X", value & 0xFF);
    return buf;
}

std::string FormatMsg(const MidiMsg & msg)
{
    std::string s = Hex2(msg.status) + " " + Hex2(msg.data1);
    if (msg.DataBytes() == 2) s += " " + Hex2(msg.data2);
    return s;
}

std::string MakeKey(uint8_t status, int data1)
{
    return data1 < 0 ? Hex2(status) : Hex2(status) + ":" + Hex2(unsigned(data1));
}

std::string MsgKey(const MidiMsg & msg)
{
    uint8_t status = msg.status;
    if (msg.Type() == 0x80) status = uint8_t(0x90 | msg.Channel());
    return MakeKey(status, msg.HasData1() ? msg.data1 : -1);
}

bool ParseKey(const std::string & key, uint8_t * status, int * data1)
{
    const size_t colon = key.find(':');
    uint8_t s = 0, d = 0;
    if (!ParseHexByte(key.substr(0, colon), &s) || ChannelVoiceDataBytes(s) < 0) return false;
    *status = s;
    if (colon == std::string::npos) { *data1 = -1; return true; }
    if (!ParseHexByte(key.substr(colon + 1), &d) || d > 0x7F) return false;
    *data1 = d;
    return true;
}

bool ParseHexByte(const std::string & token, uint8_t * out)
{
    std::string t = token;
    if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) t = t.substr(2);
    if (t.empty() || t.size() > 2) return false;
    unsigned value = 0;
    for (char c : t) {
        value <<= 4;
        if (c >= '0' && c <= '9') value |= unsigned(c - '0');
        else if (c >= 'a' && c <= 'f') value |= unsigned(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') value |= unsigned(c - 'A' + 10);
        else return false;
    }
    *out = uint8_t(value);
    return true;
}

bool ParseChannelVoiceBytes(const std::vector<std::string> & tokens, std::vector<MidiMsg> * out)
{
    std::vector<uint8_t> bytes;
    for (const std::string & token : tokens) {
        uint8_t byte = 0;
        if (!ParseHexByte(token, &byte)) return false;
        bytes.push_back(byte);
    }
    std::vector<MidiMsg> parsed;
    for (size_t i = 0; i < bytes.size(); ) {
        const int data = ChannelVoiceDataBytes(bytes[i]);
        if (data < 0 || i + 1 + size_t(data) > bytes.size()) return false;
        MidiMsg msg;
        msg.status = bytes[i];
        msg.data1 = bytes[i + 1];
        msg.data2 = data == 2 ? bytes[i + 2] : 0;
        if (msg.data1 > 0x7F || msg.data2 > 0x7F) return false;
        parsed.push_back(msg);
        i += 1 + size_t(data);
    }
    if (parsed.empty()) return false;
    out->insert(out->end(), parsed.begin(), parsed.end());
    return true;
}

bool ParseReplay(const std::string & text, std::vector<std::vector<MidiMsg>> * blocks, std::string * error)
{
    std::istringstream in(text);
    std::string line;
    int lineNo = 0;
    std::vector<MidiMsg> current;
    bool open = false;
    while (std::getline(in, line)) {
        lineNo++;
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        std::istringstream words(line);
        std::vector<std::string> tokens;
        for (std::string w; words >> w; ) tokens.push_back(w);
        if (tokens.empty()) continue;
        if (tokens.size() == 1 && tokens[0] == "---") {
            blocks->push_back(current);
            current.clear();
            open = false;
            continue;
        }
        if (!ParseChannelVoiceBytes(tokens, &current)) {
            *error = "replay line " + std::to_string(lineNo) + ": expected channel voice hex bytes";
            return false;
        }
        open = true;
    }
    if (open) blocks->push_back(current);
    return true;
}
