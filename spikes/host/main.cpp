// Prototype verification utility. JUCE is used under its open-source AGPLv3 option.
#include <juce_audio_processors/juce_audio_processors.h>
#include <iostream>
#include <cmath>
#include <stdexcept>
#include "fixture_checks.h"

int main (int argc, char** argv)
{
    if (argc != 2 && argc != 3)
    {
        std::cerr << "usage: OpenVSTHostProbe /absolute/path/plugin.vst3 [--trusted-fx|--trusted-instrument]\n";
        return 2;
    }
    const juce::String mode = argc == 3 ? argv[2] : "";
    if (mode.isNotEmpty() && mode != "--trusted-fx" && mode != "--trusted-instrument")
    {
        std::cerr << "unknown mode\n";
        return 2;
    }
    try
    {
        const juce::File plugin (juce::String::fromUTF8 (argv[1]));
        if (! plugin.exists())
            throw std::runtime_error ("missing plugin");
        juce::ScopedJuceInitialiser_GUI initialiser;
        juce::VST3PluginFormat format;
        juce::OwnedArray<juce::PluginDescription> descriptions;
        format.findAllTypesForFile (descriptions, plugin.getFullPathName());
        if (descriptions.isEmpty())
            throw std::runtime_error ("no VST3 component discovered");
        juce::String error;
        // The wrapper must be told the maximum size used by the fixture tests.
        // Feeding >64 frames after declaring a 64-frame maximum invalidates the
        // host contract and can overrun the wrapper's own conversion buffers.
        constexpr int maximumBlock = 2048;
        auto instance = format.createInstanceFromDescription (*descriptions[0], 48000.0, maximumBlock, error);
        if (! instance)
            throw std::runtime_error ("instantiate: " + error.toStdString());
        instance->setNonRealtime (true);
        instance->setPlayConfigDetails (instance->getTotalNumInputChannels(),
                                       instance->getTotalNumOutputChannels(), 48000.0, maximumBlock);
        instance->prepareToPlay (48000.0, maximumBlock);
        const int channels = juce::jmax (2, instance->getTotalNumInputChannels(),
                                        instance->getTotalNumOutputChannels());
        juce::AudioBuffer<float> audio (channels, 64);
        juce::MidiBuffer midi;
        double peak = 0;
        for (int block = 0; block < 100; ++block)
        {
            audio.clear();
            if (block == 0) audio.setSample (0, 0, 0.25f);
            instance->processBlock (audio, midi);
            for (int channel = 0; channel < audio.getNumChannels(); ++channel)
                for (int sample = 0; sample < audio.getNumSamples(); ++sample)
                {
                    const float value = audio.getSample (channel, sample);
                    if (! std::isfinite (value))
                        throw std::runtime_error ("non-finite output");
                    peak = std::max (peak, std::abs (double (value)));
                }
            midi.clear();
        }
        juce::MemoryBlock state;
        instance->getStateInformation (state);
        instance->setStateInformation (state.getData(), int (state.getSize()));
        juce::var verifiedFixture;
        if (mode == "--trusted-fx") verifiedFixture = fixture_checks::effect (*instance);
        if (mode == "--trusted-instrument") verifiedFixture = fixture_checks::instrument (*instance);
        instance->releaseResources();
        auto report = new juce::DynamicObject;
        report->setProperty ("plugin", instance->getName());
        report->setProperty ("components_discovered", descriptions.size());
        report->setProperty ("processed_frames", 6400);
        report->setProperty ("output_peak", peak);
        report->setProperty ("state_bytes", juce::int64 (state.getSize()));
        report->setProperty ("editor_created", false);
        if (! verifiedFixture.isVoid()) report->setProperty ("fixture_checks", verifiedFixture);
        report->setProperty ("scope", "offline smoke only; not a DAW or DSP correctness certification");
        std::cout << juce::JSON::toString (juce::var (report)).toStdString() << '\n';
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
