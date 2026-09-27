// verify.cpp
// ns7midi verify <inventory.json> <learned.json|-> [ledmap.json] [--md <file>|--no-md]
//
// Prints the coverage report and writes the Markdown MIDI map (by default
// NS7-MIDI-MAP.md next to the inventory). Exits 1 while anything is
// unverified or an observed message is unassigned, 0 when complete.

#include "commands.h"
#include "json.h"
#include "verify_core.h"

#include <cstdio>
#include <fstream>

int VerifyCommand(const std::vector<std::string> & args)
{
    std::vector<std::string> positional;
    std::string mdPath;
    bool writeMd = true;
    for (size_t i = 0; i < args.size(); i++) {
        if (args[i] == "--md" && i + 1 < args.size()) mdPath = args[++i];
        else if (args[i] == "--no-md") writeMd = false;
        else if (!args[i].empty() && args[i][0] == '-' && args[i] != "-") { positional.clear(); break; }
        else positional.push_back(args[i]);
    }
    if (positional.size() < 2 || positional.size() > 3) {
        std::fputs("usage: ns7midi verify <inventory.json> <learned.json|-> [ledmap.json] [--md <file> | --no-md]\n", stderr);
        return 2;
    }

    Json inventory, learned, ledmap;
    std::string error;
    if (!ReadJsonFile(positional[0], &inventory, &error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 2; }
    if (positional[1] != "-" && !ReadJsonFile(positional[1], &learned, &error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 2; }
    if (positional.size() == 3 && !ReadJsonFile(positional[2], &ledmap, &error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 2; }

    const CoverageReport report = BuildCoverage(inventory, learned, ledmap);
    std::fputs(RenderCoverageText(report).c_str(), stdout);

    if (writeMd) {
        if (mdPath.empty()) {
            const size_t slash = positional[0].rfind('/');
            mdPath = (slash == std::string::npos ? std::string() : positional[0].substr(0, slash + 1)) + "NS7-MIDI-MAP.md";
        }
        std::ofstream md(mdPath, std::ios::trunc);
        if (!md) { std::fprintf(stderr, "%s: cannot write\n", mdPath.c_str()); return 2; }
        md << RenderCoverageMarkdown(report, inventory);
        std::printf("Wrote %s\n", mdPath.c_str());
    }
    return report.Complete() ? 0 : 1;
}
