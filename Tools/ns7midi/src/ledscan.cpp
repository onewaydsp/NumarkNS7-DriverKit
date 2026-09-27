// ledscan.cpp
// ns7midi ledscan [options]
//
// Lights candidate LED addresses one at a time so a human can say what lit.
// Safety: it only ever sends channel-1 B0 (CC) or 90 (note) messages whose
// data1 is inside the requested range (or --only list), with 7-bit values,
// and it prints every message before sending it. --dry-run sends nothing.

#include "answers.h"
#include "commands.h"
#include "inventory.h"
#include "json.h"
#include "midi_io.h"

#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <sys/stat.h>

namespace {

const char * const kLedscanUsage =
    "usage: ns7midi ledscan [options]\n"
    "  --status b0|90|both   message type to sweep (default b0: the NS7 LEDs seen so far are CCs)\n"
    "  --from <hex> --to <hex>  data1 range, inclusive, hex (default 00..7F)\n"
    "  --only B0:09,B0:08    test just these addresses (must be B0/90)\n"
    "  --inventory <file>    test every expected_led from the inventory; 'y' confirms the expected control\n"
    "  --value <hex>         ON value (default 7F; some LEDs may want other values)\n"
    "  --off-value <hex>     OFF value (default 00)\n"
    "  --hold <ms>           ON time when not interactive (default 500)\n"
    "  --gap <ms>            pause after OFF (default 150)\n"
    "  --interactive         keep each LED on until you type what lit (Enter = nothing)\n"
    "  --script <file>       answers from a file or FIFO instead of the terminal (implies --interactive)\n"
    "  --out <file>          results JSON for --interactive (default ledmap.json), merged if it exists\n"
    "  --all-off             only send OFF for the whole range (cleanup) and exit\n"
    "  --dry-run             print what would be sent; open no MIDI connection\n";

struct Target {
    uint8_t status;
    uint8_t data1;
    std::vector<std::string> expected;   // control ids from --inventory
    std::string expectedLabel;
};

struct Options {
    std::string statusMode = "b0";
    uint8_t from = 0x00, to = 0x7F, value = 0x7F, offValue = 0x00;
    int holdMs = 500, gapMs = 150;
    bool interactive = false, allOff = false, dryRun = false;
    std::string only, inventory, script, out = "ledmap.json";
};

bool ParseMs(const std::string & text, int * out)
{
    char * end = nullptr;
    const long v = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0' || v < 0 || v > 60000) return false;
    *out = int(v);
    return true;
}

bool Parse7Bit(const std::string & text, uint8_t * out)
{
    return ParseHexByte(text, out) && *out <= 0x7F;
}

bool ParseOptions(const std::vector<std::string> & args, Options * o)
{
    for (size_t i = 0; i < args.size(); i++) {
        const std::string & a = args[i];
        std::string v;
        auto value = [&] {
            if (i + 1 >= args.size()) return false;
            v = args[++i];
            return true;
        };
        if (a == "--status") { if (!value() || (v != "b0" && v != "B0" && v != "90" && v != "both")) return false; o->statusMode = v == "B0" ? "b0" : v; }
        else if (a == "--from") { if (!value() || !Parse7Bit(v, &o->from)) return false; }
        else if (a == "--to") { if (!value() || !Parse7Bit(v, &o->to)) return false; }
        else if (a == "--value") { if (!value() || !Parse7Bit(v, &o->value)) return false; }
        else if (a == "--off-value") { if (!value() || !Parse7Bit(v, &o->offValue)) return false; }
        else if (a == "--hold") { if (!value() || !ParseMs(v, &o->holdMs)) return false; }
        else if (a == "--gap") { if (!value() || !ParseMs(v, &o->gapMs)) return false; }
        else if (a == "--only") { if (!value()) return false; o->only = v; }
        else if (a == "--inventory") { if (!value()) return false; o->inventory = v; }
        else if (a == "--script") { if (!value()) return false; o->script = v; o->interactive = true; }
        else if (a == "--out") { if (!value()) return false; o->out = v; }
        else if (a == "--interactive") o->interactive = true;
        else if (a == "--all-off") o->allOff = true;
        else if (a == "--dry-run") o->dryRun = true;
        else if (a == "--help" || a == "-h") return false;
        else { std::fprintf(stderr, "unknown argument %s\n", a.c_str()); return false; }
    }
    if (o->from > o->to) { std::fprintf(stderr, "--from is after --to\n"); return false; }
    return true;
}

bool BuildTargets(const Options & o, std::vector<Target> * targets)
{
    if (!o.inventory.empty()) {
        Json inv;
        std::string error;
        if (!ReadJsonFile(o.inventory, &inv, &error)) { std::fprintf(stderr, "%s\n", error.c_str()); return false; }
        std::map<std::string, size_t> index;
        for (const Json & control : inv.At("controls").items()) {
            for (const Json & entry : control.At("expected_led").items()) {
                LedMessage led;
                if (!ParseLedEntry(entry, &led) || led.status > 0xBF || (led.status & 0x0F) != 0) {
                    std::fprintf(stderr, "skipping %s LED entry %s: not a channel-1 B0/90 message\n",
                                 control.StringAt("id").c_str(), entry.Dump(0).c_str());
                    continue;
                }
                const std::string key = MakeKey(led.status, led.data1);
                auto found = index.find(key);
                if (found == index.end()) {
                    found = index.emplace(key, targets->size()).first;
                    targets->push_back(Target { led.status, led.data1, {}, "" });
                }
                Target & t = (*targets)[found->second];
                t.expected.push_back(control.StringAt("id"));
                t.expectedLabel += (t.expectedLabel.empty() ? "" : " / ") + control.StringAt("label");
            }
        }
        return true;
    }
    if (!o.only.empty()) {
        for (const std::string & item : SplitList(o.only)) {
            uint8_t status = 0;
            int data1 = -1;
            if (!ParseKey(item, &status, &data1) || data1 < 0 || (status != 0xB0 && status != 0x90)) {
                std::fprintf(stderr, "--only %s: expected B0:nn or 90:nn\n", item.c_str());
                return false;
            }
            targets->push_back(Target { status, uint8_t(data1), {}, "" });
        }
        return true;
    }
    std::vector<uint8_t> statuses;
    if (o.statusMode == "b0" || o.statusMode == "both") statuses.push_back(0xB0);
    if (o.statusMode == "90" || o.statusMode == "both") statuses.push_back(0x90);
    for (uint8_t status : statuses)
        for (unsigned d = o.from; d <= o.to; d++) targets->push_back(Target { status, uint8_t(d), {}, "" });
    return true;
}

// Last line of defence: nothing but channel-1 B0/90 with 7-bit data leaves.
bool SafeSend(MidiOutput * out, uint8_t status, uint8_t data1, uint8_t value)
{
    if ((status != 0xB0 && status != 0x90) || data1 > 0x7F || value > 0x7F) {
        std::printf("    refused %02X %02X %02X: outside the ledscan whitelist\n", status, data1, value);
        return false;
    }
    return out->Send(MidiMsg { status, data1, value, 0 });
}

bool FileExists(const std::string & path)
{
    struct stat st {};
    return stat(path.c_str(), &st) == 0;
}

}   // namespace

int LedscanCommand(const std::vector<std::string> & args)
{
    Options o;
    if (!ParseOptions(args, &o)) { std::fputs(kLedscanUsage, stderr); return 2; }
    std::vector<Target> targets;
    if (!BuildTargets(o, &targets)) return 2;
    if (targets.empty()) { std::fprintf(stderr, "nothing to scan\n"); return 2; }

    std::string error;
    std::unique_ptr<MidiOutput> dry;
    CoreMidiSession session;
    MidiOutput * out = nullptr;
    if (o.dryRun) {
        dry = MakeDryRunOutput();
        out = dry.get();
    } else {
        if (!session.Open(false, true, &error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
        out = session.output();
    }

    std::printf("ns7midi ledscan: %zu address(es), ON value %s, OFF value %s%s\n", targets.size(),
                Hex2(o.value).c_str(), Hex2(o.offValue).c_str(), o.dryRun ? " (dry run: nothing is sent)" : "");

    if (o.allOff) {
        for (const Target & t : targets) SafeSend(out, t.status, t.data1, o.offValue);
        std::printf("all off: %zu message(s)\n", targets.size());
        return 0;
    }

    std::unique_ptr<LineSource> answers;
    Json doc = Json::Object();
    if (o.interactive) {
        answers = o.script.empty() ? LineSource::Stdin() : LineSource::Open(o.script, &error);
        if (!answers) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
        if (FileExists(o.out) && !ReadJsonFile(o.out, &doc, &error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
        doc.Set("schema", "ns7-ledmap/1");
        if (!doc.Get("results")) doc.Set("results", Json::Array());
        std::printf("For each address type what lit: a control id (e.g. deckA.play), free text, or several\n"
                    "separated by commas. Enter = nothing lit, r = repeat, q = save & quit.%s\n",
                    o.inventory.empty() ? "" : " y = the expected control lit.");
    }

    // Replaces the result for this address and ON value, or appends one.
    auto record = [&](const Target & t, const std::string & observed, const std::vector<std::string> & controls) {
        Json r = Json::Object();
        r.Set("status", Hex2(t.status));
        r.Set("data1", Hex2(t.data1));
        r.Set("on", Hex2(o.value));
        r.Set("off", Hex2(o.offValue));
        r.Set("observed", observed);
        Json ids = Json::Array();
        for (const std::string & id : controls) ids.Push(id);
        r.Set("controls", ids);
        r.Set("dry_run", o.dryRun);
        r.Set("when", NowIso8601());
        std::vector<Json> & results = doc.Get("results")->items();
        for (Json & existing : results) {
            if (existing.StringAt("status") == r.StringAt("status") && existing.StringAt("data1") == r.StringAt("data1") &&
                existing.StringAt("on") == r.StringAt("on")) { existing = r; return; }
        }
        results.push_back(r);
    };
    auto save = [&] {
        if (!o.interactive) return true;
        if (!WriteJsonFile(o.out, doc, &error)) { std::fprintf(stderr, "%s\n", error.c_str()); return false; }
        return true;
    };

    for (size_t i = 0; i < targets.size(); i++) {
        const Target & t = targets[i];
        std::printf("[%02X %02X] ON%s%s\n", t.status, t.data1,
                    t.expectedLabel.empty() ? "" : "   expected: ", t.expectedLabel.c_str());
        std::fflush(stdout);
        SafeSend(out, t.status, t.data1, o.value);
        std::string line;
        bool quit = false, repeat = false;
        if (o.interactive) {
            std::printf("  what lit? ");
            std::fflush(stdout);
            const LineSource::Result r = answers->WaitLine(-1, &line);
            if (answers->IsScript() || r != LineSource::Result::Line)
                std::printf("%s\n", r == LineSource::Result::Line ? (line.empty() ? "(Enter)" : line.c_str()) : "(end of answers)");
            if (r != LineSource::Result::Line || line == "q") quit = true;
            else if (line == "r") repeat = true;
        } else {
            SleepMs(o.holdMs);
        }
        SafeSend(out, t.status, t.data1, o.offValue);
        std::printf("[%02X %02X] OFF\n", t.status, t.data1);
        std::fflush(stdout);
        if (quit) break;
        if (repeat) { i--; SleepMs(o.gapMs); continue; }
        if (o.interactive) {
            const bool confirmed = line == "y" && !t.expected.empty();
            record(t, confirmed ? "expected" : line, confirmed ? t.expected : SplitList(line));
            if (!save()) return 1;
        }
        SleepMs(o.gapMs);
    }
    if (o.interactive) std::printf("Saved %s\n", o.out.c_str());
    return save() ? 0 : 1;
}
