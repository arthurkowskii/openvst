// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>
#include "Processor.h"

namespace openvst {

// Declarative module UI (openvst.ui/0). The document is data: no script, no
// HTML, no remote resource. Unknown keys, unknown components, out-of-range
// layout values and undeclared parameter bindings make create() return null,
// so the caller falls back to the generic sliders.
class ModuleUIComponent final : public juce::Component, private juce::Timer {
public:
    static std::unique_ptr<ModuleUIComponent> create(Processor&, const juce::File& uiFile);
    static std::unique_ptr<ModuleUIComponent> createFromJSON(Processor&, const juce::String& json);
    // Parses and validates the document against the active module without
    // constructing any GUI component. Used by headless tests and by callers
    // that only need to know whether the document is usable.
    static bool validate(Processor&, const juce::String& json, juce::String& error);
    ~ModuleUIComponent() override;
    int preferredWidth() const;
    int preferredHeight() const;
    void resized() override;
    // The panel owns its background: a host window is not guaranteed to paint
    // anything behind the editor, and a test host may composite it over the
    // desktop. Without this, group frames and labels lose their contrast.
    void paint(juce::Graphics&) override;
private:
    struct Impl;
    explicit ModuleUIComponent(Processor&);
    std::unique_ptr<Impl> impl;
    void timerCallback() override;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ModuleUIComponent)
};

} // namespace openvst
