// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "CataloguePanel.h"
#include "ModuleRegistry.h"
#include "ModuleSpec.h"
#include "ModuleUI.h"
namespace openvst {
class NativeEditor final : public juce::AudioProcessorEditor, private juce::Timer {
    Processor& p;
    juce::Label info;
    juce::ComboBox modules;
    juce::StringArray ids; // combo item -> module id
    juce::OwnedArray<juce::Label> labels;
    juce::OwnedArray<juce::Slider> sliders;
    juce::TextButton discover{"Discover"};
    std::unique_ptr<ModuleUIComponent> moduleUI;
    std::unique_ptr<CataloguePanel> catalogue;
    bool catalogueVisible = false;
    juce::String lastModuleID;
public:
    explicit NativeEditor(Processor& processor):AudioProcessorEditor(processor),p(processor) {
        addAndMakeVisible(info); info.setJustificationType(juce::Justification::topLeft);
        addAndMakeVisible(modules);
        addAndMakeVisible(discover);
        modules.onChange=[this]{
            const int index=modules.getSelectedId()-1;
            if(index>=0 && index<ids.size() && p.setModuleID(ids[index])) p.reprepare();
        };
        discover.onClick=[this]{ showCatalogue(!catalogueVisible); };
        rebuildModuleList();
        lastModuleID=p.moduleID();
        rebuildContent();
        setResizable(true,true);
        setResizeLimits(640,420,1200,900);
        setSize(720,560);
        timerCallback(); startTimerHz(4);
    }
    void resized() override {
        info.setBounds(12,12,getWidth()-24,130);
        modules.setBounds(12,150,300,26);
        discover.setBounds(getWidth()-152,150,140,26);
        auto content=getLocalBounds().reduced(12);
        content.removeFromTop(190);
        if(catalogueVisible && catalogue) catalogue->setBounds(content);
        if(moduleUI && !catalogueVisible) {
            const int w=juce::jmin(content.getWidth(),moduleUI->preferredWidth());
            const int h=juce::jmin(content.getHeight(),moduleUI->preferredHeight());
            moduleUI->setBounds(content.withSizeKeepingCentre(w,h));
        }
        for(int i=0;i<sliders.size();++i) {
            labels.getUnchecked(i)->setBounds(content.getX(),content.getY()+i*40,170,24);
            sliders.getUnchecked(i)->setBounds(content.getX()+174,content.getY()+i*40,juce::jmax(120,content.getWidth()-174),24);
        }
    }
private:
    void rebuildModuleList() {
        modules.clear(juce::dontSendNotification); ids.clear();
        const bool synth=p.acceptsMidi();
        auto add=[&](const std::vector<ModuleSpec>& list) {
            for(const auto& m : list) if(m.instrument==synth) {
                modules.addItem(juce::String(m.name.c_str()),ids.size()+1);
                ids.add(m.id.c_str());
            }
        };
        add(p.builtinModules()); add(p.installedModules());
        const int wanted=ids.indexOf(p.moduleID())+1;
        if(wanted>0) modules.setSelectedId(wanted,juce::dontSendNotification);
    }
    // The declarative UI is data, rendered natively. A module without a UI
    // document falls back to one plain slider per parameter.
    void rebuildContent() {
        moduleUI.reset();
        rebuildSliders();
        if(!p.module().uiPath.empty()) {
            moduleUI=ModuleUIComponent::create(p,juce::File(juce::String(p.module().uiPath.c_str())));
            if(moduleUI) addAndMakeVisible(*moduleUI);
        }
        for(int i=0;i<sliders.size();++i) {
            sliders.getUnchecked(i)->setVisible(!moduleUI && !catalogueVisible);
            labels.getUnchecked(i)->setVisible(!moduleUI && !catalogueVisible);
        }
        resized();
    }
    void showCatalogue(bool show) {
        if(show && !catalogue) {
            catalogue=std::make_unique<CataloguePanel>(p,p.processorPaths(),
                [this]{ showCatalogue(false); rebuildModuleList(); rebuildContent(); },
                [this]{ showCatalogue(false); });
            addAndMakeVisible(*catalogue);
        }
        catalogueVisible=show;
        discover.setButtonText(show?"Close catalogue":"Discover");
        modules.setVisible(!show);
        if(catalogue) catalogue->setVisible(show);
        if(moduleUI) moduleUI->setVisible(!show);
        for(int i=0;i<sliders.size();++i) {
            sliders.getUnchecked(i)->setVisible(!show && !moduleUI);
            labels.getUnchecked(i)->setVisible(!show && !moduleUI);
        }
        resized();
    }
    // One plain native slider per parameter of the active module. The set is
    // rebuilt when the module changes: sliders are cheap native components,
    // never HTML, and their ranges come straight from the module's table. A
    // catalogue module parameter is mapped onto its permanent generic host
    // slot; the slider still shows and edits the real module value.
    void rebuildSliders() {
        sliders.clear(); labels.clear();
        const ModuleSpec& m=p.module();
        for(int i=0;i<(int)m.params.size();++i) {
            const ParamSpec& par=m.params[(std::size_t)i];
            auto* label=new juce::Label;
            label->setText(juce::String(par.name.c_str())+(par.unit.empty()?juce::String():juce::String(" (")+par.unit.c_str()+")"),juce::dontSendNotification);
            addAndMakeVisible(label); labels.add(label);
            auto* slider=new juce::Slider;
            slider->setRange(par.min,par.max,0.001);
            slider->setTextBoxStyle(juce::Slider::TextBoxRight,false,80,24);
            auto* s=slider;
            const int host=p.moduleParamHostIndex(i);
            s->onDragStart=[this,host]{ if(auto* par2=p.hostParameter(host)) par2->beginChangeGesture(); };
            s->onDragEnd=[this,host]{ if(auto* par2=p.hostParameter(host)) par2->endChangeGesture(); };
            s->onValueChange=[this,i,s]{ p.setModuleParamValue(i,(float)s->getValue()); };
            s->setValue(p.moduleParamValue(i),juce::dontSendNotification);
            addAndMakeVisible(slider); sliders.add(slider);
        }
    }
    void timerCallback() override {
        // Plain ASCII only: a non-ASCII literal compiled under a different code
        // page shows as mojibake in the host, as seen on Windows.
        info.setText(p.getName()+" - private prototype\n"+p.moduleID()+" / "+p.moduleVersion()+"\nSHA256 "+p.moduleHash()+"\nCmajor 1.0.3209\n"+p.status(),juce::dontSendNotification);
        const int wanted=ids.indexOf(p.moduleID())+1;
        if(wanted>0 && modules.getSelectedId()!=wanted) modules.setSelectedId(wanted,juce::dontSendNotification);
        if(p.moduleID()!=lastModuleID) { lastModuleID=p.moduleID(); rebuildModuleList(); rebuildContent(); }
        for(int i=0;i<sliders.size();++i) {
            auto* s=sliders.getUnchecked(i);
            if(!s->isMouseButtonDown()) {
                s->setValue(p.moduleParamValue(i),juce::dontSendNotification);
                const ParamSpec& par=p.module().params[(std::size_t)i];
                labels.getUnchecked(i)->setText(juce::String(par.name.c_str())+(par.unit.empty()?juce::String():juce::String(" (")+par.unit.c_str()+")")+": "+juce::String(p.moduleParamValue(i),3),juce::dontSendNotification);
            }
        }
    }
};
}
