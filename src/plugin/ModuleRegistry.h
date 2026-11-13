// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <juce_core/juce_core.h>
namespace openvst {

// Declarative, static, allowlisted module catalogue. No folder scan, no
// dynamic loading: a module absent from this table does not exist for the
// container. Entries are frozen by hash; a changed source is a new entry.
struct ParamInfo {
    const char* paramID;   // stable automation identity, never changes
    const char* name;      // display name, ASCII only
    const char* endpoint;  // Cmajor event endpoint the value is written to
    float min;
    float max;
    float def;
    const char* unit;      // "" when none; ASCII only
};

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
    const ParamInfo* params;   // per-module parameter table
    int paramCount;
};

// Capacity shared with the DSP parameter plumbing; the whole union of one
// build's kind must fit.
inline constexpr int kMaxParamsPerModule = 8;

inline constexpr bool cstrEq(const char* a, const char* b) {
    while (*a != '\0' && *b != '\0') if (*a++ != *b++) return false;
    return *a == *b;
}

inline constexpr ParamInfo kStereoGainParams[] = {
    // This automation ID is permanent: existing projects address it. Its
    // position as the first FX union parameter is also frozen.
    {"openvst.fx.gain.v1", "Gain", "gain", 0.0f, 2.0f, 1.0f, ""},
};
inline constexpr ParamInfo kTransientShaperParams[] = {
    {"openvst.ts.attack.v1",  "Attack",  "attack",  -12.0f, 12.0f, 0.0f, "dB"},
    {"openvst.ts.sustain.v1", "Sustain", "sustain", -12.0f, 12.0f, 0.0f, "dB"},
    {"openvst.ts.gain.v1",    "Output gain", "gain", 0.0f, 2.0f, 1.0f, ""},
};

inline constexpr ModuleInfo kModules[] = {
    {"org.openvst.spike.stereo-gain", "Stereo Gain", "0.0.1",
     "stereo_gain.cmajor", "stereo_gain.cmajorpatch",
     "906be1bc1a32331428f7e58ac8811559aa6ddc77c23a8ae8aa714951218e224f",
     "36b6af6e18af5658a21515d1395cb713c548f7b5be14cd78c1c23e9906200309",
     false, "StereoGain", kStereoGainParams, 1},
    {"org.openvst.spike.transient-shaper", "Transient Shaper", "0.0.1",
     "transient_shaper.cmajor", "transient_shaper.cmajorpatch",
     "d87cd4f2f719d2109adb53c3b398757f39d1a17acd258172c74201ac2696b428",
     "27ee6305dba4fc46cf011298debe7889ae93d707221d4f6fc626006d3c6d8c32",
     false, "TransientShaper", kTransientShaperParams, 3},
    {"org.openvst.spike.note-grid", "Note Grid", "0.0.1",
     "note_grid.cmajor", "note_grid.cmajorpatch",
     "84c27d2ad9c6a16948d99f7b830f1e3e6a8f269da6c49e8c29bac10ce8f391ea",
     "cf8940610b1ecd410de5998e006e5e69d647f702b29690e4bc670fd4daa499fe",
     true, "NoteGrid", nullptr, 0},
};

inline const ModuleInfo* findModule(const juce::String& id) {
    for (const auto& m : kModules) if (id == m.id) return &m;
    return nullptr;
}

inline const ModuleInfo* defaultModule(bool instrument) {
    for (const auto& m : kModules) if (m.instrument == instrument) return &m;
    return nullptr;
}

// The host-facing parameter set is the static union of every module of this
// build's kind, in registry order, declared once at construction. JUCE cannot
// add or remove parameters at run time, so the union never changes shape: a
// parameter of an inactive module stays present and simply has no effect.
inline constexpr int unionParamCount(bool instrument) {
    int n = 0;
    for (const auto& m : kModules) if (m.instrument == instrument) n += m.paramCount;
    return n;
}

inline constexpr const ParamInfo* unionParam(bool instrument, int index) {
    int n = 0;
    for (const auto& m : kModules) if (m.instrument == instrument)
        for (int i = 0; i < m.paramCount; ++i, ++n) if (n == index) return &m.params[i];
    return nullptr;
}

// Union position of a parameter ID, or -1. Both arguments are raw C strings
// from this table, so comparison never allocates (audio-thread safe).
inline constexpr int unionParamIndex(bool instrument, const char* paramID) {
    for (int i = 0; i < unionParamCount(instrument); ++i)
        if (cstrEq(unionParam(instrument, i)->paramID, paramID)) return i;
    return -1;
}

inline constexpr bool paramsSane() {
    for (const auto& m : kModules) {
        if (m.paramCount < 0 || m.paramCount > kMaxParamsPerModule) return false;
        if (m.paramCount > 0 && m.params == nullptr) return false;
        for (int i = 0; i < m.paramCount; ++i) {
            const auto& p = m.params[i];
            if (p.paramID == nullptr || p.name == nullptr || p.endpoint == nullptr || p.unit == nullptr) return false;
            if (!(p.min <= p.def && p.def <= p.max)) return false;
            for (int j = 0; j < i; ++j)
                if (cstrEq(p.paramID, m.params[j].paramID) || cstrEq(p.endpoint, m.params[j].endpoint)) return false;
        }
    }
    // Automation IDs are permanent: the same ID must never be declared twice,
    // or two modules could fight over one host automation lane.
    for (int a = 0; a < 2; ++a)
        for (int i = 0; i < unionParamCount(a != 0); ++i)
            for (int j = 0; j < i; ++j)
                if (cstrEq(unionParam(a != 0, i)->paramID, unionParam(a != 0, j)->paramID)) return false;
    return true;
}

inline constexpr bool registrySane() {
    bool fx = false, ins = false;
    for (const auto& m : kModules) (m.instrument ? ins : fx) = true;
    return fx && ins;
}
static_assert(registrySane(), "module registry must cover both kinds");
static_assert(paramsSane(), "parameter tables must be unique, bounded and ordered");
static_assert(unionParamCount(false) <= kMaxParamsPerModule && unionParamCount(true) <= kMaxParamsPerModule,
              "the static union of one kind must fit the fixed DSP plumbing");
static_assert(unionParamIndex(false, "openvst.fx.gain.v1") == 0,
              "the legacy FX gain automation ID stays at union position 0");
}
