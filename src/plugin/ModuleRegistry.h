// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <juce_core/juce_core.h>
namespace openvst {

// Declarative, static, allowlisted module catalogue. No folder scan, no
// dynamic loading: a module absent from this table does not exist for the
// container. Entries are frozen by hash; a changed source is a new entry.
struct ModuleInfo {
    const char* id;            // stable identity, never changes
    const char* name;          // display name, ASCII only
    const char* version;       // immutable module version
    const char* sourceFile;    // relative to the trusted fixture root
    const char* manifestFile;
    const char* manifestSHA256;
    const char* sourceSHA256;
    bool instrument;           // kind: instrument vs effect
    const char* mainProcessor; // Cmajor main processor name
    bool hasGainEndpoint;      // exposes the container gain event (effects)
};

inline constexpr ModuleInfo kModules[] = {
    {"org.openvst.spike.stereo-gain", "Stereo Gain", "0.0.1",
     "stereo_gain.cmajor", "stereo_gain.cmajorpatch",
     "906be1bc1a32331428f7e58ac8811559aa6ddc77c23a8ae8aa714951218e224f",
     "36b6af6e18af5658a21515d1395cb713c548f7b5be14cd78c1c23e9906200309",
     false, "StereoGain", true},
    {"org.openvst.spike.note-grid", "Note Grid", "0.0.1",
     "note_grid.cmajor", "note_grid.cmajorpatch",
     "84c27d2ad9c6a16948d99f7b830f1e3e6a8f269da6c49e8c29bac10ce8f391ea",
     "cf8940610b1ecd410de5998e006e5e69d647f702b29690e4bc670fd4daa499fe",
     true, "NoteGrid", false},
};

inline const ModuleInfo* findModule(const juce::String& id) {
    for (const auto& m : kModules) if (id == m.id) return &m;
    return nullptr;
}

inline const ModuleInfo* defaultModule(bool instrument) {
    for (const auto& m : kModules) if (m.instrument == instrument) return &m;
    return nullptr;
}

inline constexpr bool registrySane() {
    bool fx = false, ins = false;
    for (const auto& m : kModules) (m.instrument ? ins : fx) = true;
    return fx && ins;
}
static_assert(registrySane(), "module registry must cover both kinds");
}
