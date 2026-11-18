// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <string>
#include <vector>

namespace openvst {

// Runtime view of one module. Built-in fixtures and catalogue-installed
// modules both produce this shape, so the audio path has no special case for
// either origin. Strings are owned: an installed module can be removed from
// the store only after the processor released its reference, but the DSP must
// never depend on a file being present after preparation.
struct ParamSpec {
    std::string id;        // permanent automation identity inside the module
    std::string name;      // display name, ASCII only
    std::string endpoint;  // Cmajor event endpoint
    std::string unit;      // ASCII, empty when none
    float min = 0.0f, max = 1.0f, def = 0.0f;
};

struct ModuleSpec {
    std::string id, name, version;
    std::string kind;          // "effect" or "instrument"
    bool instrument = false;
    bool builtin = true;       // false for a catalogue-installed module
    std::string sourcePath;    // absolute path to the .cmajor source
    std::string sourceSHA256;  // digest of the exact source bytes
    std::string manifestPath;  // absolute path to the hashed manifest
    std::string manifestSHA256;
    std::string packageSHA256; // exact package digest; empty for built-ins
    std::string uiPath;        // absolute ui.json path; empty when none
    std::string mainProcessor; // Cmajor main processor name
    std::vector<ParamSpec> params;
};

} // namespace openvst
