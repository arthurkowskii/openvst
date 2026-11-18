// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <vector>
#include "CatalogueClient.h"
#include "ModuleStore.h"
#include "Processor.h"

namespace openvst {

// Minimal Discover / My Library proof. The network and the installer run on
// the message thread in this slice: packages are small and the server is
// local. The audio callback is never involved, and no package is loaded until
// the installer returned a verified store entry.
class CataloguePanel final : public juce::Component {
public:
    CataloguePanel(Processor& processor, Paths pathsIn,
                   std::function<void()> installed, std::function<void()> closed)
        : p(processor), paths(std::move(pathsIn)), client(paths.catalogueURL),
          onInstalled(std::move(installed)), onClose(std::move(closed)) {
        addAndMakeVisible(list);
        addAndMakeVisible(refreshButton);
        addAndMakeVisible(installButton);
        addAndMakeVisible(closeButton);
        addAndMakeVisible(status);
        addAndMakeVisible(details);
        status.setJustificationType(juce::Justification::topLeft);
        details.setJustificationType(juce::Justification::topLeft);
        status.setText("Catalogue: " + paths.catalogueURL + "\nPress Refresh to list approved modules.", juce::dontSendNotification);
        refreshButton.onClick = [this] { refresh(); };
        installButton.onClick = [this] { install(); };
        closeButton.onClick = [this] { if (onClose) onClose(); };
        installButton.setEnabled(false);
        list.onChange = [this] {
            const int index = list.getSelectedId() - 1;
            installButton.setEnabled(index >= 0 && index < (int)entries.size());
            showDetails(index);
        };
    }
    void paint(juce::Graphics& g) override {
        g.setColour(juce::Colour(0x18ffffff));
        g.fillRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 6.0f);
        g.setColour(juce::Colour(0x30ffffff));
        g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 6.0f, 1.0f);
    }
    void resized() override {
        auto r = getLocalBounds().reduced(12);
        auto top = r.removeFromTop(28);
        refreshButton.setBounds(top.removeFromLeft(96));
        top.removeFromLeft(8);
        closeButton.setBounds(top.removeFromRight(80));
        r.removeFromTop(8);
        list.setBounds(r.removeFromTop(28));
        r.removeFromTop(8);
        details.setBounds(r.removeFromTop(76));
        r.removeFromTop(8);
        installButton.setBounds(r.removeFromTop(28).removeFromLeft(120));
        r.removeFromTop(8);
        status.setBounds(r);
    }
private:
    void refresh() {
        status.setText("Loading " + paths.catalogueURL + " ...", juce::dontSendNotification);
        entries.clear();
        list.clear(juce::dontSendNotification);
        installButton.setEnabled(false);
        const auto result = client.list(p.acceptsMidi() ? "instrument" : "effect");
        if (!result.ok) {
            status.setText("Catalogue error: " + result.error, juce::dontSendNotification);
            resized();
            return;
        }
        entries = result.modules;
        for (int i = 0; i < (int)entries.size(); ++i) {
            const auto& e = entries[(std::size_t)i];
            list.addItem(juce::String(e.name + "  " + e.version + "  [" + e.kind + "]"), i + 1);
        }
        status.setText(result.modules.empty()
                           ? "No approved module for this variant."
                           : juce::String((int)result.modules.size()) + " module(s) available.",
                       juce::dontSendNotification);
        resized();
    }
    void showDetails(int index) {
        if (index < 0 || index >= (int)entries.size()) { details.setText({}, juce::dontSendNotification); return; }
        const auto& e = entries[(std::size_t)index];
        juce::String text;
        text << e.id << "\n" << e.vendor << " / " << e.version << " / " << e.status
             << "\nSHA256 " << e.digest << "\n" << juce::String((double)e.size / 1024.0, 1) << " KiB";
        details.setText(text, juce::dontSendNotification);
    }
    void install() {
        const int index = list.getSelectedId() - 1;
        if (index < 0 || index >= (int)entries.size()) return;
        const auto entry = entries[(std::size_t)index];
        const auto downloads = paths.moduleStore.getSiblingFile("downloads");
        if (!downloads.createDirectory()) { status.setText("Cannot create the download directory.", juce::dontSendNotification); return; }
        const auto part = downloads.getChildFile(entry.digest + ".part");
        status.setText("Downloading " + entry.name + " ...", juce::dontSendNotification);
        const auto fetched = client.download(entry, part);
        if (!fetched.ok) { status.setText("Download failed: " + fetched.error, juce::dontSendNotification); return; }
        status.setText("Verifying and installing " + entry.name + " ...", juce::dontSendNotification);
        ModuleStore store(paths.moduleStore);
        const auto installed = store.install(part, entry.digest);
        if (!installed.ok) {
            status.setText("Install refused: " + installed.error, juce::dontSendNotification);
            return;
        }
        // The server is not trusted to describe the package it served: the
        // installed manifest must name the same module, version and kind that
        // the catalogue entry advertised. A mismatch is removed, not selected.
        if (installed.installed.package.id != entry.id
            || installed.installed.package.version != entry.version
            || installed.installed.package.kind != entry.kind) {
            store.remove(installed.installed);
            status.setText("Package identity does not match the catalogue entry; removed.", juce::dontSendNotification);
            return;
        }
        part.deleteFile();
        p.refreshInstalledModules();
        if (!p.setModule(entry.id, {})) {
            status.setText("Installed, but selection failed: " + entry.id, juce::dontSendNotification);
            return;
        }
        p.reprepare();
        status.setText("Installed and selected: " + entry.name + "\n" + p.status(), juce::dontSendNotification);
        installButton.setEnabled(false);
        if (onInstalled) onInstalled();
    }

    Processor& p;
    Paths paths;
    CatalogueClient client;
    std::vector<CatalogueEntry> entries;
    juce::ComboBox list;
    juce::TextButton refreshButton{"Refresh"}, installButton{"Install"}, closeButton{"Close"};
    juce::Label status, details;
    std::function<void()> onInstalled, onClose;
};

} // namespace openvst
