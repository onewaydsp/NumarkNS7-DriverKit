// inventory.cpp

#include "inventory.h"

bool ControlHasInput(const Json & control)
{
    if (!control.BoolAt("midi_expected", true)) return false;
    return control.StringAt("kind") != "led";
}

bool ControlHasLed(const Json & control)
{
    const std::string kind = control.StringAt("kind");
    return kind == "button+led" || kind == "led" || control.At("expected_led").size() > 0;
}

std::string ControlInstruction(const Json & control)
{
    const std::string own = control.StringAt("instruction");
    if (!own.empty()) return own;
    const std::string kind = control.StringAt("kind");
    if (kind == "button" || kind == "button+led") return "press and release";
    if (kind == "fader") return "move from min to max";
    if (kind == "knob") return "turn fully left, then fully right";
    if (kind == "encoder") return "turn clockwise, then counter-clockwise";
    if (kind == "platter") return "spin forward, then backward";
    if (kind == "touch") return "touch and release";
    if (kind == "switch") return "flip through every position";
    if (kind == "led") return "observe only";
    return "operate the control";
}

std::string DescribeExpected(const Json & entries)
{
    std::string out;
    for (const Json & e : entries.items()) {
        if (!out.empty()) out += ", ";
        out += e.StringAt("status", "??");
        if (e.At("data1").IsString()) out += " " + e.StringAt("data1");
        const std::string source = e.StringAt("source");
        const bool verified = e.BoolAt("verified");
        if (!source.empty() || verified)
            out += " (" + source + (verified ? ", verified" : "") + ")";
    }
    return out.empty() ? "none known" : out;
}

bool IsLedStatus(uint8_t status)
{
    return (status & 0xF0) == 0xB0 || (status & 0xF0) == 0x90;
}

bool ParseLedEntry(const Json & entry, LedMessage * out)
{
    LedMessage led;
    if (!ParseHexByte(entry.StringAt("status"), &led.status) || !IsLedStatus(led.status)) return false;
    if (!ParseHexByte(entry.StringAt("data1"), &led.data1) || led.data1 > 0x7F) return false;
    if (entry.At("on").IsString() && (!ParseHexByte(entry.StringAt("on"), &led.on) || led.on > 0x7F)) return false;
    if (entry.At("off").IsString() && (!ParseHexByte(entry.StringAt("off"), &led.off) || led.off > 0x7F)) return false;
    *out = led;
    return true;
}

const Json * FindControl(const Json & doc, const std::string & id)
{
    for (const Json & c : doc.At("controls").items())
        if (c.StringAt("id") == id) return &c;
    return nullptr;
}

Json * FindControl(Json & doc, const std::string & id)
{
    Json * controls = doc.Get("controls");
    if (!controls) return nullptr;
    for (Json & c : controls->items())
        if (c.StringAt("id") == id) return &c;
    return nullptr;
}
