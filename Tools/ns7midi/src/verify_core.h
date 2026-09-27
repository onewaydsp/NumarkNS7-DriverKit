// verify_core.h
// Coverage logic for `ns7midi verify`: which inventory items have a verified
// input and LED, and which observed messages or lit LEDs belong to nothing.
// Pure code, tested in tests/.

#pragma once

#include "json.h"

#include <string>
#include <vector>

struct ItemCoverage {
    std::string id, section, label, kind;
    bool needsInput = false;
    bool inputVerified = false;
    std::string inputState;     // verified / skipped / empty / timeout / not learned / n/a
    std::string inputText;      // learned keys, or the prior when unverified
    bool needsLed = false;
    bool ledVerified = false;
    std::string ledText;        // "B0 09 (ledscan)" etc.
    bool uncertain = false;
};

struct CoverageReport {
    std::vector<ItemCoverage> items;
    std::vector<std::string> unassignedInputs;   // "B0:24 (x12, learn)" etc.
    std::vector<std::string> unassignedLeds;     // ledscan results that lit something unknown
    std::vector<std::string> duplicateKeys;      // one input key verified on two controls
    size_t inputsNeeded = 0, inputsVerified = 0;
    size_t ledsNeeded = 0, ledsVerified = 0;
    bool Complete() const
    {
        return inputsVerified == inputsNeeded && ledsVerified == ledsNeeded && unassignedInputs.empty();
    }
};

// learned and ledmap may be null Json values when not available.
CoverageReport BuildCoverage(const Json & inventory, const Json & learned, const Json & ledmap);

std::string RenderCoverageText(const CoverageReport & report);
std::string RenderCoverageMarkdown(const CoverageReport & report, const Json & inventory);
