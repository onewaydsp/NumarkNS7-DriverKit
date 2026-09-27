// learn.cpp
// ns7midi learn <inventory.json> <out.json> [options]
//
// Walks the inventory one control at a time. For each control it prints what
// to do, captures every incoming MIDI message until an answer line arrives
// (Enter = accept, s = skip, r = redo, q = save & quit) or the step times
// out, summarizes the capture and saves the result into out.json after every
// step. out.json is the inventory with a "learned" object added per control
// and a top-level "learn" object (background keys, all observed keys, the
// final sweep).

#include "answers.h"
#include "commands.h"
#include "inventory.h"
#include "json.h"
#include "midi_io.h"
#include "summarize.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <map>
#include <memory>
#include <set>
#include <sys/stat.h>

std::string NowIso8601()
{
    char buf[32];
    const std::time_t now = std::time(nullptr);
    std::tm local {};
    localtime_r(&now, &local);
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S%z", &local);
    return buf;
}

std::vector<std::string> SplitList(const std::string & text, char separator)
{
    std::vector<std::string> out;
    std::string item;
    for (char c : text) {
        if (c == separator) { if (!item.empty()) out.push_back(item); item.clear(); }
        else if (c != ' ') item.push_back(c);
    }
    if (!item.empty()) out.push_back(item);
    return out;
}

namespace {

const char * const kLearnUsage =
    "usage: ns7midi learn <inventory.json> <out.json> [options]\n"
    "  --from <id>          start at this control (always re-learns it; later ones as usual)\n"
    "  --only <id,id,...>   learn just these controls\n"
    "  --all                re-learn controls already verified in out.json\n"
    "  --retry-skipped      also revisit controls that were skipped, empty or timed out\n"
    "  --timeout <s>        end a step after s seconds (0 = wait for an answer; default 0)\n"
    "  --min <s>            capture at least s seconds before reading the answer\n"
    "  --script <file>      read answers from a file or FIFO instead of the terminal\n"
    "  --replay <file>      feed recorded MIDI (replay format) instead of CoreMIDI\n"
    "  --baseline <s>       hands-off capture before the first step (default 3, 0 = off)\n"
    "  --ignore <keys>      message keys to ignore, e.g. B0:00,E0\n"
    "  --no-sweep           skip the final 'move everything' sweep\n"
    "  --leds               after each control with a known LED, light it and ask y/n\n"
    "  --dry-run            print LED messages instead of sending them\n";

struct Options {
    std::string inventoryPath, outPath, from, script, replay;
    std::set<std::string> only, ignore;
    bool all = false, retrySkipped = false, sweep = true, leds = false, dryRun = false;
    double timeout = 0, min = 0, baseline = 3;
};

bool ParseSeconds(const std::string & text, double * out)
{
    char * end = nullptr;
    const double v = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0' || v < 0 || v > 86400) return false;
    *out = v;
    return true;
}

bool ParseOptions(const std::vector<std::string> & args, Options * o)
{
    std::vector<std::string> positional;
    for (size_t i = 0; i < args.size(); i++) {
        const std::string & a = args[i];
        auto value = [&](std::string * out) {
            if (i + 1 >= args.size()) return false;
            *out = args[++i];
            return true;
        };
        std::string v;
        if (a == "--from") { if (!value(&o->from)) return false; }
        else if (a == "--only") { if (!value(&v)) return false; for (auto & id : SplitList(v)) o->only.insert(id); }
        else if (a == "--all") o->all = true;
        else if (a == "--retry-skipped") o->retrySkipped = true;
        else if (a == "--timeout") { if (!value(&v) || !ParseSeconds(v, &o->timeout)) return false; }
        else if (a == "--min") { if (!value(&v) || !ParseSeconds(v, &o->min)) return false; }
        else if (a == "--baseline") { if (!value(&v) || !ParseSeconds(v, &o->baseline)) return false; }
        else if (a == "--script") { if (!value(&o->script)) return false; }
        else if (a == "--replay") { if (!value(&o->replay)) return false; }
        else if (a == "--ignore") {
            if (!value(&v)) return false;
            for (auto & key : SplitList(v)) {
                uint8_t status = 0;
                int data1 = 0;
                if (!ParseKey(key, &status, &data1)) { std::fprintf(stderr, "bad key %s\n", key.c_str()); return false; }
                o->ignore.insert(MakeKey(status, data1));
            }
        }
        else if (a == "--no-sweep") o->sweep = false;
        else if (a == "--leds") o->leds = true;
        else if (a == "--dry-run") o->dryRun = true;
        else if (a == "--help" || a == "-h") return false;
        else if (!a.empty() && a[0] == '-') { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return false; }
        else positional.push_back(a);
    }
    if (positional.size() != 2) return false;
    o->inventoryPath = positional[0];
    o->outPath = positional[1];
    return true;
}

bool FileExists(const std::string & path)
{
    struct stat st {};
    return stat(path.c_str(), &st) == 0;
}

enum class Answer { Accept, Skip, Redo, Quit, Timeout };

struct Capture {
    std::vector<MidiMsg> msgs;
    Answer answer = Answer::Timeout;
};

class Learner {
public:
    Learner(const Options & o, MidiInput * in, MidiOutput * out, LineSource * answers)
        : o_(o), in_(in), out_(out), answers_(answers) {}

    int Run();

private:
    bool Load();
    bool Save();
    Capture CaptureStep(bool wantAnswer, double fixedSeconds);
    bool ReadAnswer(double timeout, Answer * answer, bool * gotLine);
    std::set<std::string> IgnoreSet() const;
    void Baseline();
    // Returns false to stop the session.
    bool LearnControl(Json & control, size_t index, size_t total);
    void CheckLeds(Json & control, Json & learned);
    void Sweep();
    std::map<std::string, std::string> AssignedKeys() const;

    const Options & o_;
    MidiInput * in_;
    MidiOutput * out_;
    LineSource * answers_;
    Json doc_;
    bool quit_ = false;
    bool inputEnded_ = false;
};

bool Learner::Load()
{
    Json inventory;
    std::string error;
    if (!ReadJsonFile(o_.inventoryPath, &inventory, &error)) { std::fprintf(stderr, "%s\n", error.c_str()); return false; }
    if (!inventory.At("controls").IsArray()) { std::fprintf(stderr, "%s: no controls array\n", o_.inventoryPath.c_str()); return false; }

    Json previous;
    if (FileExists(o_.outPath) && !ReadJsonFile(o_.outPath, &previous, &error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return false;
    }
    // Start from the inventory so its edits carry over, then bring back what
    // earlier sessions learned.
    doc_ = inventory;
    doc_.Set("schema", "ns7-learned/1");
    doc_.Set("inventory", o_.inventoryPath);
    for (Json & control : doc_.Get("controls")->items()) {
        const Json * old = FindControl(previous, control.StringAt("id"));
        if (old && old->Get("learned")) control.Set("learned", old->At("learned"));
    }
    Json learn = previous.At("learn").IsObject() ? previous.At("learn") : Json::Object();
    if (!learn.Get("observed")) learn.Set("observed", Json::Object());
    if (!learn.Get("background")) learn.Set("background", Json::Array());
    doc_.Set("learn", learn);
    return true;
}

bool Learner::Save()
{
    doc_.Get("learn")->Set("updated", NowIso8601());
    std::string error;
    if (!WriteJsonFile(o_.outPath, doc_, &error)) { std::fprintf(stderr, "%s\n", error.c_str()); return false; }
    return true;
}

std::set<std::string> Learner::IgnoreSet() const
{
    std::set<std::string> keys = o_.ignore;
    for (const Json & k : doc_.At("learn").At("background").items()) keys.insert(k.AsString());
    return keys;
}

bool Learner::ReadAnswer(double timeout, Answer * answer, bool * gotLine)
{
    std::string line;
    *gotLine = false;
    const LineSource::Result r = answers_->WaitLine(timeout, &line);
    if (r == LineSource::Result::Timeout) return false;
    *gotLine = true;
    if (r == LineSource::Result::Eof) {
        std::printf("  (end of answers: save & quit)\n");
        *answer = Answer::Quit;
        return true;
    }
    if (answers_->IsScript()) std::printf("  > %s\n", line.empty() ? "(Enter)" : line.c_str());
    if (line.empty() || line == "a" || line == "accept" || line == "y") *answer = Answer::Accept;
    else if (line == "s" || line == "skip") *answer = Answer::Skip;
    else if (line == "r" || line == "redo") *answer = Answer::Redo;
    else if (line == "q" || line == "quit") *answer = Answer::Quit;
    else {
        std::printf("  unknown answer \"%s\": Enter = accept, s = skip, r = redo, q = save & quit\n", line.c_str());
        return false;
    }
    return true;
}

// Captures one step. With wantAnswer the step ends on an answer line (after
// --min seconds, and for a replay once its block is delivered) or on
// --timeout; otherwise it runs for fixedSeconds (a replay: one block).
Capture Learner::CaptureStep(bool wantAnswer, double fixedSeconds)
{
    using Clock = std::chrono::steady_clock;
    Capture capture;
    if (!in_->BeginStep()) {
        std::printf("  replay has no more blocks: save & quit\n");
        inputEnded_ = true;
        capture.answer = Answer::Quit;
        return capture;
    }
    const auto start = Clock::now();
    size_t printed = 0;
    const size_t kPrintLimit = 16;
    const std::set<std::string> ignore = IgnoreSet();
    auto drain = [&] {
        for (const MidiMsg & msg : in_->Drain()) {
            capture.msgs.push_back(msg);
            if (ignore.count(MsgKey(msg))) continue;
            if (printed < kPrintLimit) std::printf("    < %s\n", FormatMsg(msg).c_str());
            else if (printed == kPrintLimit) std::printf("    < ... (more)\n");
            printed++;
        }
        std::fflush(stdout);
    };
    while (true) {
        drain();
        const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
        if (!wantAnswer) {
            if (in_->IsReplay() ? in_->StepComplete() : elapsed >= fixedSeconds) break;
            SleepMs(50);
            continue;
        }
        if (o_.timeout > 0 && elapsed >= o_.timeout) {
            std::printf("  (timeout after %.0f s)\n", o_.timeout);
            capture.answer = Answer::Timeout;
            break;
        }
        const bool ready = elapsed >= o_.min && (!in_->IsReplay() || in_->StepComplete());
        if (!ready) { SleepMs(50); continue; }
        Answer answer;
        bool gotLine = false;
        if (ReadAnswer(0.05, &answer, &gotLine)) {
            capture.answer = answer;
            break;
        }
    }
    drain();
    return capture;
}

void Learner::Baseline()
{
    if (o_.baseline <= 0) return;
    std::printf("\n== Baseline: hands off the controller for %.0f s (platter motors off) ==\n", o_.baseline);
    std::fflush(stdout);
    const Capture capture = CaptureStep(false, o_.baseline);
    if (inputEnded_) return;
    const Summary summary = Summarize(capture.msgs, o_.ignore);
    Json background = Json::Array();
    for (const MsgGroup & g : summary.groups) background.Push(g.key);
    doc_.Get("learn")->Set("background", background);
    doc_.Get("learn")->Set("background_detail", SummaryToJson(summary));
    if (summary.groups.empty()) {
        std::printf("  quiet: nothing arrived.\n");
    } else {
        std::printf("  WARNING: these keep arriving with hands off; they are ignored in every step:\n");
        for (const std::string & line : DescribeSummary(summary)) std::printf("    %s\n", line.c_str());
        std::printf("  (Turn platter motors off and rerun if a platter is streaming.)\n");
    }
}

std::map<std::string, std::string> Learner::AssignedKeys() const
{
    std::map<std::string, std::string> assigned;
    for (const Json & control : doc_.At("controls").items()) {
        const Json & learned = control.At("learned");
        if (!learned.BoolAt("verified_input")) continue;
        for (const Json & group : learned.At("inputs").items())
            assigned.emplace(group.StringAt("key"), control.StringAt("id"));
    }
    return assigned;
}

void Learner::CheckLeds(Json & control, Json & learned)
{
    Json tested = Json::Array();
    bool anyLit = false;
    std::set<std::string> done;
    for (const Json & entry : control.At("expected_led").items()) {
        LedMessage led;
        if (!ParseLedEntry(entry, &led)) {
            std::printf("  LED entry %s is not a B0/90 message; not sent\n", entry.Dump(0).c_str());
            continue;
        }
        const std::string key = MakeKey(led.status, led.data1) + ":" + Hex2(led.on);
        if (!done.insert(key).second) continue;
        std::printf("  LED check %s %s: sending ON\n", Hex2(led.status).c_str(), Hex2(led.data1).c_str());
        out_->Send(MidiMsg { led.status, led.data1, led.on, 0 });
        std::printf("  Is the %s light ON now? [y = yes, n or Enter = no]\n", control.StringAt("label").c_str());
        std::fflush(stdout);
        std::string line;
        const LineSource::Result r = answers_->WaitLine(o_.timeout > 0 ? o_.timeout : -1, &line);
        if (r == LineSource::Result::Line && answers_->IsScript()) std::printf("  > %s\n", line.empty() ? "(Enter)" : line.c_str());
        const bool lit = r == LineSource::Result::Line && (line == "y" || line == "yes");
        out_->Send(MidiMsg { led.status, led.data1, led.off, 0 });
        Json t = Json::Object();
        t.Set("status", Hex2(led.status));
        t.Set("data1", Hex2(led.data1));
        t.Set("on", Hex2(led.on));
        t.Set("off", Hex2(led.off));
        t.Set("lit", lit);
        t.Set("dry_run", out_->IsDryRun());
        tested.Push(t);
        anyLit = anyLit || lit;
        if (r == LineSource::Result::Eof) { quit_ = true; break; }
    }
    if (tested.size() == 0) return;
    learned.Set("led_tests", tested);
    // A dry run proves nothing about the hardware.
    learned.Set("verified_led", anyLit && !out_->IsDryRun());
}

bool Learner::LearnControl(Json & control, size_t index, size_t total)
{
    const std::string id = control.StringAt("id");
    while (true) {
        std::printf("\n[%zu/%zu] %s: %s (%s)\n", index, total, id.c_str(),
                    control.StringAt("label").c_str(), control.StringAt("kind").c_str());
        std::printf("  Do: %s.\n", ControlInstruction(control).c_str());
        std::printf("  Prior input: %s\n", DescribeExpected(control.At("expected_input")).c_str());
        if (control.BoolAt("uncertain")) std::printf("  Note: this control is marked uncertain in the inventory.\n");
        std::printf("  Then: Enter = accept, s = skip, r = redo, q = save & quit\n");
        std::fflush(stdout);

        const Capture capture = CaptureStep(true, 0);
        if (capture.answer == Answer::Redo) { std::printf("  redo\n"); continue; }
        if (capture.answer == Answer::Quit) { quit_ = true; return false; }

        const std::set<std::string> ignore = IgnoreSet();
        const Summary summary = Summarize(capture.msgs, ignore);
        Json learned = Json::Object();
        learned.Set("when", NowIso8601());
        learned.Set("instruction", ControlInstruction(control));

        // Everything seen counts toward the observed set, so nothing is missed.
        Json & observed = *doc_.Get("learn")->Get("observed");
        for (const MsgGroup & g : summary.groups)
            observed.Set(g.key, observed.NumberAt(g.key) + double(g.count));

        if (capture.answer == Answer::Skip) {
            learned.Set("status", "skipped");
            learned.Set("verified_input", false);
            std::printf("  skipped\n");
        } else {
            for (const std::string & line : DescribeSummary(summary)) std::printf("  = %s\n", line.c_str());
            const bool empty = summary.groups.empty();
            if (empty) std::printf("  WARNING: nothing captured; recorded as unverified (rerun with --from %s)\n", id.c_str());
            learned.Set("status", empty ? (capture.answer == Answer::Timeout ? "timeout" : "empty") : "accepted");
            learned.Set("verified_input", !empty);
            learned.Set("inputs", SummaryToJson(summary));
            learned.Set("sample", SampleToJson(capture.msgs, 24, ignore));
            learned.Set("message_count", summary.total - summary.ignored);

            // Flag keys another verified control already owns.
            Json conflicts = Json::Array();
            const auto assigned = AssignedKeys();
            for (const MsgGroup & g : summary.groups) {
                auto owner = assigned.find(g.key);
                if (owner != assigned.end() && owner->second != id) {
                    std::printf("  WARNING: %s is also assigned to %s\n", g.key.c_str(), owner->second.c_str());
                    conflicts.Push(g.key + " also " + owner->second);
                }
            }
            if (conflicts.size()) learned.Set("conflicts", conflicts);
            if (o_.leds && ControlHasLed(control)) CheckLeds(control, learned);
        }
        control.Set("learned", learned);
        Save();
        return !quit_;
    }
}

void Learner::Sweep()
{
    std::printf("\n== Final sweep: move, press and touch EVERY control once. ==\n"
                "  Anything that is not assigned to a control will be listed. Enter when done.\n");
    std::fflush(stdout);
    const Capture capture = CaptureStep(true, 0);
    if (capture.answer == Answer::Quit || capture.answer == Answer::Skip || inputEnded_) return;
    const Summary summary = Summarize(capture.msgs, IgnoreSet());
    Json & observed = *doc_.Get("learn")->Get("observed");
    for (const MsgGroup & g : summary.groups)
        observed.Set(g.key, observed.NumberAt(g.key) + double(g.count));
    const auto assigned = AssignedKeys();
    Json unassigned = Json::Array();
    for (const MsgGroup & g : summary.groups)
        if (!assigned.count(g.key)) unassigned.Push(g.key);
    Json sweep = Json::Object();
    sweep.Set("when", NowIso8601());
    sweep.Set("inputs", SummaryToJson(summary));
    sweep.Set("unassigned", unassigned);
    doc_.Get("learn")->Set("sweep", sweep);
    std::printf("  sweep saw %zu message group(s); %zu not assigned to any control%s\n",
                summary.groups.size(), unassigned.size(), unassigned.size() ? ":" : "");
    for (const Json & key : unassigned.items()) std::printf("    %s\n", key.AsString().c_str());
}

int Learner::Run()
{
    if (!Load()) return 1;
    Json & controls = *doc_.Get("controls");

    // Pick the steps.
    std::vector<size_t> steps;
    bool started = o_.from.empty();
    if (!o_.from.empty() && !FindControl(doc_, o_.from)) {
        std::fprintf(stderr, "--from %s: no such control id\n", o_.from.c_str());
        return 2;
    }
    for (const std::string & id : o_.only)
        if (!FindControl(doc_, id)) { std::fprintf(stderr, "--only %s: no such control id\n", id.c_str()); return 2; }
    for (size_t i = 0; i < controls.size(); i++) {
        const Json & c = controls.items()[i];
        const std::string id = c.StringAt("id");
        if (id == o_.from) started = true;
        if (!started || !ControlHasInput(c)) continue;
        if (!o_.only.empty()) { if (o_.only.count(id)) steps.push_back(i); continue; }
        const Json & learned = c.At("learned");
        // --from always redoes its own control; later ones follow the usual
        // resume rule unless --all.
        const bool redoAnyway = o_.all || id == o_.from;
        const bool done = learned.BoolAt("verified_input") ||
                          (!o_.retrySkipped && learned.IsObject());
        if (redoAnyway || !done) steps.push_back(i);
    }

    std::printf("ns7midi learn: %zu control(s) to capture, results in %s%s\n", steps.size(),
                o_.outPath.c_str(), o_.dryRun ? " (dry run: LED messages are printed, not sent)" : "");
    if (!Save()) return 1;
    Baseline();
    if (!Save()) return 1;

    for (size_t n = 0; n < steps.size() && !quit_ && !inputEnded_; n++)
        if (!LearnControl(controls.items()[steps[n]], n + 1, steps.size())) break;

    if (!quit_ && !inputEnded_ && o_.sweep) Sweep();
    if (!Save()) return 1;

    size_t verified = 0, eligible = 0;
    for (const Json & c : controls.items()) {
        if (!ControlHasInput(c)) continue;
        eligible++;
        if (c.At("learned").BoolAt("verified_input")) verified++;
    }
    std::printf("\nSaved %s: %zu of %zu input controls verified.%s\n", o_.outPath.c_str(), verified, eligible,
                quit_ ? " Resume with the same command (or --from <id>)." : "");
    return 0;
}

}   // namespace

int LearnCommand(const std::vector<std::string> & args)
{
    Options o;
    if (!ParseOptions(args, &o)) { std::fputs(kLearnUsage, stderr); return 2; }

    std::string error;
    std::unique_ptr<LineSource> answers = o.script.empty() ? LineSource::Stdin() : LineSource::Open(o.script, &error);
    if (!answers) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }

    // MIDI in: a replay file or the NS7. MIDI out: only needed for --leds.
    std::unique_ptr<MidiInput> replay;
    std::unique_ptr<MidiOutput> dryOut;
    CoreMidiSession session;
    MidiInput * in = nullptr;
    MidiOutput * out = nullptr;
    if (!o.replay.empty()) {
        replay = OpenReplayInput(o.replay, &error);
        if (!replay) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
        in = replay.get();
    }
    const bool liveOut = o.leds && !o.dryRun;
    if (!in || liveOut) {
        if (!session.Open(!in, liveOut, &error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
        if (!in) in = session.input();
        if (liveOut) out = session.output();
    }
    if (!out) { dryOut = MakeDryRunOutput(); out = dryOut.get(); }

    Learner learner(o, in, out, answers.get());
    return learner.Run();
}
