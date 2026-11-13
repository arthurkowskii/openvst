// SPDX-License-Identifier: AGPL-3.0-only
#include "Processor.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <fstream>
using namespace openvst;
static void check(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
static void near(float a, float b, const char* what) { check(std::isfinite(a) && std::abs(a-b)<2e-6f,what); }
static void block(Processor& p, int n, float g) {
    juce::AudioBuffer<float> b(2,n); juce::MidiBuffer midi;
    for(int i=0;i<n;++i) { b.setSample(0,i,0.25f); b.setSample(1,i,-0.75f); }
    p.processBlock(b,midi);
    for(int i=0;i<n;++i) { near(b.getSample(0,i),0.25f*g,"FX left"); near(b.getSample(1,i),-0.75f*g,"FX right"); }
}
static juce::AudioBuffer<float> synth(Processor& p, bool chord, int length=2049) {
    juce::AudioBuffer<float> b(2,length); b.clear(); juce::MidiBuffer m;
    m.addEvent(juce::MidiMessage::noteOn(1,60,(juce::uint8)100),37);
    if(chord) { m.addEvent(juce::MidiMessage::noteOn(1,64,(juce::uint8)80),37); m.addEvent(juce::MidiMessage::noteOn(1,67,(juce::uint8)90),37); }
    m.addEvent(juce::MidiMessage::noteOff(1,60),1001);
    if(chord) { m.addEvent(juce::MidiMessage::noteOff(1,64),1001); m.addEvent(juce::MidiMessage::noteOn(1,67,(juce::uint8)0),1001); }
    p.processBlock(b,m); return b;
}
int main() {
 try {
    Processor fx(false); check(!fx.supportsDoublePrecisionProcessing(),"double must be refused");
    check(fx.getParameters().size()==1,"one fixed gain");
    auto* id=dynamic_cast<juce::AudioProcessorParameterWithID*>(fx.getParameters()[0]);
    check(id && id->paramID=="openvst.fx.gain.v1","permanent parameter ID");
    fx.prepareToPlay(48000,2049); check(fx.ready(),fx.status().toRawUTF8());
    for(int n : {0,1,7,113,512,513,2049}) { fx.setGain(0.5f); block(fx,n,0.5f); fx.setGain(1.5f); block(fx,n,1.5f); }
    std::cout<<"PASS FX stereo, automation, zero/variable/large blocks\n";
    juce::MemoryBlock state; fx.setGain(0.375f); fx.getStateInformation(state);
    auto text=state.toString(); check(!text.contains("/home/") && !text.contains("runtime\"") && !text.contains("token"),"state path leak");
    check(text.contains(fx.moduleID()) && text.contains(fx.moduleHash()) && text.contains("1.0.3209") && text.contains("0.0.1"),"exact module identity");
    Processor restored(false); restored.setStateInformation(state.getData(),(int)state.getSize());
    near(restored.gain(),0.375f,"gain roundtrip before prepare"); restored.prepareToPlay(48000,64); check(restored.ready(),"restore prepare"); block(restored,513,0.375f);
    restored.setStateInformation(state.getData(),(int)state.getSize()-2); check(!restored.ready(),"truncated state rejected"); block(restored,64,1.0f);
    const char invalid[]="invalid"; restored.setStateInformation(invalid,7); check(!restored.ready(),"invalid state rejected");
    restored.setStateInformation(state.getData(),(int)state.getSize()); check(restored.ready(),"valid same-module restore no JIT"); block(restored,7,0.375f);
    auto wrong=text.replace(fx.moduleHash(),juce::String::repeatedString("0",64));
    restored.setStateInformation(wrong.toRawUTF8(),wrong.getNumBytesAsUTF8()); check(!restored.ready(),"wrong hash rejected");
    restored.prepareToPlay(48000,64); check(!restored.ready(),"prepare must not hide failed restore");
    std::cout<<"PASS versioned state, exact identity, invalid/truncated/hash mismatch and bypass\n";
    for(int i=0;i<3;++i) { fx.releaseResources(); check(!fx.ready(),"release"); fx.prepareToPlay(48000,64); check(fx.ready(),"reprepare"); fx.reset(); block(fx,513,0.375f); }
    Processor ins(true); check(ins.getParameters().isEmpty(),"no dynamic manifest params"); ins.prepareToPlay(48000,2049); check(ins.ready(),ins.status().toRawUTF8());
    auto b=synth(ins,true);
    for(int i=0;i<b.getNumSamples();++i) {
       double l=0,r=0;
       if(i>=37 && i<1481) for(auto nv : {std::pair<int,int>{60,100},{64,80},{67,90}}) {
          double amp=nv.second/127.0*0.08; if(i>=1001) amp*=std::max(0.0,1.0-(i-1001)/480.0);
          double phase=(i-37)*6.283185307179586*440.0*std::pow(2.0,(nv.first-69)/12.0)/48000.0;
          double s=std::sin(phase)*amp; l+=s; r+=s*(0.5+(nv.first%12)/24.0);
       }
       near(b.getSample(0,i),(float)l,"MIDI frame37/chord/off/velocityzero L"); near(b.getSample(1,i),(float)r,"MIDI stereo R");
    }
    ins.reset(); auto again=synth(ins,true); for(int c=0;c<2;++c) for(int i=0;i<2049;++i) near(b.getSample(c,i),again.getSample(c,i),"reset determinism");
    ins.reset();
    int position=0, index=0; const int sizes[]={0,1,7,113,513,3,1024};
    while(position<2049) {
        int count=std::min(sizes[index++%7],2049-position);
        juce::AudioBuffer<float> part(2,count); juce::MidiBuffer events;
        for(int frame : {37,1001}) if(frame>=position && frame<position+count) {
            if(frame==37) { events.addEvent(juce::MidiMessage::noteOn(1,60,(juce::uint8)100),frame-position); events.addEvent(juce::MidiMessage::noteOn(1,64,(juce::uint8)80),frame-position); events.addEvent(juce::MidiMessage::noteOn(1,67,(juce::uint8)90),frame-position); }
            else { events.addEvent(juce::MidiMessage::noteOff(1,60),frame-position); events.addEvent(juce::MidiMessage::noteOff(1,64),frame-position); events.addEvent(juce::MidiMessage::noteOn(1,67,(juce::uint8)0),frame-position); }
        }
        ins.processBlock(part,events);
        for(int c=0;c<2;++c) for(int i=0;i<count;++i) near(part.getSample(c,i),b.getSample(c,position+i),"MIDI host partition invariant");
        position+=count;
    }
    juce::MemoryBlock instrumentState; ins.getStateInformation(instrumentState);
    Processor insRestored(true); insRestored.setStateInformation(instrumentState.getData(),(int)instrumentState.getSize()); insRestored.prepareToPlay(48000,64); check(insRestored.ready(),"instrument state restore");
    auto recalled=synth(insRestored,true); for(int c=0;c<2;++c) for(int i=0;i<2049;++i) near(recalled.getSample(c,i),b.getSample(c,i),"instrument state render");
    insRestored.setStateInformation(state.getData(),(int)state.getSize()); check(!insRestored.ready(),"cross-variant rejected");
    std::cout<<"PASS instrument state, cross-variant rejection, host partition invariance\n";
    std::ofstream raw(PLUGIN_ARTIFACT_DIR "/instrument.f32",std::ios::binary); for(int i=0;i<2049;++i) for(int c=0;c<2;++c) { float f=b.getSample(c,i); raw.write((char*)&f,4); }
    for(int n : {0,1,7,511,513}) { juce::AudioBuffer<float> small(2,n); juce::MidiBuffer m; ins.processBlock(small,m); for(int c=0;c<2;++c) for(int i=0;i<n;++i) check(std::isfinite(small.getSample(c,i)),"finite variable synth"); }
    ins.releaseResources(); ins.prepareToPlay(44100,7); check(ins.ready(),"synth reprepare rate");
    Processor badIns(true,{"/nonexistent/runtime",Paths::defaults().fixtures}); badIns.prepareToPlay(48000,64); check(!badIns.ready(),"bad runtime must fail, no fake engine"); auto silent=synth(badIns,false); for(int c=0;c<2;++c) for(int i=0;i<2049;++i) near(silent.getSample(c,i),0,"failed synth silence");
    Processor badFx(false,{Paths::defaults().runtime,"/nonexistent/fixtures"}); badFx.prepareToPlay(48000,64); check(!badFx.ready(),"missing fixture"); block(badFx,64,1);
    std::cout<<"PASS MIDI frame37, chord, offs, velocityzero, stereo reference, finite, reset, lifecycle, failure fallback\nALL PROCESSOR TESTS PASSED\n";
    return 0;
 } catch(const std::exception& e) { std::cerr<<"FAIL "<<e.what()<<'\n'; return 1; }
}
