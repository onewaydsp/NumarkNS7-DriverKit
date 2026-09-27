// inventory.h
// Helpers for docs/controls/ns7-controls.json (schema "ns7-controls/1") and
// the learned / ledmap files built from it. Pure code.

#pragma once

#include "json.h"
#include "midi_msg.h"

#include <string>
#include <vector>

// Does this control produce MIDI a human can capture? False for LED-only
// items and for switches marked "midi_expected": false.
bool ControlHasInput(const Json & control);

// Does this control have a light (button+led, led, or any expected_led)?
bool ControlHasLed(const Json & control);

// "press and release" etc.: the control's own instruction, else one by kind.
std::string ControlInstruction(const Json & control);

// "90 11 (mixxx)" for each expected_input / expected_led entry.
std::string DescribeExpected(const Json & entries);

// An LED message from an expected_led entry. Only B0/90-type statuses with
// 7-bit data are accepted.
struct LedMessage {
    uint8_t status = 0;
    uint8_t data1 = 0;
    uint8_t on = 0x7F;
    uint8_t off = 0x00;
};
bool ParseLedEntry(const Json & entry, LedMessage * out);
bool IsLedStatus(uint8_t status);   // 0xB0..0xBF or 0x90..0x9F

// Finds a control by id in an inventory-shaped document, or nullptr.
const Json * FindControl(const Json & doc, const std::string & id);
Json * FindControl(Json & doc, const std::string & id);
