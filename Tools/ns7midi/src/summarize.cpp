// summarize.cpp

#include "summarize.h"

#include <algorithm>
#include <map>

const char * GroupClassName(GroupClass c)
{
    switch (c) {
    case GroupClass::NotePair: return "note_pair";
    case GroupClass::NoteOnOnly: return "note_on_only";
    case GroupClass::NoteOffOnly: return "note_off_only";
    case GroupClass::CcButton: return "cc_button";
    case GroupClass::CcSingle: return "cc_single_value";
    case GroupClass::CcAbsolute: return "cc_absolute";
    case GroupClass::CcRelative2c: return "cc_relative_2c";
    case GroupClass::CcRelativeOffset: return "cc_relative_offset64";
    case GroupClass::PitchBend: return "pitchbend_14bit";
    case GroupClass::Other: return "other";
    }
    return "other";
}

namespace {

struct Work {
    MsgGroup group;
    std::vector<int> values;
    bool pressed = false;
};

int ValueOf(const MidiMsg & msg)
{
    if (msg.Type() == 0xE0) return msg.data1 | (msg.data2 << 7);
    if (msg.Type() == 0xC0 || msg.Type() == 0xD0) return msg.data1;
    return msg.data2;
}

void CountSteps(const std::vector<int> & values, MsgGroup * g)
{
    for (size_t i = 1; i < values.size(); i++) {
        if (values[i] > values[i - 1]) g->ups++;
        else if (values[i] < values[i - 1]) g->downs++;
    }
}

std::string Direction(const MsgGroup & g)
{
    if (g.ups && g.downs) return "rose and fell";
    if (g.ups) return "rising";
    if (g.downs) return "falling";
    return "static";
}

// Relative encoders send small step sizes around 0 (two's complement) or
// around 0x40 (offset binary). A fader swept over its range never fits.
bool LooksRelative2c(const std::vector<int> & values, const std::set<int> & distinct)
{
    if (values.size() < 3 || distinct.size() > 12) return false;
    for (int v : distinct)
        if (!((v >= 0x01 && v <= 0x10) || (v >= 0x70 && v <= 0x7F))) return false;
    return true;
}

bool LooksRelativeOffset(const std::vector<int> & values, const std::set<int> & distinct)
{
    if (values.size() < 3 || distinct.size() > 12) return false;
    for (int v : distinct)
        if (v < 0x30 || v > 0x50 || v == 0x40) return false;
    return true;
}

void Classify(Work * w)
{
    MsgGroup & g = w->group;
    const std::set<int> distinct(w->values.begin(), w->values.end());
    g.distinct = distinct.size();
    g.minValue = distinct.empty() ? 0 : *distinct.begin();
    g.maxValue = distinct.empty() ? 0 : *distinct.rbegin();
    const uint8_t type = g.status & 0xF0;
    char buf[160];

    if (type == 0x90) {
        if (g.pairs > 0) g.cls = GroupClass::NotePair;
        else if (g.presses > 0) g.cls = GroupClass::NoteOnOnly;
        else g.cls = GroupClass::NoteOffOnly;
        std::snprintf(buf, sizeof buf, "note %s: %zu press(es), %zu release(s), %zu pair(s)",
                      g.key.c_str(), g.presses, g.releases, g.pairs);
        g.detail = buf;
        return;
    }
    if (type == 0xE0) {
        g.cls = GroupClass::PitchBend;
        CountSteps(w->values, &g);
        std::snprintf(buf, sizeof buf, "pitch bend %s: 14-bit %04X..%04X, %s (%zu up, %zu down), %zu msgs",
                      g.key.c_str(), g.minValue, g.maxValue, Direction(g).c_str(), g.ups, g.downs, g.count);
        g.detail = buf;
        return;
    }
    if (type != 0xB0) {
        g.cls = GroupClass::Other;
        std::snprintf(buf, sizeof buf, "%s: values %02X..%02X, %zu msgs", g.key.c_str(), g.minValue, g.maxValue, g.count);
        g.detail = buf;
        return;
    }

    // Control change.
    if (distinct.size() == 2 && g.minValue == 0) {
        g.cls = GroupClass::CcButton;
        size_t presses = 0;
        for (size_t i = 0; i < w->values.size(); i++)
            if (w->values[i] != 0 && (i == 0 || w->values[i - 1] == 0)) presses++;
        g.presses = presses;
        std::snprintf(buf, sizeof buf, "CC %s button: on %02X / off 00, %zu press(es)",
                      g.key.c_str(), g.maxValue, presses);
    } else if (distinct.size() == 1 && w->values.size() < 3) {
        g.cls = GroupClass::CcSingle;
        std::snprintf(buf, sizeof buf, "CC %s single value %02X x%zu (button without release?)",
                      g.key.c_str(), g.minValue, g.count);
    } else if (LooksRelative2c(w->values, distinct)) {
        g.cls = GroupClass::CcRelative2c;
        for (int v : w->values) (v < 0x40 ? g.ups : g.downs)++;
        std::snprintf(buf, sizeof buf, "CC %s relative encoder (two's complement): %zu tick(s) +, %zu tick(s) -",
                      g.key.c_str(), g.ups, g.downs);
    } else if (LooksRelativeOffset(w->values, distinct)) {
        g.cls = GroupClass::CcRelativeOffset;
        for (int v : w->values) (v > 0x40 ? g.ups : g.downs)++;
        std::snprintf(buf, sizeof buf, "CC %s relative encoder (offset 64): %zu tick(s) +, %zu tick(s) -",
                      g.key.c_str(), g.ups, g.downs);
    } else {
        g.cls = GroupClass::CcAbsolute;
        CountSteps(w->values, &g);
        const bool full = g.minValue <= 0x01 && g.maxValue >= 0x7E;
        std::snprintf(buf, sizeof buf, "CC %s absolute %02X..%02X%s, %s, %zu distinct, %zu msgs",
                      g.key.c_str(), g.minValue, g.maxValue, full ? " (full range)" : "",
                      Direction(g).c_str(), g.distinct, g.count);
    }
    g.detail = buf;
}

}   // namespace

Summary Summarize(const std::vector<MidiMsg> & msgs, const std::set<std::string> & ignore)
{
    Summary summary;
    std::vector<Work> work;
    std::map<std::string, size_t> index;

    for (const MidiMsg & msg : msgs) {
        summary.total++;
        const std::string key = MsgKey(msg);
        if (ignore.count(key)) { summary.ignored++; continue; }
        auto found = index.find(key);
        if (found == index.end()) {
            Work w;
            w.group.key = key;
            ParseKey(key, &w.group.status, &w.group.data1);
            found = index.emplace(key, work.size()).first;
            work.push_back(w);
        }
        Work & w = work[found->second];
        w.group.count++;
        if ((w.group.status & 0xF0) == 0x90) {
            const bool release = msg.Type() == 0x80 || msg.data2 == 0;
            if (release) {
                w.group.releases++;
                if (w.pressed) { w.group.pairs++; w.pressed = false; }
            } else {
                w.group.presses++;
                w.pressed = true;
            }
        }
        w.values.push_back(ValueOf(msg));
    }

    for (Work & w : work) Classify(&w);

    // 14-bit CC pairs: MSB on CC n (0..31) with LSB on CC n+32, same channel.
    for (Work & msb : work) {
        const MsgGroup & g = msb.group;
        if ((g.status & 0xF0) != 0xB0 || g.data1 < 0 || g.data1 >= 32) continue;
        const std::string lsbKey = MakeKey(g.status, g.data1 + 32);
        auto lsb = index.find(lsbKey);
        if (lsb == index.end()) continue;
        msb.group.pairedWith = lsbKey;
        msb.group.isMsb = true;
        msb.group.detail += "; 14-bit MSB, LSB on " + lsbKey;
        work[lsb->second].group.pairedWith = g.key;
        work[lsb->second].group.detail += "; 14-bit LSB of " + g.key;
    }

    for (Work & w : work) summary.groups.push_back(w.group);
    return summary;
}

std::vector<std::string> DescribeSummary(const Summary & summary)
{
    std::vector<std::string> lines;
    for (const MsgGroup & g : summary.groups) lines.push_back(g.detail);
    if (summary.ignored)
        lines.push_back(std::to_string(summary.ignored) + " background message(s) ignored");
    return lines;
}

Json SummaryToJson(const Summary & summary)
{
    Json groups = Json::Array();
    for (const MsgGroup & g : summary.groups) {
        Json j = Json::Object();
        j.Set("key", g.key);
        j.Set("status", Hex2(g.status));
        j.Set("data1", g.data1 < 0 ? Json() : Json(Hex2(unsigned(g.data1))));
        j.Set("class", GroupClassName(g.cls));
        j.Set("count", g.count);
        j.Set("min", g.minValue);
        j.Set("max", g.maxValue);
        j.Set("distinct", g.distinct);
        if (g.presses || g.releases) {
            j.Set("presses", g.presses);
            j.Set("releases", g.releases);
        }
        if (g.ups || g.downs) {
            j.Set("ups", g.ups);
            j.Set("downs", g.downs);
        }
        if (!g.pairedWith.empty()) {
            j.Set("paired_with", g.pairedWith);
            j.Set("pair_role", g.isMsb ? "msb" : "lsb");
        }
        j.Set("detail", g.detail);
        groups.Push(std::move(j));
    }
    return groups;
}

Json SampleToJson(const std::vector<MidiMsg> & msgs, size_t limit, const std::set<std::string> & ignore)
{
    Json sample = Json::Array();
    for (const MidiMsg & msg : msgs) {
        if (sample.size() >= limit) break;
        if (ignore.count(MsgKey(msg))) continue;
        sample.Push(FormatMsg(msg));
    }
    return sample;
}
