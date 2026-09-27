// test_main.cpp
// Unit tests for the pure ns7midi modules: JSON, message parsing, the step
// summarizer and the verify coverage logic. No CoreMIDI.

#include "inventory.h"
#include "json.h"
#include "midi_msg.h"
#include "summarize.h"
#include "verify_core.h"

#include <cstdio>
#include <string>
#include <vector>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failures++; \
    std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { g_checks++; if (!((a) == (b))) { g_failures++; \
    std::printf("FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); } } while (0)

static std::vector<MidiMsg> Msgs(const std::string & hex)
{
    std::vector<std::string> tokens;
    std::string t;
    for (char c : hex + " ") {
        if (c == ' ') { if (!t.empty()) tokens.push_back(t); t.clear(); }
        else t.push_back(c);
    }
    std::vector<MidiMsg> out;
    if (!ParseChannelVoiceBytes(tokens, &out)) std::printf("bad test hex: %s\n", hex.c_str());
    return out;
}

static const MsgGroup * FindGroup(const Summary & s, const std::string & key)
{
    for (const MsgGroup & g : s.groups) if (g.key == key) return &g;
    return nullptr;
}

// ------------------------------------------------------------------- JSON

static void TestJsonRoundTrip()
{
    const std::string text = R"({
  "a": 1,
  "b": [true, false, null, -2.5, 1e3],
  "c": {"nested": "q\"uote \\ tab\t nl\n \u00e9 \ud83c\udfb5"},
  "d": [],
  "e": {},
  "f": [{"x": 1}, {"y": [1, 2]}]
})";
    Json j;
    std::string error;
    CHECK(Json::Parse(text, &j, &error));
    CHECK_EQ(j.NumberAt("a"), 1.0);
    CHECK_EQ(j.At("b").size(), 5u);
    CHECK(j.At("b").items()[2].IsNull());
    CHECK_EQ(j.At("b").items()[3].AsNumber(), -2.5);
    CHECK_EQ(j.At("b").items()[4].AsNumber(), 1000.0);
    CHECK_EQ(j.At("c").StringAt("nested"), std::string("q\"uote \\ tab\t nl\n \xC3\xA9 \xF0\x9F\x8E\xB5"));

    // Dump then parse gives the same value, and keys keep their order.
    Json again;
    CHECK(Json::Parse(j.Dump(2), &again, &error));
    CHECK(again == j);
    CHECK(Json::Parse(j.Dump(0), &again, &error));
    CHECK(again == j);
    CHECK_EQ(j.members()[0].first, std::string("a"));
    CHECK_EQ(j.members()[5].first, std::string("f"));

    // Set replaces in place.
    j.Set("a", "changed");
    CHECK_EQ(j.members()[0].first, std::string("a"));
    CHECK_EQ(j.StringAt("a"), std::string("changed"));
    CHECK(j.Remove("d"));
    CHECK(j.Get("d") == nullptr);

    // Integers print without a fraction.
    Json n = Json::Object();
    n.Set("count", 42);
    n.Set("half", 0.5);
    CHECK_EQ(n.Dump(0), std::string(R"({"count":42,"half":0.5})"));

    // Errors carry a position.
    CHECK(!Json::Parse("{\"a\": }", &j, &error));
    CHECK(error.find("line 1") != std::string::npos);
    CHECK(!Json::Parse("[1, 2", &j, &error));
    CHECK(!Json::Parse("{} x", &j, &error));
    CHECK(!Json::Parse("\"\\ud800\"", &j, &error));
}

// ---------------------------------------------------------- MIDI parsing

static void TestMidiParsing()
{
    uint8_t b = 0;
    CHECK(ParseHexByte("7F", &b) && b == 0x7F);
    CHECK(ParseHexByte("0x0a", &b) && b == 0x0A);
    CHECK(ParseHexByte("9", &b) && b == 0x09);
    CHECK(!ParseHexByte("", &b));
    CHECK(!ParseHexByte("-1", &b));
    CHECK(!ParseHexByte("100", &b));
    CHECK(!ParseHexByte("7G", &b));

    std::vector<MidiMsg> m;
    CHECK(ParseChannelVoiceBytes({ "90", "11", "7F", "C0", "05", "E0", "00", "40" }, &m));
    CHECK_EQ(m.size(), 3u);
    CHECK_EQ(FormatMsg(m[1]), std::string("C0 05"));
    CHECK_EQ(MsgKey(m[2]), std::string("E0"));
    m.clear();
    CHECK(!ParseChannelVoiceBytes({ "11", "7F" }, &m));          // running status
    CHECK(!ParseChannelVoiceBytes({ "F0", "7E", "F7" }, &m));    // sysex
    CHECK(!ParseChannelVoiceBytes({ "90", "11" }, &m));          // truncated
    CHECK(!ParseChannelVoiceBytes({ "90", "80", "7F" }, &m));    // data byte >= 0x80

    // Note off shares the note-on key.
    CHECK_EQ(MsgKey(Msgs("80 11 00")[0]), std::string("90:11"));
    CHECK_EQ(MsgKey(Msgs("B0 09 7F")[0]), std::string("B0:09"));

    uint8_t status = 0;
    int data1 = 0;
    CHECK(ParseKey("B0:09", &status, &data1) && status == 0xB0 && data1 == 9);
    CHECK(ParseKey("E2", &status, &data1) && status == 0xE2 && data1 == -1);
    CHECK(!ParseKey("F0", &status, &data1));

    std::vector<std::vector<MidiMsg>> blocks;
    std::string error;
    CHECK(ParseReplay("# c\n90 11 7F\n90 11 00 # release\n---\n---\nB0 04 10 B0 24 00\n", &blocks, &error));
    CHECK_EQ(blocks.size(), 3u);
    CHECK_EQ(blocks[0].size(), 2u);
    CHECK_EQ(blocks[1].size(), 0u);
    CHECK_EQ(blocks[2].size(), 2u);
    CHECK(!ParseReplay("90 11\n", &blocks, &error));
    CHECK(error.find("line 1") != std::string::npos);
}

// -------------------------------------------------------------- summarizer

static void TestNotePairs()
{
    const Summary s = Summarize(Msgs("90 11 7F 90 11 00 90 11 7F 80 11 40"));
    CHECK_EQ(s.groups.size(), 1u);
    const MsgGroup & g = s.groups[0];
    CHECK(g.cls == GroupClass::NotePair);
    CHECK_EQ(g.presses, 2u);
    CHECK_EQ(g.releases, 2u);
    CHECK_EQ(g.pairs, 2u);

    const Summary onOnly = Summarize(Msgs("90 12 7F"));
    CHECK(onOnly.groups[0].cls == GroupClass::NoteOnOnly);
}

static void TestCcButtonAndRange()
{
    const Summary button = Summarize(Msgs("B0 30 7F B0 30 00 B0 30 7F B0 30 00"));
    CHECK(button.groups[0].cls == GroupClass::CcButton);
    CHECK_EQ(button.groups[0].presses, 2u);
    CHECK_EQ(button.groups[0].maxValue, 0x7F);

    // A fader swept min to max and back.
    std::string hex;
    for (int v = 0; v <= 127; v += 3) hex += "B0 08 " + Hex2(unsigned(v)) + " ";
    hex += "B0 08 7F B0 08 40 B0 08 00";
    const Summary fader = Summarize(Msgs(hex));
    const MsgGroup & g = fader.groups[0];
    CHECK(g.cls == GroupClass::CcAbsolute);
    CHECK_EQ(g.minValue, 0);
    CHECK_EQ(g.maxValue, 0x7F);
    CHECK(g.ups > 0 && g.downs > 0);
    CHECK(g.detail.find("full range") != std::string::npos);

    const Summary single = Summarize(Msgs("B0 31 7F"));
    CHECK(single.groups[0].cls == GroupClass::CcSingle);
}

static void TestRelativeEncoders()
{
    const Summary twos = Summarize(Msgs("B0 50 01 B0 50 01 B0 50 02 B0 50 7F B0 50 7F B0 50 7E"));
    CHECK(twos.groups[0].cls == GroupClass::CcRelative2c);
    CHECK_EQ(twos.groups[0].ups, 3u);
    CHECK_EQ(twos.groups[0].downs, 3u);

    const Summary offset = Summarize(Msgs("B0 51 41 B0 51 41 B0 51 42 B0 51 3F B0 51 3E"));
    CHECK(offset.groups[0].cls == GroupClass::CcRelativeOffset);
    CHECK_EQ(offset.groups[0].ups, 3u);
    CHECK_EQ(offset.groups[0].downs, 2u);

    // A slow knob turn over a wide absolute range is not relative.
    const Summary knob = Summarize(Msgs("B0 0C 10 B0 0C 20 B0 0C 30 B0 0C 50 B0 0C 70"));
    CHECK(knob.groups[0].cls == GroupClass::CcAbsolute);
}

static void Test14Bit()
{
    const Summary pair = Summarize(Msgs("B0 04 10 B0 24 05 B0 04 11 B0 24 70 B0 04 12 B0 24 00"));
    const MsgGroup * msb = FindGroup(pair, "B0:04");
    const MsgGroup * lsb = FindGroup(pair, "B0:24");
    CHECK(msb && lsb);
    if (msb && lsb) {
        CHECK(msb->isMsb);
        CHECK_EQ(msb->pairedWith, std::string("B0:24"));
        CHECK_EQ(lsb->pairedWith, std::string("B0:04"));
        CHECK(!lsb->isMsb);
    }

    const Summary bend = Summarize(Msgs("E0 00 40 E0 7F 40 E0 00 41 E0 00 3F"));
    CHECK(bend.groups[0].cls == GroupClass::PitchBend);
    CHECK_EQ(bend.groups[0].minValue, 0x3F << 7);
    CHECK_EQ(bend.groups[0].maxValue, 0x41 << 7);
    CHECK(bend.groups[0].ups > 0 && bend.groups[0].downs > 0);

    // Platter: CC stream plus pitch bend, both reported.
    const Summary platter = Summarize(Msgs("B0 00 01 E0 10 40 B0 00 02 E0 20 40 B0 00 7F E0 10 40"));
    CHECK_EQ(platter.groups.size(), 2u);
}

static void TestIgnore()
{
    const Summary s = Summarize(Msgs("B0 00 01 B0 00 02 90 11 7F 90 11 00"), { "B0:00" });
    CHECK_EQ(s.groups.size(), 1u);
    CHECK_EQ(s.ignored, 2u);
    CHECK_EQ(s.total, 4u);
    const Json j = SummaryToJson(s);
    CHECK_EQ(j.items()[0].StringAt("class"), std::string("note_pair"));
    CHECK_EQ(SampleToJson(Msgs("B0 00 01 90 11 7F"), 8, { "B0:00" }).size(), 1u);
}

// ------------------------------------------------------------------ verify

static Json ParseOrDie(const std::string & text)
{
    Json j;
    std::string error;
    if (!Json::Parse(text, &j, &error)) std::printf("bad test json: %s\n", error.c_str());
    return j;
}

static void TestVerifyCoverage()
{
    const Json inv = ParseOrDie(R"({
      "controls": [
        {"id": "deckA.play", "section": "deckA", "label": "Play A", "kind": "button+led",
         "expected_input": [{"status": "90", "data1": "11", "source": "mixxx"}],
         "expected_led": [{"status": "B0", "data1": "09", "source": "hw", "verified": true}]},
        {"id": "deckA.cue", "section": "deckA", "label": "Cue A", "kind": "button+led",
         "expected_input": [], "expected_led": [{"status": "B0", "data1": "08", "source": "mixxx"}]},
        {"id": "mixer.fader", "section": "mixer", "label": "Fader", "kind": "fader",
         "expected_input": [], "expected_led": []},
        {"id": "mixer.meter", "section": "mixer", "label": "Meter", "kind": "led",
         "expected_input": [], "expected_led": [{"status": "B0", "data1": "36"}]},
        {"id": "rear.power", "section": "rear", "label": "Power", "kind": "switch", "midi_expected": false}
      ],
      "unidentified_observed": [{"status": "B0", "data1": "24", "guess": "lsb"}]
    })");

    // Nothing learned: 3 inputs needed (power and the meter need none),
    // 3 LEDs needed of which the verified prior counts.
    CoverageReport r = BuildCoverage(inv, Json(), Json());
    CHECK_EQ(r.inputsNeeded, 3u);
    CHECK_EQ(r.inputsVerified, 0u);
    CHECK_EQ(r.ledsNeeded, 3u);
    CHECK_EQ(r.ledsVerified, 1u);
    CHECK_EQ(r.unassignedInputs.size(), 1u);   // the B0 24 prior observation
    CHECK(!r.Complete());

    const Json learned = ParseOrDie(R"({
      "controls": [
        {"id": "deckA.play", "learned": {"status": "accepted", "verified_input": true,
          "inputs": [{"key": "90:11", "class": "note_pair"}]}},
        {"id": "deckA.cue", "learned": {"status": "accepted", "verified_input": true,
          "inputs": [{"key": "90:10", "class": "note_pair"}],
          "led_tests": [{"status": "B0", "data1": "08", "lit": true}], "verified_led": true}},
        {"id": "mixer.fader", "learned": {"status": "accepted", "verified_input": true,
          "inputs": [{"key": "B0:08", "class": "cc_absolute"}, {"key": "B0:24", "class": "cc_absolute"}]}}
      ],
      "learn": {"background": ["B0:00"], "observed": {"90:11": 2, "90:10": 2, "B0:08": 40, "B0:24": 40, "B0:00": 900, "B0:6E": 3}}
    })");
    r = BuildCoverage(inv, learned, Json());
    CHECK_EQ(r.inputsVerified, 3u);
    CHECK_EQ(r.ledsVerified, 2u);
    CHECK_EQ(r.unassignedInputs.size(), 1u);   // B0:6E; B0:00 is background
    CHECK(r.unassignedInputs[0].find("B0 6E") == 0);
    CHECK(!r.Complete());

    const Json ledmap = ParseOrDie(R"({"results": [
      {"status": "B0", "data1": "36", "on": "7F", "observed": "mixer.meter", "controls": ["mixer.meter"]},
      {"status": "B0", "data1": "37", "on": "7F", "observed": "some ring", "controls": ["some", "ring"]},
      {"status": "B0", "data1": "38", "on": "7F", "observed": "", "controls": []},
      {"status": "B0", "data1": "39", "on": "7F", "observed": "mixer.meter", "controls": ["mixer.meter"], "dry_run": true}
    ]})");
    r = BuildCoverage(inv, learned, ledmap);
    CHECK_EQ(r.ledsVerified, 3u);
    CHECK_EQ(r.unassignedLeds.size(), 1u);

    // Resolve the last unassigned input and it is complete.
    Json learned2 = learned;
    learned2.Get("learn")->Get("observed")->Remove("B0:6E");
    r = BuildCoverage(inv, learned2, ledmap);
    CHECK(r.Complete());

    // The same key verified on two controls is reported.
    Json dup = learned2;
    Json * cue = FindControl(dup, "deckA.cue");
    cue->Get("learned")->Set("inputs", ParseOrDie(R"([{"key": "90:11", "class": "note_pair"}])"));
    r = BuildCoverage(inv, dup, ledmap);
    CHECK_EQ(r.duplicateKeys.size(), 1u);

    const std::string md = RenderCoverageMarkdown(r, inv);
    CHECK(md.find("| Play A `deckA.play` |") != std::string::npos);
    CHECK(RenderCoverageText(r).find("Inputs verified: 3/3") != std::string::npos);
}

static void TestInventoryHelpers()
{
    const Json led = ParseOrDie(R"({"status": "B0", "data1": "09", "on": "7F", "off": "00"})");
    LedMessage m;
    CHECK(ParseLedEntry(led, &m) && m.status == 0xB0 && m.data1 == 0x09 && m.on == 0x7F);
    CHECK(!ParseLedEntry(ParseOrDie(R"({"status": "C0", "data1": "09"})"), &m));
    CHECK(!ParseLedEntry(ParseOrDie(R"({"status": "B0", "data1": "80"})"), &m));
    CHECK(!ParseLedEntry(ParseOrDie(R"({"status": "B0", "data1": "09", "on": "FF"})"), &m));
    CHECK_EQ(ControlInstruction(ParseOrDie(R"({"kind": "platter"})")), std::string("spin forward, then backward"));
    CHECK(!ControlHasInput(ParseOrDie(R"({"kind": "led"})")));
    CHECK(ControlHasLed(ParseOrDie(R"({"kind": "button", "expected_led": [{}]})")));
}

int main()
{
    TestJsonRoundTrip();
    TestMidiParsing();
    TestNotePairs();
    TestCcButtonAndRange();
    TestRelativeEncoders();
    Test14Bit();
    TestIgnore();
    TestVerifyCoverage();
    TestInventoryHelpers();
    std::printf("unit tests: %d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
