// Checks for the original trusted OpenVST proof modules, not arbitrary plugins.
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <cmath>
#include <stdexcept>

namespace fixture_checks
{
inline void require (bool condition, const char* message)
{
    if (! condition) throw std::runtime_error (message);
}

inline juce::var effect (juce::AudioPluginInstance& plugin)
{
    require (plugin.getTotalNumInputChannels() == 2 && plugin.getTotalNumOutputChannels() == 2,
             "FX must have stereo input/output");
    juce::AudioProcessorParameter* gain = nullptr;
    for (auto parameter : plugin.getParameters())
        if (parameter->getName (128).equalsIgnoreCase ("Gain")) gain = parameter;
    require (gain != nullptr, "Gain parameter not exposed through VST3");
    juce::MemoryBlock saved;
    gain->setValueNotifyingHost (0.25f); // Trusted fixture range [0,2], hence linear 0.5.
    plugin.getStateInformation (saved);
    require (saved.getSize() > 0, "empty state");
    double maximumError = 0;
    int frames = 0;
    for (const int size : { 1, 16, 37, 64, 128, 512, 777, 2048 })
    {
        juce::AudioBuffer<float> audio (2, size);
        juce::MidiBuffer midi;
        for (int sample = 0; sample < size; ++sample)
        {
            audio.setSample (0, sample, 0.2f);
            audio.setSample (1, sample, -0.3f);
        }
        plugin.processBlock (audio, midi);
        for (int sample = 0; sample < size; ++sample)
        {
            const double leftError = std::abs (double (audio.getSample (0, sample)) - 0.1);
            const double rightError = std::abs (double (audio.getSample (1, sample)) + 0.15);
            require (std::isfinite (leftError) && std::isfinite (rightError), "nonfinite FX output");
            maximumError = std::max (maximumError, std::max (leftError, rightError));
        }
        frames += size;
    }
    require (maximumError < 1e-5, "FX stereo/gain mismatch through VST3");
    gain->setValueNotifyingHost (0.8f);
    plugin.setStateInformation (saved.getData(), int (saved.getSize()));
    require (std::abs (gain->getValue() - 0.25f) < 1e-5f, "FX gain state did not restore");
    // A malformed state must not silently change a valid gain parameter.
    const char invalid[] = "{broken";
    plugin.setStateInformation (invalid, int (sizeof invalid));
    require (std::abs (gain->getValue() - 0.25f) < 1e-5f, "invalid state changed gain");
    auto result = new juce::DynamicObject;
    result->setProperty ("mode", "trusted-fx");
    result->setProperty ("frames_verified", frames);
    result->setProperty ("maximum_audio_error", maximumError);
    result->setProperty ("gain_state_roundtrip", true);
    result->setProperty ("invalid_state_preserves_gain", true);
    return juce::var (result);
}

inline juce::var instrument (juce::AudioPluginInstance& plugin)
{
    require (plugin.getTotalNumOutputChannels() == 2, "instrument output must be stereo");
    juce::AudioBuffer<float> audio (2, 1024);
    audio.clear();
    juce::MidiBuffer midi;
    midi.addEvent (juce::MidiMessage::noteOn (1, 69, juce::uint8 (127)), 37);
    midi.addEvent (juce::MidiMessage::noteOn (1, 72, juce::uint8 (100)), 63);
    plugin.processBlock (audio, midi);
    double maximumError = 0;
    double peak = 0;
    for (int frame = 0; frame < 1024; ++frame)
    {
        double expectedLeft = 0, expectedRight = 0;
        for (const auto note : { 69, 72 })
        {
            const int start = note == 69 ? 37 : 63;
            if (frame < start) continue;
            const double velocity = note == 69 ? 1.0 : 100.0 / 127.0;
            const double frequency = 440 * std::pow (2.0, (note - 69) / 12.0);
            const double sample = std::sin (juce::MathConstants<double>::twoPi * frequency
                                           * (frame - start) / 48000.0) * velocity * 0.08;
            expectedLeft += sample;
            expectedRight += sample * (0.5 + (note % 12) / 24.0);
        }
        const double left = audio.getSample (0, frame), right = audio.getSample (1, frame);
        require (std::isfinite (left) && std::isfinite (right), "nonfinite instrument output");
        maximumError = std::max (maximumError, std::max (std::abs (left - expectedLeft),
                                                         std::abs (right - expectedRight)));
        peak = std::max (peak, std::abs (left));
    }
    require (peak > 0.01 && maximumError < 2e-5,
             "VST3 instrument notes/offsets/velocity/stereo mismatch");
    midi.clear();
    midi.addEvent (juce::MidiMessage::noteOff (1, 69), 11);
    midi.addEvent (juce::MidiMessage::noteOn (1, 72, juce::uint8 (0)), 17);
    audio.clear();
    plugin.processBlock (audio, midi);
    double releasePeak = 0;
    for (int frame = 600; frame < 1024; ++frame)
        for (int channel = 0; channel < 2; ++channel)
        {
            const double sample = audio.getSample (channel, frame);
            require (std::isfinite (sample), "nonfinite release output");
            releasePeak = std::max (releasePeak, std::abs (sample));
        }
    require (releasePeak < 1e-5, "VST3 instrument note-off/zero-velocity release failure");
    auto result = new juce::DynamicObject;
    result->setProperty ("mode", "trusted-instrument");
    result->setProperty ("frames_verified", 2048);
    result->setProperty ("maximum_audio_error", maximumError);
    result->setProperty ("release_tail_peak", releasePeak);
    result->setProperty ("onsets_checked", "37,63");
    return juce::var (result);
}
}
