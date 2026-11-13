// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <memory>
#include "ModuleRegistry.h"
namespace openvst {
struct Paths { juce::String runtime, fixtures; static Paths defaults(); };
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
    // Static parameter union of this build's kind. Positions are frozen:
    // parameter i always means the same automation ID for the process lifetime.
    int paramCount() const;
    const ParamInfo& paramInfo(int i) const;
    float paramValue(int i) const;
    void setParamValue(int i, float v); // clamped to the parameter's table range
    // Editor-facing helpers: the active module's parameter i, mapped onto the
    // static union (hostParameter returns nullptr when out of range).
    juce::AudioProcessorParameter* hostParameter(int unionIndex) const;
    int moduleParamHostIndex(int moduleParamIndex) const;
    float moduleParamValue(int moduleParamIndex) const;
    juce::String moduleID() const;
    juce::String moduleHash() const;
    juce::String moduleVersion() const;
    const ModuleInfo& module() const;
    // Selection is validated against the static registry: an unknown id or a
    // wrong-kind id is refused and the current selection is kept. Takes effect
    // at the next prepareToPlay, never inside the audio callback.
    bool setModuleID(const juce::String& id);
    void reprepare(); // message-thread recompilation for editor-driven changes
private:
    struct DSP;
    const bool instrument;
    const Paths paths;
    // Fixed-size host parameter union; filled once in the constructor, in
    // registry order. idChars caches the table's C string for allocation-free
    // audio-thread lookup. gainIndex is the union position of the legacy gain
    // ID, or -1 when this kind has no such parameter.
    static constexpr int kMaxParams = kMaxParamsPerModule;
    std::array<juce::AudioParameterFloat*, kMaxParams> paramObjects{};
    std::array<const char*, kMaxParams> paramIDs{};
    int hostParamCount = 0;
    int gainIndex = -1;
    std::atomic<const ModuleInfo*> selectedModule;
    std::atomic<double> preparedRate{0};
    // Shared, not unique: the audio thread takes a reference for the duration of
    // a block, so a module change on the message thread cannot free the DSP under
    // it. Access only through std::atomic_load / std::atomic_store.
    std::shared_ptr<DSP> dsp;
    std::atomic<bool> prepared{false};
    std::atomic<int> fault{1}; // 0 ready, 1 unprepared, 2 preparation, 3 state, 4 runtime
    mutable juce::CriticalSection errorLock;
    juce::String preparationError;
    void resetParamsToDefaults();
};
}
