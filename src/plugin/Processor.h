// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <memory>
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
private:
    struct DSP;
    const bool instrument;
    const Paths paths;
    juce::AudioParameterFloat* gainParameter = nullptr;
    std::unique_ptr<DSP> dsp;
    std::atomic<bool> prepared{false};
    std::atomic<int> fault{1}; // 0 ready, 1 unprepared, 2 preparation, 3 state, 4 runtime
    mutable juce::CriticalSection errorLock;
    juce::String preparationError;
};
}
