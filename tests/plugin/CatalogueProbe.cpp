// SPDX-License-Identifier: AGPL-3.0-only
// End-to-end catalogue probe: list -> download -> install -> load -> render ->
// save state -> (server can stop) -> restore -> render. No DAW and no UI.
#include "Processor.h"
#include "CatalogueClient.h"
#include "ModuleStore.h"
#include "ModuleUI.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>

using namespace openvst;

namespace {
int fail(const juce::String& message) { std::cerr << "FAIL " << message << "\n"; return 1; }

bool render(Processor& processor, float& peak) {
    juce::AudioBuffer<float> buffer(2, 512);
    juce::MidiBuffer midi;
    peak = 0.0f;
    // Exercise modules with short delay lines as well as zero-history effects.
    for (int block = 0; block < 16; ++block) {
        for (int i = 0; i < buffer.getNumSamples(); ++i) {
            buffer.setSample(0, i, 0.25f);
            buffer.setSample(1, i, -0.75f);
        }
        processor.processBlock(buffer, midi);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < buffer.getNumSamples(); ++i) {
                const float v = buffer.getSample(c, i);
                if (!std::isfinite(v)) return false;
                peak = std::max(peak, std::abs(v));
            }
    }
    return peak > 0.0f;
}

double sidebandRatio(Processor& processor, float amplitude) {
    constexpr float fs = 48000.0f;
    constexpr double tau = 6.283185307179586;
    constexpr int block = 240;
    juce::AudioBuffer<float> buffer(2, block);
    juce::MidiBuffer midi;
    double sine = 0.0, cosine = 0.0;
    for (int frame = 0; frame < 14400; frame += block) {
        buffer.clear();
        for (int i = 0; i < block; ++i)
            buffer.setSample(0, i, amplitude * (float)std::sin(tau * 400.0 * (frame + i) / fs));
        processor.processBlock(buffer, midi);
        for (int i = 0; i < block; ++i) {
            if (buffer.getSample(1, i) != 0.0f || !std::isfinite(buffer.getSample(0, i))) return -1.0;
            if (frame < 9600) continue;
            const auto angle = tau * 500.0 * (frame + i) / fs;
            sine += buffer.getSample(0, i) * std::sin(angle);
            cosine += buffer.getSample(0, i) * std::cos(angle);
        }
    }
    return 2.0 * std::hypot(sine, cosine) / (4800.0 * amplitude);
}

float probeValue(const ParamSpec& param, bool tremolo) {
    // Preserve the values consumed by the existing REAPER tremolo fixture.
    if (tremolo) {
        if (param.id == "speed") return 5.0f;
        if (param.id == "amount") return 0.75f;
        if (param.id == "level") return 1.5f;
    }
    return param.min + 0.375f * (param.max - param.min);
}
}

int main(int argc, char** argv) {
    juce::String url, storePath, runtime, fixtures, id, stateOut, stateIn, packagePath;
    bool offline = false;
    for (int i = 1; i + 1 < argc; i += 2) {
        const juce::String key(argv[i]);
        const juce::String value(argv[i + 1]);
        if (key == "--url") url = value;
        else if (key == "--store") storePath = value;
        else if (key == "--runtime") runtime = value;
        else if (key == "--fixtures") fixtures = value;
        else if (key == "--id") id = value;
        else if (key == "--state-out") stateOut = value;
        else if (key == "--state-in") stateIn = value;
        else if (key == "--package") packagePath = value;
        else if (key == "--offline") offline = value == "1";
        else return fail("unknown argument " + key);
    }
    if (storePath.isEmpty() || runtime.isEmpty() || fixtures.isEmpty() || id.isEmpty())
        return fail("missing --store, --runtime, --fixtures or --id");

    if (!offline) {
        ModuleStore store{juce::File(storePath)};
        if (packagePath.isNotEmpty()) {
            juce::File zip(packagePath);
            if (!zip.existsAsFile()) return fail("package file missing: " + packagePath);
            const auto digest = ModuleStore::sha256File(zip);
            const auto installed = store.install(zip, digest);
            if (!installed.ok) return fail("local install: " + installed.error);
        } else {
            if (url.isEmpty()) return fail("missing --url");
            CatalogueClient client(url);
            auto list = client.list("effect");
            if (!list.ok) return fail("list: " + list.error);
            const CatalogueEntry* entry = nullptr;
            for (const auto& e : list.modules) if (e.id == id) { entry = &e; break; }
            if (entry == nullptr) return fail("module not listed: " + id);

            auto downloads = juce::File(storePath).getSiblingFile("downloads");
            if (!downloads.createDirectory()) return fail("cannot create downloads directory");
            auto part = downloads.getChildFile(entry->digest + ".part");
            auto fetched = client.download(*entry, part);
            if (!fetched.ok) return fail("download: " + fetched.error);
            const auto installed = store.install(part, entry->digest);
            if (!installed.ok) return fail("install: " + installed.error);
            if (installed.installed.package.id != entry->id
                || installed.installed.package.version != entry->version
                || installed.installed.package.kind != entry->kind) {
                store.remove(installed.installed);
                return fail("package identity does not match the catalogue entry");
            }
            part.deleteFile();
        }
    }

    Paths paths;
    paths.runtime = runtime;
    paths.fixtures = fixtures;
    paths.moduleStore = juce::File(storePath);
    paths.catalogueURL = url;

    Processor processor(false, paths);
    processor.refreshInstalledModules();
    if (!processor.setModule(id, {})) return fail("installed module not selectable: " + id);
    if (!processor.module().uiPath.empty()) {
        const juce::File uiFile(juce::String(processor.module().uiPath.c_str()));
        juce::String uiError;
        if (!ModuleUIComponent::validate(processor, uiFile.loadFileAsString(), uiError))
            return fail("module UI rejected: " + uiError);
    }

    if (stateIn.isNotEmpty()) {
        juce::MemoryBlock state;
        juce::FileInputStream in{juce::File(stateIn)};
        if (!in.openedOk()) return fail("cannot open state file");
        state.setSize((size_t)in.getTotalLength());
        if (state.getSize() == 0 || in.read(state.getData(), (int)state.getSize()) != (int)state.getSize())
            return fail("cannot read state file");
        processor.setStateInformation(state.getData(), (int)state.getSize());
        if (processor.moduleID() != id) return fail("state restored another module");
    }

    const bool tremolo = id == "org.openvst.demo.tremolo";
    const auto& params = processor.module().params;
    for (size_t i = 0; i < params.size(); ++i) {
        const auto expected = probeValue(params[i], tremolo);
        if (stateIn.isEmpty()) processor.setModuleParamValue((int)i, expected);
        if (std::abs(processor.moduleParamValue((int)i) - expected) > 1e-4f)
            return fail("unexpected parameter value: " + juce::String(params[i].id.c_str()));
    }

    processor.prepareToPlay(48000, 512);
    if (!processor.ready()) return fail("prepare after restore: " + processor.status());
    float peak = 0.0f;
    if (!render(processor, peak)) return fail("render failed");

    if (stateIn.isEmpty() && stateOut.isNotEmpty()) {
        juce::MemoryBlock state;
        processor.getStateInformation(state);
        juce::File out{juce::File(stateOut)};
        out.deleteFile();
        juce::FileOutputStream stream{out};
        if (stream.failedToOpen() || !stream.write(state.getData(), state.getSize()))
            return fail("cannot write state file");
    }

    std::cout << "OK id=" << processor.moduleID() << " digest=" << processor.packageHash()
              << " peak=" << peak;
    for (size_t i = 0; i < params.size(); ++i)
        std::cout << " " << params[i].id << "=" << processor.moduleParamValue((int)i);
    if (id == "org.openvst.creature-voice") {
        // Isolate ring modulation from the pitch branch and check the 400+100 Hz sideband.
        processor.setModuleParamValue(1, 0.0f);
        processor.setModuleParamValue(2, 100.0f);
        processor.setModuleParamValue(3, 1.0f);
        processor.setModuleParamValue(4, 0.16f);
        processor.setModuleParamValue(5, 1.0f);
        processor.prepareToPlay(48000, 512);
        if (!processor.ready()) return fail("prepare sideband probe");
        const double quiet = sidebandRatio(processor, 0.03f);
        const double loud = sidebandRatio(processor, 0.7f);
        if (quiet < 0.0 || quiet > 0.003 || loud < 0.35 || loud > 0.6)
            return fail("reactive texture sideband or stereo isolation");
        std::cout << " quiet_sideband=" << quiet << " loud_sideband=" << loud;
    }
    std::cout << "\n";
    return 0;
}
