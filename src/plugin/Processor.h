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
    juce::AudioParameterFloat* gainParameter = nullptr;
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
};
}
