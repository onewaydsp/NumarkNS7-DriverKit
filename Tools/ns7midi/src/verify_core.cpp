// verify_core.cpp

#include "verify_core.h"

#include "inventory.h"
#include "midi_msg.h"

#include <cstdio>
#include <map>
#include <set>

namespace {

std::string KeyText(const std::string & key)
{
    std::string s = key;
    for (char & c : s) if (c == ':') c = ' ';
    return s;
}

std::string EntryKey(const Json & entry)
{
    uint8_t status = 0, data1 = 0;
    if (!ParseHexByte(entry.StringAt("status"), &status)) return "";
    if (entry.At("data1").IsString() && ParseHexByte(entry.StringAt("data1"), &data1)) return MakeKey(status, data1);
    return MakeKey(status, -1);
}

}   // namespace

CoverageReport BuildCoverage(const Json & inventory, const Json & learned, const Json & ledmap)
{
    CoverageReport report;
    std::set<std::string> ids;
    for (const Json & c : inventory.At("controls").items()) ids.insert(c.StringAt("id"));

    // LED confirmations from ledscan, by control id.
    std::map<std::string, std::string> ledscanHits;
    for (const Json & r : ledmap.At("results").items()) {
        const std::string observed = r.StringAt("observed");
        if (observed.empty() || r.BoolAt("dry_run")) continue;
        const std::string where = r.StringAt("status") + " " + r.StringAt("data1") +
                                  (r.StringAt("on", "7F") == "7F" ? "" : " value " + r.StringAt("on"));
        bool known = false;
        for (const Json & id : r.At("controls").items()) {
            if (ids.count(id.AsString())) {
                known = true;
                ledscanHits.emplace(id.AsString(), where + " (ledscan)");
            }
        }
        if (!known) report.unassignedLeds.push_back(where + " lit \"" + observed + "\"");
    }

    std::map<std::string, std::string> keyOwner;
    for (const Json & control : inventory.At("controls").items()) {
        ItemCoverage item;
        item.id = control.StringAt("id");
        item.section = control.StringAt("section");
        item.label = control.StringAt("label");
        item.kind = control.StringAt("kind");
        item.uncertain = control.BoolAt("uncertain");
        const Json * learnedControl = FindControl(learned, item.id);
        static const Json kNone;
        const Json & l = learnedControl ? learnedControl->At("learned") : kNone;

        item.needsInput = ControlHasInput(control);
        if (!item.needsInput) {
            item.inputState = "n/a";
        } else if (l.BoolAt("verified_input")) {
            item.inputVerified = true;
            item.inputState = "verified";
            for (const Json & g : l.At("inputs").items()) {
                const std::string key = g.StringAt("key");
                if (!item.inputText.empty()) item.inputText += ", ";
                item.inputText += KeyText(key) + " " + g.StringAt("class");
                auto owner = keyOwner.emplace(key, item.id);
                if (!owner.second && owner.first->second != item.id)
                    report.duplicateKeys.push_back(key + " on " + owner.first->second + " and " + item.id);
            }
        } else {
            item.inputState = l.IsObject() ? l.StringAt("status", "not learned") : "not learned";
            item.inputText = "prior: " + DescribeExpected(control.At("expected_input"));
        }

        item.needsLed = ControlHasLed(control);
        if (item.needsLed) {
            if (l.BoolAt("verified_led")) {
                item.ledVerified = true;
                for (const Json & t : l.At("led_tests").items())
                    if (t.BoolAt("lit")) { item.ledText = t.StringAt("status") + " " + t.StringAt("data1") + " (learn --leds)"; break; }
            } else if (ledscanHits.count(item.id)) {
                item.ledVerified = true;
                item.ledText = ledscanHits[item.id];
            } else {
                for (const Json & e : control.At("expected_led").items()) {
                    if (e.BoolAt("verified")) {
                        item.ledVerified = true;
                        item.ledText = e.StringAt("status") + " " + e.StringAt("data1") + " (prior " + e.StringAt("source") + ")";
                        break;
                    }
                }
                if (!item.ledVerified) item.ledText = "prior: " + DescribeExpected(control.At("expected_led"));
            }
        }

        if (item.needsInput) { report.inputsNeeded++; if (item.inputVerified) report.inputsVerified++; }
        if (item.needsLed) { report.ledsNeeded++; if (item.ledVerified) report.ledsVerified++; }
        report.items.push_back(item);
    }

    // Anything observed that no verified control owns.
    std::set<std::string> background;
    for (const Json & k : learned.At("learn").At("background").items()) background.insert(k.AsString());
    std::set<std::string> reported;
    for (const auto & member : learned.At("learn").At("observed").members()) {
        const std::string & key = member.first;
        if (keyOwner.count(key) || background.count(key) || !reported.insert(key).second) continue;
        char buf[96];
        std::snprintf(buf, sizeof buf, "%s (x%.0f, learn)", KeyText(key).c_str(), member.second.AsNumber());
        report.unassignedInputs.push_back(buf);
    }
    for (const Json & entry : inventory.At("unidentified_observed").items()) {
        const std::string key = EntryKey(entry);
        if (key.empty() || keyOwner.count(key) || !reported.insert(key).second) continue;
        report.unassignedInputs.push_back(KeyText(key) + " (seen on hardware before, guess: " + entry.StringAt("guess", "none") + ")");
    }
    return report;
}

std::string RenderCoverageText(const CoverageReport & report)
{
    std::string out;
    char buf[512];
    std::string section;
    for (const ItemCoverage & item : report.items) {
        if (item.section != section) { section = item.section; out += "\n[" + section + "]\n"; }
        const char * in = !item.needsInput ? "  --  " : item.inputVerified ? "  OK  " : " MISS ";
        const char * led = !item.needsLed ? "  --  " : item.ledVerified ? "  OK  " : " MISS ";
        std::snprintf(buf, sizeof buf, "  in%s led%s %-26s %s%s\n", in, led, item.id.c_str(),
                      item.inputVerified ? item.inputText.c_str() : item.inputState.c_str(),
                      item.needsLed ? ("  | LED " + item.ledText).c_str() : "");
        out += buf;
    }
    out += "\n";
    if (!report.unassignedInputs.empty()) {
        out += "Observed input messages not assigned to any control:\n";
        for (const std::string & s : report.unassignedInputs) out += "  " + s + "\n";
    }
    if (!report.unassignedLeds.empty()) {
        out += "LED outputs that lit something not in the inventory:\n";
        for (const std::string & s : report.unassignedLeds) out += "  " + s + "\n";
    }
    if (!report.duplicateKeys.empty()) {
        out += "Input keys verified on more than one control:\n";
        for (const std::string & s : report.duplicateKeys) out += "  " + s + "\n";
    }
    std::snprintf(buf, sizeof buf,
                  "Inputs verified: %zu/%zu   LEDs verified: %zu/%zu   unassigned inputs: %zu   => %s\n",
                  report.inputsVerified, report.inputsNeeded, report.ledsVerified, report.ledsNeeded,
                  report.unassignedInputs.size(), report.Complete() ? "COMPLETE" : "INCOMPLETE");
    out += buf;
    return out;
}

static std::string Cell(const std::string & text)
{
    std::string s;
    for (char c : text) s += (c == '|') ? std::string("\\|") : std::string(1, c);
    return s.empty() ? "–" : s;
}

std::string RenderCoverageMarkdown(const CoverageReport & report, const Json & inventory)
{
    std::string md;
    md += "# Numark NS7 MIDI map\n\n";
    md += "Generated by `ns7midi verify` from `docs/controls/ns7-controls.json`";
    md += " and the learn / ledscan results. Do not edit by hand; rerun verify.\n\n";
    char buf[256];
    std::snprintf(buf, sizeof buf, "Inputs verified: **%zu / %zu**. LEDs verified: **%zu / %zu**. "
                  "Unassigned observed inputs: **%zu**. Status: **%s**.\n\n",
                  report.inputsVerified, report.inputsNeeded, report.ledsVerified, report.ledsNeeded,
                  report.unassignedInputs.size(), report.Complete() ? "complete" : "incomplete");
    md += buf;
    md += "Messages are hex: `90 11` is a note on channel 1, `B0 09` a CC, `E0` pitch bend. "
          "\"prior\" means taken from the manual or the 2010 Mixxx mapping and not yet confirmed on hardware.\n";

    std::string section;
    for (const ItemCoverage & item : report.items) {
        if (item.section != section) {
            section = item.section;
            std::string title = section;
            for (const Json & s : inventory.At("sections").items())
                if (s.StringAt("id") == section) title = s.StringAt("label", section);
            md += "\n## " + title + "\n\n";
            md += "| Control | Kind | Input message(s) | Input verified | LED message | LED verified |\n";
            md += "|---|---|---|---|---|---|\n";
        }
        const std::string label = item.label + (item.uncertain ? " (uncertain)" : "");
        md += "| " + Cell(label) + " `" + item.id + "` | " + Cell(item.kind) + " | " +
              (item.needsInput ? Cell(item.inputText) : "–") + " | " +
              (item.needsInput ? (item.inputVerified ? "yes" : "no (" + item.inputState + ")") : "n/a") + " | " +
              (item.needsLed ? Cell(item.ledText) : "–") + " | " +
              (item.needsLed ? (item.ledVerified ? "yes" : "no") : "n/a") + " |\n";
    }
    if (!report.unassignedInputs.empty()) {
        md += "\n## Observed but unassigned\n\n";
        for (const std::string & s : report.unassignedInputs) md += "- " + Cell(s) + "\n";
    }
    if (!report.unassignedLeds.empty()) {
        md += "\n## LED outputs outside the inventory\n\n";
        for (const std::string & s : report.unassignedLeds) md += "- " + Cell(s) + "\n";
    }
    return md;
}
