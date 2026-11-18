// SPDX-License-Identifier: AGPL-3.0-only
#include "Processor.h"
#include "TrustedDSP.h"
#include "NativeEditor.h"
#include "ModuleStore.h"
#include <algorithm>
#include <cctype>
#include <cstring>

namespace openvst {
namespace {

ModuleSpec builtinSpec(const ModuleInfo& m, const juce::String& fixturesDir) {
    ModuleSpec s;
    s.id = m.id; s.name = m.name; s.version = m.version;
    s.kind = m.instrument ? "instrument" : "effect";
    s.instrument = m.instrument; s.builtin = true;
    const juce::File root(fixturesDir);
    s.sourcePath = root.getChildFile(m.sourceFile).getFullPathName().toStdString();
    s.manifestPath = root.getChildFile(m.manifestFile).getFullPathName().toStdString();
    s.sourceSHA256 = m.sourceSHA256;
    s.manifestSHA256 = m.manifestSHA256;
    s.mainProcessor = m.mainProcessor != nullptr ? m.mainProcessor : "";
    s.params.reserve((std::size_t)m.paramCount);
    for (int i = 0; i < m.paramCount; ++i) {
        const ParamInfo& p = m.params[i];
        s.params.push_back({p.paramID, p.name, p.endpoint, p.unit, p.min, p.max, p.def});
    }
    return s;
}

// Map a real module value onto the host parameter's range and back. The
// built-in parameters use their real range (so existing automation is
// unchanged); the generic catalogue slots use 0..1.
float toHost(float real, float modMin, float modMax, float hostMin, float hostMax) {
    if (!(modMax > modMin)) return hostMin;
    const float t = (juce::jlimit(modMin, modMax, real) - modMin) / (modMax - modMin);
    return hostMin + t * (hostMax - hostMin);
}
float fromHost(float host, float hostMin, float hostMax, float modMin, float modMax) {
    if (!(hostMax > hostMin)) return modMin;
    const float t = (juce::jlimit(hostMin, hostMax, host) - hostMin) / (hostMax - hostMin);
    return modMin + t * (modMax - modMin);
}

std::array<long, 3> semverCore(const std::string& s) {
    std::array<long, 3> v{};
    std::size_t p = 0;
    for (int i = 0; i < 3 && p < s.size(); ++i) {
        long n = 0;
        while (p < s.size() && std::isdigit((unsigned char)s[p])) { n = n * 10 + (s[p] - '0'); ++p; }
        v[(std::size_t)i] = n;
        if (p < s.size() && s[p] == '.') ++p; else break;
    }
    return v;
}
bool semverGreater(const std::string& a, const std::string& b) {
    const auto x = semverCore(a), y = semverCore(b);
    if (x != y) return x > y;
    return a > b;
}

bool knownTopLevel(const juce::DynamicObject& o, int schema) {
    for (const auto& entry : o.getProperties()) {
        const juce::String k = entry.name.toString();
        const bool common = k == "schema" || k == "variant" || k == "module"
            || k == "version" || k == "sourceSHA256" || k == "engine"
            || k == "restoreValid" || k == "params" || k == "packageSHA256";
        const bool legacy = k == "parameterID" || k == "gain";
        if (!common && !(schema == 1 && legacy)) return false;
        if (k == "packageSHA256" && schema != 3) return false;
        if (legacy && schema != 1) return false;
    }
    return true;
}

} // namespace

Paths Paths::defaults() {
    juce::File store = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                           .getChildFile("OpenVST").getChildFile("modules");
    juce::String url = juce::SystemStats::getEnvironmentVariable("OPENVST_CATALOGUE_URL", {});
    if (url.isEmpty()) url = "http://127.0.0.1:8080";
    return {OPENVST_RUNTIME, OPENVST_FIXTURES, store, url};
}

Processor::Processor(bool s, Paths p)
    : AudioProcessor(s ? BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)
                       : BusesProperties().withInput("Input", juce::AudioChannelSet::stereo(), true)
                                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      instrument(s), paths(std::move(p)) {
    // Built-in fixtures are values, not pointers into a global table: the rest
    // of the processor sees exactly the same ModuleSpec shape as an installed
    // package.
    for (const auto& m : kModules)
        if (m.instrument == instrument) builtins.push_back(builtinSpec(m, paths.fixtures));
    jassert(!builtins.empty());
    if (!builtins.empty()) selectedModule = std::make_shared<ModuleSpec>(builtins.front());

    // Host-facing parameter set: the frozen built-in union, then the permanent
    // generic slots. Declared once and never reshaped: JUCE cannot add or
    // remove parameters at run time. A slot used by an installed module stays
    // present and neutral when the module is not selected.
    hostParamCount = openvst::hostParamCount(instrument);
    for (int i = 0; i < hostParamCount; ++i) {
        const ParamInfo& info = *openvst::hostParam(instrument, i);
        auto* parameter = new juce::AudioParameterFloat(
            juce::ParameterID{info.paramID, i + 1}, info.name,
            juce::NormalisableRange<float>(info.min, info.max), info.def, info.unit);
        addParameter(parameter);
        paramObjects[(std::size_t)i] = parameter;
        paramIDs[(std::size_t)i] = info.paramID;
        if (cstrEq(info.paramID, "openvst.fx.gain.v1")) gainIndex = i;
    }
    refreshInstalledModules();
}

Processor::~Processor() = default;

const juce::String Processor::getName() const { return instrument ? "OpenVST Instrument" : "OpenVST FX"; }

const ModuleSpec& Processor::module() const {
    jassert(selectedModule != nullptr);
    return *selectedModule;
}
juce::String Processor::moduleID() const { return module().id.c_str(); }
juce::String Processor::moduleHash() const { return module().sourceSHA256.c_str(); }
juce::String Processor::moduleVersion() const { return module().version.c_str(); }
juce::String Processor::packageHash() const { return module().packageSHA256.c_str(); }

int Processor::paramCount() const { return hostParamCount; }
const ParamInfo& Processor::paramInfo(int i) const {
    const ParamInfo* p = openvst::hostParam(instrument, i);
    jassert(p != nullptr);
    return *p;
}
float Processor::paramValue(int i) const {
    return (i >= 0 && i < hostParamCount && paramObjects[(std::size_t)i])
               ? paramObjects[(std::size_t)i]->get() : 0.0f;
}
void Processor::setParamValue(int i, float v) {
    if (i < 0 || i >= hostParamCount || !paramObjects[(std::size_t)i] || !std::isfinite(v)) return;
    const ParamInfo& info = *openvst::hostParam(instrument, i);
    auto* p = paramObjects[(std::size_t)i];
    p->setValueNotifyingHost(p->convertTo0to1(juce::jlimit(info.min, info.max, v)));
}
juce::AudioProcessorParameter* Processor::hostParameter(int i) const {
    return (i >= 0 && i < hostParamCount) ? paramObjects[(std::size_t)i] : nullptr;
}

int Processor::moduleParamHostIndex(int i) const {
    const ModuleSpec& m = module();
    if (i < 0 || i >= (int)m.params.size()) return -1;
    if (m.builtin) return unionParamIndex(instrument, m.params[i].id.c_str());
    if (i >= kGenericSlots) return -1;
    return unionParamCount(instrument) + i;
}

float Processor::moduleParamValue(int i) const {
    const ModuleSpec& m = module();
    if (i < 0 || i >= (int)m.params.size()) return 0.0f;
    const int u = moduleParamHostIndex(i);
    if (u < 0 || !paramObjects[(std::size_t)u]) return m.params[i].def;
    auto* p = paramObjects[(std::size_t)u];
    const auto range = p->getNormalisableRange();
    return fromHost(p->get(), range.start, range.end, m.params[i].min, m.params[i].max);
}

void Processor::setModuleParamValue(int i, float value) {
    const ModuleSpec& m = module();
    if (i < 0 || i >= (int)m.params.size() || !std::isfinite(value)) return;
    const int u = moduleParamHostIndex(i);
    if (u < 0 || !paramObjects[(std::size_t)u]) return;
    auto* p = paramObjects[(std::size_t)u];
    const auto range = p->getNormalisableRange();
    const float hostValue = toHost(value, m.params[i].min, m.params[i].max, range.start, range.end);
    p->setValueNotifyingHost(p->convertTo0to1(juce::jlimit(range.start, range.end, hostValue)));
}

float Processor::gain() const { return gainIndex >= 0 ? paramObjects[(std::size_t)gainIndex]->get() : 1.0f; }
void Processor::setGain(float value) {
    if (gainIndex < 0 || !std::isfinite(value)) return;
    setParamValue(gainIndex, value);
}

void Processor::resetParamsToDefaults() {
    for (int i = 0; i < hostParamCount; ++i) {
        const ParamInfo& info = *openvst::hostParam(instrument, i);
        auto* p = paramObjects[(std::size_t)i];
        if (p) p->setValueNotifyingHost(p->convertTo0to1(info.def));
    }
    const ModuleSpec& m = module();
    for (int i = 0; i < (int)m.params.size(); ++i)
        setModuleParamValue(i, m.params[i].def);
}

const ModuleSpec* Processor::findSpec(const juce::String& id, const juce::String& hash) const {
    const ModuleSpec* best = nullptr;
    const auto consider = [&](const std::vector<ModuleSpec>& list) {
        for (const auto& m : list) {
            if (m.instrument != instrument) continue;
            if (id != juce::String(m.id.c_str())) continue;
            if (hash.isNotEmpty() && hash != juce::String(m.sourceSHA256.c_str())) continue;
            if (best == nullptr || semverGreater(m.version, best->version)) best = &m;
        }
    };
    consider(builtins);
    consider(installed);
    return best;
}

bool Processor::setModuleID(const juce::String& id) { return setModule(id, {}); }

bool Processor::setModule(const juce::String& id, const juce::String& sourceSHA256) {
    // Unknown or wrong-kind id: refused explicitly, never silently substituted.
    const ModuleSpec* m = findSpec(id, sourceSHA256);
    if (m == nullptr || m->instrument != instrument) return false;
    if (selectedModule && selectedModule->id == m->id
        && selectedModule->sourceSHA256 == m->sourceSHA256) return true;
    selectedModule = std::make_shared<ModuleSpec>(*m);
    // The newly selected module starts from its own declared defaults; stale
    // values of a previous selection must not leak into it.
    resetParamsToDefaults();
    prepared.store(false);
    fault.store(1); // compiled at the next prepareToPlay
    return true;
}

void Processor::setInstalledModules(std::vector<ModuleSpec> modules) {
    std::vector<ModuleSpec> accepted;
    for (auto& m : modules) {
        if (m.instrument != instrument) continue;
        if (m.id.empty() || m.sourceSHA256.empty() || m.sourcePath.empty()) continue;
        if (m.params.size() > (std::size_t)kGenericSlots) continue;
        accepted.push_back(std::move(m));
    }
    installed = std::move(accepted);
}

bool Processor::refreshInstalledModules() {
    // Message-thread only. The installer calls this after publishing a new
    // package; selection and preparation stay explicit, and the audio callback
    // never scans a directory.
    std::vector<ModuleSpec> specs;
    if (paths.moduleStore.isDirectory()) {
        ModuleStore store(paths.moduleStore);
        const auto found = store.scan();
        specs.reserve(found.size());
        for (const auto& m : found) {
            const bool isInstrument = m.package.kind == "instrument";
            if (isInstrument != instrument) continue;
            if (m.package.params.size() > (std::size_t)kGenericSlots) continue;
            ModuleSpec s;
            s.id = m.package.id.toStdString();
            s.name = m.package.name.toStdString();
            s.version = m.package.version.toStdString();
            s.kind = m.package.kind.toStdString();
            s.instrument = isInstrument;
            s.builtin = false;
            s.sourcePath = m.sourceFile.getFullPathName().toStdString();
            s.sourceSHA256 = m.package.sourceSHA256.toStdString();
            s.manifestPath = m.manifestFile.getFullPathName().toStdString();
            s.manifestSHA256 = m.package.manifestSHA256.toStdString();
            s.packageSHA256 = m.package.packageSHA256.toStdString();
            if (m.uiFile.existsAsFile()) s.uiPath = m.uiFile.getFullPathName().toStdString();
            s.mainProcessor = m.package.mainProcessor.toStdString();
            for (const auto& p : m.package.params)
                s.params.push_back({p.id.toStdString(), p.name.toStdString(), p.endpoint.toStdString(),
                                    p.unit.toStdString(), p.min, p.max, p.def});
            specs.push_back(std::move(s));
        }
    }
    const auto same = [](const ModuleSpec& a, const ModuleSpec& b) {
        return a.id == b.id && a.sourceSHA256 == b.sourceSHA256 && a.version == b.version;
    };
    const bool changed = specs.size() != installed.size()
        || !std::equal(specs.begin(), specs.end(), installed.begin(), same);
    installed = std::move(specs);
    return changed;
}

void Processor::reprepare() {
    const double rate = preparedRate.load(std::memory_order_relaxed);
    if (rate > 0) prepareToPlay(rate, 512);
}

void Processor::prepareToPlay(double rate, int) {
    // Prototype lifecycle: host must serialise prepare/release/reset against audio.
    // Compilation happens ONLY here, never in state setter or processBlock.
    preparedRate.store(rate);
    prepared.store(false);
    std::atomic_store(&dsp, std::shared_ptr<DSP>{});
    if (fault.load() == 3) return; // A rejected restore is sticky until a valid restore.
    fault.store(1);
    // Build first, publish after: a failed compilation never replaces a working
    // performer, and the old object is destroyed only when the audio thread is
    // done with it.
    try {
        auto selected = std::atomic_load(&selectedModule);
        trusted::require(selected != nullptr, "No selected module");
        const ModuleSpec& m = *selected;
        trusted::require(m.params.size() <= (std::size_t)kMaxParamsPerModule, "Module parameter capacity exceeded");
        // Resolve module parameter -> host parameter and both ranges once, on
        // this (non-audio) thread; the DSP then never compares strings.
        std::array<juce::AudioParameterFloat*, kMaxParamsPerModule> host{};
        std::array<float, kMaxParamsPerModule> hostMin{}, hostMax{};
        for (int i = 0; i < (int)m.params.size(); ++i) {
            const int u = moduleParamHostIndex(i);
            trusted::require(u >= 0 && paramObjects[(std::size_t)u], "Module parameter missing from the host union: " + m.params[(std::size_t)i].id);
            host[(std::size_t)i] = paramObjects[(std::size_t)u];
            const auto range = host[(std::size_t)i]->getNormalisableRange();
            hostMin[(std::size_t)i] = range.start;
            hostMax[(std::size_t)i] = range.end;
        }
        std::shared_ptr<DSP> fresh = std::make_shared<DSP>(m, paths.runtime, rate, host, hostMin, hostMax);
        std::atomic_store(&dsp, std::move(fresh));
        prepared.store(true);
        fault.store(0);
    } catch (const std::exception& e) {
        juce::ScopedLock lock(errorLock);
        preparationError = e.what();
        fault.store(2);
    }
}

void Processor::releaseResources() {
    prepared.store(false);
    std::atomic_store(&dsp, std::shared_ptr<DSP>{});
    if (fault.load() != 3) fault.store(1);
}

void Processor::reset() {
    auto d = std::atomic_load(&dsp);
    if (d && d->performer.reset() != cmaj::Result::Ok) fault.store(4);
}

bool Processor::ready() const { return fault.load() == 0; }

juce::String Processor::status() const {
    switch (fault.load()) {
        case 0: return "Ready - " + moduleID();
        case 1: return "Not prepared / released";
        case 2: { juce::ScopedLock lock(errorLock); return "Preparation failed: " + preparationError; }
        case 3: return "State rejected: schema/module/version/hash/parameter mismatch; restore NOT successful";
        default: return "Cmajor runtime error; fallback active";
    }
}

bool Processor::isBusesLayoutSupported(const BusesLayout& l) const {
    return l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo()
        && l.getMainInputChannelSet() == (instrument ? juce::AudioChannelSet::disabled()
                                                     : juce::AudioChannelSet::stereo());
}

juce::AudioProcessorEditor* Processor::createEditor() { return new NativeEditor(*this); }

void Processor::getStateInformation(juce::MemoryBlock& dest) {
    const ModuleSpec& m = module();
    auto* o = new juce::DynamicObject;
    // Schema 3 adds the exact package digest. Schema 2 (built-in module and
    // its parameter values) and schema 1 (gain only) remain readable; they
    // never gain new writers.
    o->setProperty("schema", 3);
    o->setProperty("variant", instrument ? "instrument" : "fx");
    o->setProperty("module", m.id.c_str());
    o->setProperty("version", m.version.c_str());
    o->setProperty("sourceSHA256", m.sourceSHA256.c_str());
    o->setProperty("packageSHA256", m.packageSHA256.c_str());
    o->setProperty("engine", "cmajor-1.0.3209");
    o->setProperty("restoreValid", fault.load() != 3);
    auto* params = new juce::DynamicObject;
    for (int i = 0; i < (int)m.params.size(); ++i)
        params->setProperty(m.params[(std::size_t)i].id.c_str(), moduleParamValue(i));
    o->setProperty("params", juce::var(params));
    const auto text = juce::JSON::toString(juce::var(o), true);
    dest.replaceAll(text.toRawUTF8(), text.getNumBytesAsUTF8());
}

void Processor::setStateInformation(const void* data, int size) {
    // Fixed schemas only. Does not touch performer or filesystem. JSON parsing may
    // allocate: hosts must not call this on their real-time callback in this slice.
    if (!data || size < 2 || size > 8192) { fault.store(3); return; }
    juce::var v;
    const auto parsed = juce::JSON::parse(juce::String::fromUTF8((const char*)data, size), v);
    auto* o = v.getDynamicObject();
    const auto eq = [&](const char* key, const juce::String& expected) {
        return v[key].isString() && v[key].toString() == expected;
    };
    const auto moduleVar = v["module"];
    const bool hasModule = moduleVar.isString();
    const bool moduleAbsent = moduleVar.isVoid();
    const int schema = (v["schema"].isInt() || v["schema"].isDouble()) ? (int)v["schema"] : 0;
    bool valid = parsed.wasOk() && o != nullptr && schema >= 1 && schema <= 3
        && eq("variant", instrument ? "instrument" : "fx")
        && eq("engine", "cmajor-1.0.3209")
        && v["restoreValid"].isBool() && (bool)v["restoreValid"]
        && knownTopLevel(*o, schema);

    // Selection comes from the state itself. A state with no module field is a
    // legacy state: it restores to the default module of this build. A state
    // with an unknown or wrong-kind module id is refused, never substituted.
    const ModuleSpec* m = nullptr;
    if (valid && schema == 3) {
        const auto versionVar = v["version"], hashVar = v["sourceSHA256"], pkgVar = v["packageSHA256"];
        if (!hasModule || !versionVar.isString() || !hashVar.isString() || !pkgVar.isString()) {
            valid = false;
        } else {
            m = findSpec(moduleVar.toString(), hashVar.toString());
            valid = m != nullptr
                && juce::String(m->version.c_str()) == versionVar.toString()
                && juce::String(m->sourceSHA256.c_str()) == hashVar.toString()
                && juce::String(m->packageSHA256.c_str()) == pkgVar.toString();
        }
    } else if (valid && schema == 2) {
        if (!hasModule) {
            valid = false;
        } else {
            m = findSpec(moduleVar.toString(), {});
            valid = m != nullptr
                && eq("version", juce::String(m->version.c_str()))
                && eq("sourceSHA256", juce::String(m->sourceSHA256.c_str()));
        }
    } else if (valid) {
        if (moduleAbsent) {
            m = instrument ? findSpec(builtins.front().id.c_str(), {}) : findSpec(builtins.front().id.c_str(), {});
        } else if (hasModule) {
            m = findSpec(moduleVar.toString(), {});
        }
        valid = m != nullptr
            && eq("version", juce::String(m->version.c_str()))
            && eq("sourceSHA256", juce::String(m->sourceSHA256.c_str()));
    }

    // Parameter values: schema 2/3 carry a map of paramID -> real value for
    // the active module. Anything unknown, non-numeric, non-finite or out of
    // the module's declared range is refused, never corrected in silence.
    juce::DynamicObject* params = nullptr;
    if (valid && (schema == 2 || schema == 3)) {
        const auto paramsVar = v["params"];
        if (paramsVar.isVoid()) {
            // absent: every parameter takes its default
        } else if ((params = paramsVar.getDynamicObject()) == nullptr) {
            valid = false;
        } else {
            if (params->getProperties().size() > (int)m->params.size()) valid = false;
            for (const auto& entry : params->getProperties()) {
                const juce::String key = entry.name.toString();
                const ParamSpec* known = nullptr;
                for (const auto& p : m->params) if (key == juce::String(p.id.c_str())) { known = &p; break; }
                const auto& value = entry.value;
                if (known == nullptr || !(value.isDouble() || value.isInt())
                    || !std::isfinite((double)value)
                    || (double)value < (double)known->min || (double)value > (double)known->max) {
                    valid = false;
                    break;
                }
            }
        }
    }
    const auto g = v["gain"];
    if (valid && schema == 1) {
        // Legacy schema: exactly the old document, gain in the legacy 0..2 range.
        valid = o->getProperties().size() == (hasModule ? 9 : 8)
            && eq("parameterID", instrument ? "" : "openvst.fx.gain.v1")
            && (g.isDouble() || g.isInt()) && std::isfinite((double)g)
            && (double)g >= 0 && (double)g <= 2 && (!instrument || (double)g == 1);
    }
    if (!valid || m == nullptr) { fault.store(3); return; }

    const std::string previousID = selectedModule ? selectedModule->id : std::string();
    const std::string previousHash = selectedModule ? selectedModule->sourceSHA256 : std::string();
    const bool wasPrepared = prepared.load();
    selectedModule = std::make_shared<ModuleSpec>(*m);
    // Everything not stated by the document falls back to its declared default,
    // so an old single-gain state restores a clean per-module parameter set.
    resetParamsToDefaults();
    if (schema == 1) {
        setGain((float)(double)g); // schema 1: the gain value is the only stated value
    } else if (params != nullptr) {
        for (const auto& entry : params->getProperties()) {
            const juce::String key = entry.name.toString();
            for (int i = 0; i < (int)m->params.size(); ++i)
                if (key == juce::String(m->params[(std::size_t)i].id.c_str()))
                    setModuleParamValue(i, (float)(double)entry.value);
        }
    }
    const bool changed = previousID != m->id || previousHash != m->sourceSHA256;
    if (changed && wasPrepared) { prepared.store(false); fault.store(1); } // recompile at next prepare
    else fault.store(wasPrepared ? 0 : 1);
}

void Processor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
    juce::ScopedNoDenormals noDenormals;
    const int frames = buffer.getNumSamples();
    // One reference for the whole block: a concurrent module swap cannot free the
    // DSP while this callback is using it.
    auto held = std::atomic_load(&dsp);
    // A refused restore must never look like a working plugin: mute instead of
    // passing unprocessed audio through. Before the first prepare the FX stays
    // transparent, so a host that probes it gets its own signal back.
    if (fault.load() == 3) { buffer.clear(); midi.clear(); return; }
    if (!ready() || !held || buffer.getNumChannels() != 2) { if (instrument) buffer.clear(); midi.clear(); return; }
    if (frames == 0) { midi.clear(); return; }
    auto& d = *held; auto& p = d.performer;
    auto ok = [](cmaj::Result r) { return r == cmaj::Result::Ok; };
    auto fail = [&] { fault.store(4); if (instrument) buffer.clear(); midi.clear(); };
    // Every parameter of the active module is sent at the top of the call;
    // parameters of inactive modules are never sent (they are present for the
    // host but have no effect). get() reads the host value from an atomic, then
    // the fixed range map is allocation-free and lock-free.
    for (int i = 0; i < d.paramCount; ++i) {
        const float hostValue = d.hostParams[(std::size_t)i]->get();
        const float real = fromHost(hostValue, d.hostMin[(std::size_t)i], d.hostMax[(std::size_t)i],
                                    d.paramMin[(std::size_t)i], d.paramMax[(std::size_t)i]);
        if (!ok(p.addInputEvent(d.paramEvents[(std::size_t)i], 0, real))) { fail(); return; }
    }
    auto it = midi.cbegin(), last = midi.cend();
    int pos = 0;
    while (pos < frames) {
        if (instrument) while (it != last && (*it).samplePosition <= pos) {
            const auto e = *it; ++it;
            // This fixture implements channel-1 note on/off only; ignore SysEx,
            // transport and other non-note messages, without constructing Value.
            if (e.samplePosition < 0 || e.numBytes != 3 || ((e.data[0] & 0xf0) != 0x90 && (e.data[0] & 0xf0) != 0x80)) continue;
            const int32_t packed = (int32_t(e.data[0]) << 16) | (int32_t(e.data[1]) << 8) | int32_t(e.data[2]);
            // Validated MIDI object has exactly one int32 member. Raw payload per
            // tag API avoids allocating a choc::value::Value on the audio thread.
            if (!ok(p.addInputEvent(d.midiEvent, 0, static_cast<const void*>(&packed)))) { fail(); return; }
        }
        int end = std::min(frames, pos + 512);
        if (instrument && it != last) end = std::min(end, (*it).samplePosition);
        const int n = end - pos;
        if (n <= 0 || !ok(p.setBlockSize((uint32_t)n))) { fail(); return; }
        if (!instrument) {
            for (int i = 0; i < n; ++i) { d.in[2 * i] = buffer.getSample(0, pos + i); d.in[2 * i + 1] = buffer.getSample(1, pos + i); }
            if (!ok(p.setInputFrames(d.input, d.in.data(), (uint32_t)n))) { fail(); return; }
        }
        if (!ok(p.advance()) || !ok(p.copyOutputFrames(d.output, d.out.data(), (uint32_t)n))) { fail(); return; }
        const char* error = p.getRuntimeError();
        if ((error && *error) || p.getXRuns() != 0) { fail(); return; }
        for (int i = 0; i < 2 * n; ++i) if (!std::isfinite(d.out[i])) { fail(); return; }
        for (int i = 0; i < n; ++i) { buffer.setSample(0, pos + i, d.out[2 * i]); buffer.setSample(1, pos + i, d.out[2 * i + 1]); }
        pos = end;
    }
    midi.clear();
}
} // namespace openvst
