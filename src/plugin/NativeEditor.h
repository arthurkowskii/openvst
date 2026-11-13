// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "ModuleRegistry.h"
namespace openvst {
class NativeEditor final : public juce::AudioProcessorEditor, private juce::Timer {
    Processor& p;
    juce::Label info;
    juce::Slider slider;
    juce::ComboBox modules;
    juce::StringArray ids; // combo item -> registry id
public:
    explicit NativeEditor(Processor& processor):AudioProcessorEditor(processor),p(processor) {
        addAndMakeVisible(info); info.setJustificationType(juce::Justification::topLeft);
        // Only modules of this build's kind are offered; each carries its own
        // frozen identity. Selecting compiles at the next prepare, off the
        // audio thread, through the existing preparation path.
        const bool synth=p.acceptsMidi();
        for(const auto& m : kModules) if(m.instrument==synth) { modules.addItem(m.name,ids.size()+1); ids.add(m.id); }
        addAndMakeVisible(modules); modules.onChange=[this]{
            const int index=modules.getSelectedId()-1;
            if(index>=0 && index<ids.size() && p.setModuleID(ids[index])) p.reprepare();
        };
        if(!synth) {
            addAndMakeVisible(slider); slider.setRange(0,2,0.001); slider.setValue(p.gain(),juce::dontSendNotification);
            slider.setTextBoxStyle(juce::Slider::TextBoxRight,false,70,24);
            slider.onDragStart=[this]{p.getParameters()[0]->beginChangeGesture();};
            slider.onDragEnd=[this]{p.getParameters()[0]->endChangeGesture();};
            slider.onValueChange=[this]{p.setGain((float)slider.getValue());};
        }
        setSize(560,280); timerCallback(); startTimerHz(4);
    }
    void resized() override { info.setBounds(12,12,getWidth()-24,165); modules.setBounds(12,182,220,26); slider.setBounds(12,216,getWidth()-24,35); }
private:
    void timerCallback() override {
        // Plain ASCII only: a non-ASCII literal compiled under a different code
        // page shows as mojibake in the host, as seen on Windows.
        info.setText(p.getName()+" - private prototype\n"+p.moduleID()+" / "+p.moduleVersion()+"\nSHA256 "+p.moduleHash()+"\nCmajor 1.0.3209\n"+p.status(),juce::dontSendNotification);
        const int wanted=ids.indexOf(p.moduleID())+1;
        if(wanted>0 && modules.getSelectedId()!=wanted) modules.setSelectedId(wanted,juce::dontSendNotification);
        if(!slider.isMouseButtonDown()) slider.setValue(p.gain(),juce::dontSendNotification);
    }
};
}
