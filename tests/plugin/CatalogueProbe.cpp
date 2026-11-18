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
    juce::AudioBuffer<float> buffer(2, 513);
    juce::MidiBuffer midi;
    for (int i = 0; i < 513; ++i) { buffer.setSample(0, i, 0.25f); buffer.setSample(1, i, -0.75f); }
    processor.processBlock(buffer, midi);
    peak = 0.0f;
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < 513; ++i) {
            const float v = buffer.getSample(c, i);
            if (!std::isfinite(v)) return false;
            peak = std::max(peak, std::abs(v));
        }
    return peak > 0.0f;
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
    } else {
        processor.prepareToPlay(48000, 512);
        if (!processor.ready()) return fail("prepare: " + processor.status());
        processor.setModuleParamValue(0, 5.0f);
        processor.setModuleParamValue(1, 0.75f);
        processor.setModuleParamValue(2, 1.5f);
    }

    processor.prepareToPlay(48000, 512);
    if (!processor.ready()) return fail("prepare after restore: " + processor.status());
    float peak = 0.0f;
    if (!render(processor, peak)) return fail("render failed");

    if (!stateIn.isNotEmpty()) {
        if (std::abs(processor.moduleParamValue(0) - 5.0f) > 1e-4f
            || std::abs(processor.moduleParamValue(1) - 0.75f) > 1e-4f
            || std::abs(processor.moduleParamValue(2) - 1.5f) > 1e-4f)
            return fail("parameter values before state");
        if (stateOut.isNotEmpty()) {
            juce::MemoryBlock state;
            processor.getStateInformation(state);
            juce::File out{juce::File(stateOut)};
            out.deleteFile();
            juce::FileOutputStream stream{out};
            if (stream.failedToOpen() || !stream.write(state.getData(), state.getSize()))
                return fail("cannot write state file");
        }
    }

    std::cout << "OK id=" << processor.moduleID() << " digest=" << processor.packageHash()
              << " peak=" << peak << " attack=" << processor.moduleParamValue(0)
              << " depth=" << processor.moduleParamValue(1) << " gain=" << processor.moduleParamValue(2) << "\n";
    return 0;
}
