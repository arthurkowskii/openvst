// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
namespace openvst {
class NativeEditor final : public juce::AudioProcessorEditor, private juce::Timer {
    Processor& p;
    juce::Label info;
    juce::Slider slider;
public:
    explicit NativeEditor(Processor& processor):AudioProcessorEditor(processor),p(processor) {
        addAndMakeVisible(info); info.setJustificationType(juce::Justification::topLeft);
        if(!p.acceptsMidi()) {
            addAndMakeVisible(slider); slider.setRange(0,2,0.001); slider.setValue(p.gain(),juce::dontSendNotification);
            slider.setTextBoxStyle(juce::Slider::TextBoxRight,false,70,24);
            slider.onDragStart=[this]{p.getParameters()[0]->beginChangeGesture();};
            slider.onDragEnd=[this]{p.getParameters()[0]->endChangeGesture();};
            slider.onValueChange=[this]{p.setGain((float)slider.getValue());};
        }
        setSize(560,240); timerCallback(); startTimerHz(4);
    }
    void resized() override { info.setBounds(12,12,getWidth()-24,165); slider.setBounds(12,185,getWidth()-24,35); }
private:
    void timerCallback() override {
        // Plain ASCII only: a non-ASCII literal compiled under a different code
        // page shows as mojibake in the host, as seen on Windows.
        info.setText(p.getName()+" - private prototype\n"+p.moduleID()+" / 0.0.1\nSHA256 "+p.moduleHash()+"\nCmajor 1.0.3209\n"+p.status(),juce::dontSendNotification);
        if(!slider.isMouseButtonDown()) slider.setValue(p.gain(),juce::dontSendNotification);
    }
};
}
