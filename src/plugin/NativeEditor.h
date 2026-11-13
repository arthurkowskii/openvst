// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "ModuleRegistry.h"
namespace openvst {
class NativeEditor final : public juce::AudioProcessorEditor, private juce::Timer {
    Processor& p;
    juce::Label info;
    juce::ComboBox modules;
    juce::StringArray ids; // combo item -> registry id
    juce::OwnedArray<juce::Label> labels;
    juce::OwnedArray<juce::Slider> sliders;
    juce::String lastModuleID;
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
        lastModuleID=p.moduleID(); rebuildSliders();
        setSize(560,260+p.module().paramCount*40); timerCallback(); startTimerHz(4);
    }
    void resized() override {
        info.setBounds(12,12,getWidth()-24,140);
        modules.setBounds(12,158,220,26);
        for(int i=0;i<sliders.size();++i) {
            labels.getUnchecked(i)->setBounds(12,192+i*40,150,24);
            sliders.getUnchecked(i)->setBounds(166,192+i*40,getWidth()-178,24);
        }
    }
private:
    // One plain native slider per parameter of the active module. The set is
    // rebuilt when the module changes: sliders are cheap native components,
    // never HTML, and their ranges come straight from the module's table.
    void rebuildSliders() {
        sliders.clear(); labels.clear();
        const ModuleInfo& m=p.module();
        for(int i=0;i<m.paramCount;++i) {
            const ParamInfo& par=m.params[i];
            auto* label=new juce::Label;
            label->setText(juce::String(par.name)+(par.unit[0]!='\0'?juce::String(" (")+par.unit+")":juce::String()),juce::dontSendNotification);
            addAndMakeVisible(label); labels.add(label);
            auto* slider=new juce::Slider;
            slider->setRange(par.min,par.max,0.001);
            slider->setTextBoxStyle(juce::Slider::TextBoxRight,false,80,24);
            const int host=p.moduleParamHostIndex(i);
            auto* s=slider;
            s->onDragStart=[this,host]{ if(auto* par2=p.hostParameter(host)) par2->beginChangeGesture(); };
            s->onDragEnd=[this,host]{ if(auto* par2=p.hostParameter(host)) par2->endChangeGesture(); };
            s->onValueChange=[this,host,s]{ p.setParamValue(host,(float)s->getValue()); };
            s->setValue(p.paramValue(host),juce::dontSendNotification);
            addAndMakeVisible(slider); sliders.add(slider);
        }
        setSize(560,260+m.paramCount*40);
    }
    void timerCallback() override {
        // Plain ASCII only: a non-ASCII literal compiled under a different code
        // page shows as mojibake in the host, as seen on Windows.
        info.setText(p.getName()+" - private prototype\n"+p.moduleID()+" / "+p.moduleVersion()+"\nSHA256 "+p.moduleHash()+"\nCmajor 1.0.3209\n"+p.status(),juce::dontSendNotification);
        const int wanted=ids.indexOf(p.moduleID())+1;
        if(wanted>0 && modules.getSelectedId()!=wanted) modules.setSelectedId(wanted,juce::dontSendNotification);
        if(p.moduleID()!=lastModuleID) { lastModuleID=p.moduleID(); rebuildSliders(); resized(); }
        for(int i=0;i<sliders.size();++i) {
            auto* s=sliders.getUnchecked(i);
            if(!s->isMouseButtonDown()) {
                s->setValue(p.moduleParamValue(i),juce::dontSendNotification);
                const ParamInfo& par=p.module().params[i];
                labels.getUnchecked(i)->setText(juce::String(par.name)+(par.unit[0]!='\0'?juce::String(" (")+par.unit+")":juce::String())+": "+juce::String(p.moduleParamValue(i),3),juce::dontSendNotification);
            }
        }
    }
};
}
