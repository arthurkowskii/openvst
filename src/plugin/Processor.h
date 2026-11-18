// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <memory>
#include <vector>
#include "ModuleRegistry.h"
#include "ModuleSpec.h"

namespace openvst {
struct Paths {
    juce::String runtime, fixtures;
    juce::File moduleStore;      // empty disables catalogue-installed modules
    juce::String catalogueURL;   // empty disables the catalogue client
    static Paths defaults();
};

class Processor final : public juce::AudioProcessor {
public:
    explicit Processor(bool instrument, Paths = Paths::defaults());
    ~Processor() override;
    const juce::String getName() const override;
    void prepareToPlay(double, int) override;
    void releaseResources() override;
    void reset() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    bool isBusesLayoutSupported(const BusesLayout&) const override;
    bool acceptsMidi() const override { return instrument; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    bool supportsDoublePrecisionProcessing() const override { return false; }
    double getTailLengthSeconds() const override { return instrument ? 0.01 : 0.0; }
    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;
    juce::String status() const;
    bool ready() const;
    void setGain(float);
    float gain() const;
    // Host-facing parameter set: the built-in union followed by the permanent
    // generic slots. Positions are frozen for the process lifetime.
    int paramCount() const;
    const ParamInfo& paramInfo(int i) const;
    float paramValue(int i) const; // host value: real for built-ins, 0..1 for slots
    void setParamValue(int i, float v); // clamped to the host parameter's range
    juce::AudioProcessorParameter* hostParameter(int unionIndex) const;
    // Editor/state helpers: real module values, independent of the host range.
    int moduleParamHostIndex(int moduleParamIndex) const;
    float moduleParamValue(int moduleParamIndex) const;
    void setModuleParamValue(int moduleParamIndex, float value);
    juce::String moduleID() const;
    juce::String moduleHash() const;
    juce::String moduleVersion() const;
    juce::String packageHash() const;
    const ModuleSpec& module() const;
    const Paths& processorPaths() const { return paths; }
    const std::vector<ModuleSpec>& builtinModules() const { return builtins; }
    const std::vector<ModuleSpec>& installedModules() const { return installed; }
    // Re-scans the module store. Message thread only; audio is untouched until
    // the next prepareToPlay. Returns true when the installed set changed.
    bool refreshInstalledModules();
    // Used by tests and by the installer after a store scan. Takes effect on
    // the next selection; does not compile anything.
    void setInstalledModules(std::vector<ModuleSpec> modules);
    // Selection is validated against built-ins and installed modules: an
    // unknown id or wrong-kind id is refused and the current selection is
    // kept. With an empty hash, the highest version of that id wins. Takes
    // effect at the next prepareToPlay, never inside the audio callback.
    bool setModuleID(const juce::String& id);
    bool setModule(const juce::String& id, const juce::String& sourceSHA256);
    void reprepare(); // message-thread recompilation for editor-driven changes
private:
    struct DSP;
    const bool instrument;
    const Paths paths;
    // Host parameter array: built-in union then generic slots.
    std::array<juce::AudioParameterFloat*, kMaxHostParams> paramObjects{};
    std::array<const char*, kMaxHostParams> paramIDs{};
    int hostParamCount = 0;
    int gainIndex = -1;
    std::vector<ModuleSpec> builtins;
    std::vector<ModuleSpec> installed;
    // The selected module is an immutable value object. The audio thread does
    // not read it at all: preparation resolves everything into the DSP.
    std::shared_ptr<const ModuleSpec> selectedModule;
    std::atomic<double> preparedRate{0};
    // Shared, not unique: the audio thread takes a reference for the duration of
    // a block, so a module change on the message thread cannot free the DSP under
    // it. Access only through std::atomic_load / std::atomic_store.
    std::shared_ptr<DSP> dsp;
    std::atomic<bool> prepared{false};
    std::atomic<int> fault{1}; // 0 ready, 1 unprepared, 2 preparation, 3 state, 4 runtime
    mutable juce::CriticalSection errorLock;
    juce::String preparationError;
    const ModuleSpec* findSpec(const juce::String& id, const juce::String& hash) const;
    void resetParamsToDefaults();
};
} // namespace openvst
