// main.cpp
// ns7midi: CoreMIDI-side checks and the control verification tools for the
// NS7 dext. See Tools/ns7midi/README.md.
//
//   ns7midi list                          every MIDI device, entity and endpoint
//   ns7midi monitor [seconds] [--record f] MIDI arriving from the NS7 (default 20 s)
//   ns7midi send <hex bytes...>           send channel voice messages, e.g. 90 11 7F
//   ns7midi learn ...                     guided input capture (see README)
//   ns7midi ledscan ...                   light LED addresses one at a time
//   ns7midi verify ...                    coverage report + docs/controls/NS7-MIDI-MAP.md

#include "commands.h"
#include "midi_io.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static const char * const kUsage =
    "usage: ns7midi <command> ...\n"
    "  list\n"
    "  monitor [seconds] [--record <replay-file>]\n"
    "  send <hex bytes...>\n"
    "  learn <inventory.json> <out.json> [--from id] [--script f] [--timeout s] [--replay f] ...\n"
    "  ledscan [--status b0|90|both] [--from hex --to hex] [--hold ms] [--value hex] [--interactive] ...\n"
    "  verify <inventory.json> <learned.json|-> [ledmap.json] [--md file | --no-md]\n"
    "Run a command without arguments (learn) or with --help for its options.\n";

int main(int argc, const char * argv[])
{
    if (argc < 2) { std::fputs(kUsage, stderr); return 2; }
    const std::string command = argv[1];
    std::vector<std::string> args(argv + 2, argv + argc);
    if (command == "--help" || command == "-h") { std::fputs(kUsage, stdout); return 0; }

    if (command == "list") return ListDevices();
    if (command == "monitor") {
        double seconds = 20.0;
        std::string record;
        for (size_t i = 0; i < args.size(); i++) {
            if (args[i] == "--record" && i + 1 < args.size()) record = args[++i];
            else seconds = std::atof(args[i].c_str());
        }
        return MonitorCommand(seconds, record);
    }
    if (command == "send" && !args.empty()) return SendCommand(args);
    if (command == "learn") return LearnCommand(args);
    if (command == "ledscan") return LedscanCommand(args);
    if (command == "verify") return VerifyCommand(args);
    std::fputs(kUsage, stderr);
    return 2;
}
