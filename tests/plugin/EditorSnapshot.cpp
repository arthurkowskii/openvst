// Offscreen snapshot of the plugin editor, for layout review without a DAW.
//
// The editor is a plain JUCE component: it can be rendered into an image on a
// headless display (Xvfb), which makes label clipping, overlapping children and
// control sizes measurable, instead of guessed from a host screenshot.
//
// Usage: openvst_editor_snapshot --store DIR --runtime DLL --fixtures DIR
//                               --module ID --out PNG [--width W --height H]
//                               [--catalogue-panel] [--width2 W --height2 H]
#include "Processor.h"
#include "CataloguePanel.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <iostream>

using namespace openvst;

namespace {
int fail(const juce::String& message) { std::cerr << "FAIL " << message << "\n"; return 1; }

bool saveSnapshot(juce::AudioProcessorEditor& editor, const juce::String& path) {
    editor.setSize(editor.getWidth(), editor.getHeight());
    const auto image = editor.createComponentSnapshot(editor.getLocalBounds(), true, 1.0f);
    if (!image.isValid()) return false;
    juce::File file(path);
    file.deleteFile();
    juce::FileOutputStream out(file);
    if (!out.openedOk()) return false;
    juce::PNGImageFormat png;
    return png.writeImageToStream(image, out);
}

bool saveComponent(juce::Component& component, const juce::String& path) {
    const auto image = component.createComponentSnapshot(component.getLocalBounds(), true, 1.0f);
    if (!image.isValid()) return false;
    juce::File file(path);
    file.deleteFile();
    juce::FileOutputStream out(file);
    if (!out.openedOk()) return false;
    juce::PNGImageFormat png;
    return png.writeImageToStream(image, out);
}

} // namespace

int main(int argc, char** argv) {
    juce::String store, runtime, fixtures, module, out;
    int width = 720, height = 560, width2 = 0, height2 = 0;
    bool catalogue = false;
    for (int i = 1; i + 1 < argc; i += 2) {
        const juce::String key(argv[i]), value(argv[i + 1]);
        if (key == "--store") store = value;
        else if (key == "--runtime") runtime = value;
        else if (key == "--fixtures") fixtures = value;
        else if (key == "--module") module = value;
        else if (key == "--out") out = value;
        else if (key == "--width") width = value.getIntValue();
        else if (key == "--height") height = value.getIntValue();
        else if (key == "--width2") width2 = value.getIntValue();
        else if (key == "--height2") height2 = value.getIntValue();
        else if (key == "--catalogue-panel") catalogue = value == "1";
        else return fail("unknown argument " + key);
    }
    if (store.isEmpty() || runtime.isEmpty() || fixtures.isEmpty() || module.isEmpty() || out.isEmpty())
        return fail("missing --store, --runtime, --fixtures, --module or --out");

    juce::ScopedJuceInitialiser_GUI gui;
    Paths paths;
    paths.runtime = runtime;
    paths.fixtures = fixtures;
    paths.moduleStore = juce::File(store);
    paths.catalogueURL = "http://127.0.0.1:8080";

    Processor processor(false, paths);
    processor.refreshInstalledModules();
    if (!processor.setModule(module, {})) return fail("module not selectable: " + module);
    processor.prepareToPlay(48000, 512);
    if (!processor.ready()) return fail("prepare: " + processor.status());

    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    if (editor == nullptr) return fail("no editor");
    editor->setSize(width, height);
    if (!saveSnapshot(*editor, out)) return fail("cannot write " + out);
    std::cout << "OK " << out << " " << width << "x" << height << "\n";

    if (width2 > 0 && height2 > 0) {
        editor->setSize(width2, height2);
        juce::String second = juce::File(out).getSiblingFile(juce::File(out).getFileNameWithoutExtension() + "-resized.png").getFullPathName();
        if (!saveSnapshot(*editor, second)) return fail("cannot write " + second);
        std::cout << "OK " << second << " " << width2 << "x" << height2 << "\n";
    }
    if (catalogue) {
        // The Discover panel is exercised the way a user does: press Refresh,
        // then select the first entry and press Install. The catalogue URL is
        // the local test server; nothing leaves the machine.
        CataloguePanel panel(processor, paths, [] {}, [] {});
        panel.setSize(660, 300);
        panel.refreshNow();
        juce::String first = juce::File(out).getSiblingFile("discover-1-list.png").getFullPathName();
        if (!saveComponent(panel, first)) return fail("cannot write " + first);
        std::cout << "OK " << first << "\n";
        panel.selectFirstEntry();
        panel.installNow();
        juce::String second = juce::File(out).getSiblingFile("discover-2-installed.png").getFullPathName();
        if (!saveComponent(panel, second)) return fail("cannot write " + second);
        std::cout << "OK " << second << "\n";
        std::cout << "module_after_install=" << processor.moduleID() << "\n";
    }
    return 0;
}
