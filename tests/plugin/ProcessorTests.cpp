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
// Value of an exposed host parameter by its permanent automation ID.
static float valueByID(Processor& pr,const char* pid) {
    for(int i=0;i<pr.getParameters().size();++i) {
        auto* withID=dynamic_cast<juce::AudioProcessorParameterWithID*>(pr.getParameters()[i]);
        if(withID && withID->paramID==pid) { auto* f=dynamic_cast<juce::AudioParameterFloat*>(pr.getParameters()[i]); check(f!=nullptr,"parameter must be a float parameter"); return f->get(); }
    }
    check(false,"parameter id not exposed by the union"); return -9999.0f;
}
int main() {
 try {
    Processor fx(false); check(!fx.supportsDoublePrecisionProcessing(),"double must be refused");
    check(fx.getParameters().size()==4,"static union: legacy gain + three transient shaper parameters");
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
    restored.setStateInformation(state.getData(),(int)state.getSize()-2); check(!restored.ready(),"truncated state rejected");
    // A rejected restore must mute: passing audio through would make a broken
    // project sound like a working plugin at default settings.
    { juce::AudioBuffer<float> b(2,64); juce::MidiBuffer m;
      for(int i=0;i<64;++i) { b.setSample(0,i,0.3f); b.setSample(1,i,-0.4f); }
      restored.processBlock(b,m);
      for(int i=0;i<64;++i) { near(b.getSample(0,i),0.0f,"rejected state mutes left"); near(b.getSample(1,i),0.0f,"rejected state mutes right"); }
      check(!restored.ready(),"rejected state stays rejected through audio"); }
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
    // --- Module registry: selection, defaults, refusal -----------------------------------
    check(fx.getParameters().size()==4 && fx.moduleID()=="org.openvst.spike.stereo-gain","default FX selection");
    check(!fx.setModuleID("org.example.transient-shaper"),"unknown id refused");
    check(!fx.setModuleID("org.openvst.spike.note-grid"),"cross-kind id refused");
    check(fx.moduleID()=="org.openvst.spike.stereo-gain","refusal keeps the current selection");
    check(fx.getParameters().size()==4,"gain parameter survives selection attempts");
    auto* idAfter=dynamic_cast<juce::AudioParameterFloat*>(fx.getParameters()[0]);
    check(idAfter && idAfter->paramID=="openvst.fx.gain.v1" && idAfter->getNormalisableRange().start==0 && idAfter->getNormalisableRange().end==2,"parameter identity unchanged");
    check(fx.setModuleID("org.openvst.spike.stereo-gain"),"known id accepted");
    fx.prepareToPlay(48000,64); check(fx.ready(),fx.status().toRawUTF8());
    fx.setGain(0.5f); block(fx,513,0.5f);
    std::cout<<"PASS explicit selection compiles and passes audio\n";
    fx.setGain(0.25f); juce::MemoryBlock fxSel; fx.getStateInformation(fxSel);
    check(juce::String(fxSel.toString()).contains("org.openvst.spike.stereo-gain"),"state carries selected module id");
    Processor fxSelRestored(false); fxSelRestored.setStateInformation(fxSel.getData(),(int)fxSel.getSize());
    check(fxSelRestored.moduleID()=="org.openvst.spike.stereo-gain" && fxSelRestored.moduleHash()==fx.moduleHash(),"module roundtrip");
    near(fxSelRestored.gain(),0.25f,"gain roundtrip with module");
    fxSelRestored.prepareToPlay(48000,64); check(fxSelRestored.ready(),"selected module prepares"); block(fxSelRestored,513,0.25f);
    std::cout<<"PASS state roundtrip with module selection\n";
    // Legacy state: no module field -> default module of this build.
    auto* legacyObj=new juce::DynamicObject;
    legacyObj->setProperty("schema",1); legacyObj->setProperty("variant","fx");
    legacyObj->setProperty("version","0.0.1"); legacyObj->setProperty("sourceSHA256",fx.moduleHash());
    legacyObj->setProperty("engine","cmajor-1.0.3209"); legacyObj->setProperty("parameterID","openvst.fx.gain.v1");
    legacyObj->setProperty("gain",0.625); legacyObj->setProperty("restoreValid",true);
    auto legacy=juce::JSON::toString(juce::var(legacyObj),true);
    Processor legacyFx(false); legacyFx.setStateInformation(legacy.toRawUTF8(),(int)legacy.getNumBytesAsUTF8());
    check(legacyFx.moduleID()=="org.openvst.spike.stereo-gain","state without module field falls back to default");
    near(legacyFx.gain(),0.625f,"legacy state gain");
    legacyFx.prepareToPlay(48000,64); check(legacyFx.ready(),"legacy state prepares"); block(legacyFx,513,0.625f);
    // Parameters the old schema never stated take their declared defaults.
    near(valueByID(legacyFx,"openvst.ts.attack.v1"),0.0f,"legacy state: ts attack default");
    near(valueByID(legacyFx,"openvst.ts.sustain.v1"),0.0f,"legacy state: ts sustain default");
    near(valueByID(legacyFx,"openvst.ts.gain.v1"),1.0f,"legacy state: ts output gain default");
    std::cout<<"PASS legacy state without module field restores default module with per-module defaults\n";
    // Unknown module id in state: refused, never substituted.
    auto bogus=juce::String(fxSel.toString()).replace("org.openvst.spike.stereo-gain","org.example.transient-shaper");
    Processor fxReject(false); fxReject.setStateInformation(bogus.toRawUTF8(),(int)bogus.getNumBytesAsUTF8());
    check(!fxReject.ready(),"unknown module id in state refused");
    check(fxReject.moduleID()=="org.openvst.spike.stereo-gain","refused state keeps default selection");
    auto crossKind=juce::String(fxSel.toString()).replace("org.openvst.spike.stereo-gain","org.openvst.spike.note-grid");
    Processor fxKind(false); fxKind.setStateInformation(crossKind.toRawUTF8(),(int)crossKind.getNumBytesAsUTF8());
    check(!fxKind.ready(),"wrong-kind module id in state refused");
    std::cout<<"PASS unknown and wrong-kind ids refused explicitly\n";
    // Instrument build: default from legacy state, unknown id refused.
    auto* legacyInsObj=new juce::DynamicObject;
    legacyInsObj->setProperty("schema",1); legacyInsObj->setProperty("variant","instrument");
    legacyInsObj->setProperty("version","0.0.1"); legacyInsObj->setProperty("sourceSHA256",ins.moduleHash());
    legacyInsObj->setProperty("engine","cmajor-1.0.3209"); legacyInsObj->setProperty("parameterID","");
    legacyInsObj->setProperty("gain",1); legacyInsObj->setProperty("restoreValid",true);
    auto legacyInsState=juce::JSON::toString(juce::var(legacyInsObj),true);
    Processor legacyIns(true); legacyIns.setStateInformation(legacyInsState.toRawUTF8(),(int)legacyInsState.getNumBytesAsUTF8());
    check(legacyIns.moduleID()=="org.openvst.spike.note-grid","instrument legacy state defaults to note-grid");
    legacyIns.prepareToPlay(48000,64); check(legacyIns.ready(),"instrument legacy state prepares");
    auto insBogus=juce::String(juce::MemoryBlock(instrumentState).toString()).replace("org.openvst.spike.note-grid","org.example.ghost");
    Processor insReject(true); insReject.setStateInformation(insBogus.toRawUTF8(),(int)insBogus.getNumBytesAsUTF8());
    check(!insReject.ready(),"unknown instrument module refused");
    std::cout<<"PASS instrument legacy default and refusal\n";
    // --- Per-module parameters: union, transient shaper, refusal --------------------------
    const char* fxIDs[]={"openvst.fx.gain.v1","openvst.ts.attack.v1","openvst.ts.sustain.v1","openvst.ts.gain.v1"};
    for(int i=0;i<4;++i) { auto* pid=dynamic_cast<juce::AudioProcessorParameterWithID*>(fx.getParameters()[i]); check(pid && pid->paramID==fxIDs[i],"union position frozen for automation"); }
    auto* tsInfo=findModule("org.openvst.spike.transient-shaper");
    check(tsInfo && tsInfo->paramCount==3 && juce::String(tsInfo->mainProcessor)=="TransientShaper","transient shaper registered as an effect");
    check(tsInfo && juce::String(tsInfo->sourceSHA256)=="27ee6305dba4fc46cf011298debe7889ae93d707221d4f6fc626006d3c6d8c32"
        && juce::String(tsInfo->manifestSHA256)=="d87cd4f2f719d2109adb53c3b398757f39d1a17acd258172c74201ac2696b428","transient shaper hashes match TRANSIENT_REPORT");
    // Legacy automation of openvst.fx.gain.v1 on stereo-gain: same analytic
    // render as before this change, through the union's frozen position 0.
    fx.setParamValue(1,12.0f); fx.setParamValue(2,-12.0f); // inactive ts params must stay without effect
    fx.setGain(0.5f); block(fx,513,0.5f);
    fx.setParamValue(0,0.25f); block(fx,513,0.25f); // same parameter through the generic path
    std::cout<<"PASS openvst.fx.gain.v1 automation unchanged, inactive parameters without effect\n";
    // Selecting the transient shaper exposes its parameters and compiles.
    Processor tsFx(false); tsFx.prepareToPlay(48000,64); check(tsFx.ready(),tsFx.status().toRawUTF8());
    check(tsFx.setModuleID("org.openvst.spike.transient-shaper"),"transient shaper selectable by identity");
    tsFx.reprepare(); check(tsFx.ready(),tsFx.status().toRawUTF8());
    check(tsFx.getParameters().size()==4,"union never reshapes when the module changes");
    near(valueByID(tsFx,"openvst.fx.gain.v1"),1.0f,"inactive gain parameter stays present at its default");
    // Neutral at declared defaults: exact passthrough (spike-measured 0.0 error).
    {
        juce::AudioBuffer<float> neutral(2,513); juce::MidiBuffer m;
        for(int i=0;i<513;++i) { neutral.setSample(0,i,0.25f); neutral.setSample(1,i,-0.75f); }
        tsFx.processBlock(neutral,m);
        for(int i=0;i<513;++i) { near(neutral.getSample(0,i),0.25f,"ts neutral left"); near(neutral.getSample(1,i),-0.75f,"ts neutral right"); }
    }
    // Attack boost on an onset; sustain cut on a decaying body; deterministic reset.
    {
        auto onset=[&](juce::AudioBuffer<float>& b){ b.clear(); for(int i=64;i<1536;++i) { b.setSample(0,i,0.5f); b.setSample(1,i,0.5f); } };
        juce::AudioBuffer<float> neutral(2,1536), shaped(2,1536), again(2,1536); juce::MidiBuffer m;
        onset(neutral); tsFx.reset(); tsFx.processBlock(neutral,m);
        tsFx.setParamValue(1,6.0f); // openvst.ts.attack.v1 = +6 dB
        onset(shaped); tsFx.reset(); tsFx.processBlock(shaped,m);
        onset(again); tsFx.reset(); tsFx.processBlock(again,m);
        for(int c=0;c<2;++c) for(int i=0;i<1536;++i) check(again.getSample(c,i)==shaped.getSample(c,i),"ts render determinism");
        float peakN=0,peakS=0;
        for(int i=64;i<400;++i) { peakN=std::max(peakN,neutral.getSample(0,i)); peakS=std::max(peakS,shaped.getSample(0,i)); }
        check(peakS>peakN+0.05f && peakS<=1.0f,"attack parameter boosts the onset, bounded");
        tsFx.setParamValue(1,0.0f);
        juce::AudioBuffer<float> decayA(2,1536),decayB(2,1536); decayA.clear(); decayB.clear();
        for(int i=64;i<1536;++i) { float v=0.5f*std::exp(-(float)(i-64)/300.0f); decayA.setSample(0,i,v); decayA.setSample(1,i,v); decayB.setSample(0,i,v); decayB.setSample(1,i,v); }
        tsFx.reset(); tsFx.processBlock(decayA,m); // neutral sustain (0 dB): decay untouched
        tsFx.setParamValue(2,-6.0f); // openvst.ts.sustain.v1 = -6 dB
        tsFx.reset(); tsFx.processBlock(decayB,m); // body attenuated
        double rmsA=0,rmsB=0;
        for(int i=800;i<1500;++i) { double a=decayA.getSample(0,i),b=decayB.getSample(0,i); rmsA+=a*a; rmsB+=b*b; }
        check(std::sqrt(rmsB)<std::sqrt(rmsA)*0.9,"sustain parameter attenuates the decaying body");
        // ts parameters drive audio: attack must not be stuck at neutral.
        check(peakS>0.55f,"attack event endpoint actually reached the DSP");
    }
    std::cout<<"PASS transient shaper neutral, attack boost, sustain cut, determinism\n";
    // State roundtrip for the active module's parameters (schema 2).
    // Distinct non-default value for each of the module's three parameters, so a
    // roundtrip that silently keeps the defaults cannot pass.
    tsFx.setParamValue(1,9.0f);  // openvst.ts.attack.v1
    tsFx.setParamValue(2,-6.0f); // openvst.ts.sustain.v1
    tsFx.setParamValue(3,1.5f);  // openvst.ts.gain.v1
    juce::MemoryBlock tsState; tsFx.getStateInformation(tsState);
    auto tsText=tsState.toString();
    check(tsText.contains("openvst.ts.attack.v1") && tsText.contains("openvst.ts.sustain.v1") && tsText.contains("openvst.ts.gain.v1"),"state carries the active module's parameter values");
    check(!tsText.contains("/home/") && !tsText.contains("token"),"state path leak");
    Processor tsRestored(false); tsRestored.setStateInformation(tsState.getData(),(int)tsState.getSize());
    check(tsRestored.moduleID()=="org.openvst.spike.transient-shaper","ts state restores module identity");
    near(valueByID(tsRestored,"openvst.ts.attack.v1"),9.0f,"ts attack roundtrip");
    near(valueByID(tsRestored,"openvst.ts.gain.v1"),1.5f,"ts output gain roundtrip");
    near(valueByID(tsRestored,"openvst.ts.sustain.v1"),-6.0f,"ts sustain roundtrip");
    near(valueByID(tsRestored,"openvst.fx.gain.v1"),1.0f,"inactive gain parameter back to default after restore");
    tsRestored.prepareToPlay(48000,64); check(tsRestored.ready(),"ts state prepares");
    // One fresh document per case: reusing a DynamicObject after replacing one of
    // its properties frees the previous child while a raw pointer to it is still
    // held, which is exactly the kind of use-after-free this test is meant to
    // catch, not to commit.
    auto fxState=[&](const char* paramID,double value) {
        auto* o=new juce::DynamicObject;
        juce::var doc(o); // owns the object, releases the whole tree at scope exit
        o->setProperty("schema",2); o->setProperty("variant","fx");
        o->setProperty("module","org.openvst.spike.transient-shaper"); o->setProperty("version","0.0.1");
        o->setProperty("sourceSHA256",tsInfo->sourceSHA256); o->setProperty("engine","cmajor-1.0.3209");
        o->setProperty("restoreValid",true);
        auto* p=new juce::DynamicObject; p->setProperty(paramID,value);
        o->setProperty("params",juce::var(p));
        return juce::JSON::toString(doc,true);
    };
    // Out-of-range value: refused, never corrected in silence. Fault is sticky.
    auto bad=fxState("openvst.ts.attack.v1",13.0);
    Processor rangeFx(false); rangeFx.prepareToPlay(48000,64); check(rangeFx.ready(),"prepare before refusal");
    rangeFx.setStateInformation(bad.toRawUTF8(),(int)bad.getNumBytesAsUTF8());
    check(!rangeFx.ready(),"out-of-range parameter value refused");
    check(rangeFx.moduleID()=="org.openvst.spike.stereo-gain","refused restore keeps the current selection");
    rangeFx.prepareToPlay(48000,64); check(!rangeFx.ready(),"prepare must not hide a refused restore");
    // A known non-zero input makes the mutation visible: a refused restore must
    // mute, not pass the signal through as if the plugin were running.
    { juce::AudioBuffer<float> silent(2,64); juce::MidiBuffer m; silent.clear();
      for(int i=0;i<64;++i) { silent.setSample(0,i,0.3f); silent.setSample(1,i,-0.4f); }
      rangeFx.processBlock(silent,m);
      for(int i=0;i<64;++i) { near(silent.getSample(0,i),0.0f,"refused restore mutes left"); near(silent.getSample(1,i),0.0f,"refused restore mutes right"); } }
    // Unknown parameter id in state: refused.
    auto unk=fxState("openvst.unknown.param.v1",1.0);
    rangeFx.setStateInformation(unk.toRawUTF8(),(int)unk.getNumBytesAsUTF8());
    check(!rangeFx.ready(),"unknown parameter id in state refused");
    // In-range value: accepted again, applied exactly.
    auto good=fxState("openvst.ts.attack.v1",12.0);
    rangeFx.setStateInformation(good.toRawUTF8(),(int)good.getNumBytesAsUTF8());
    // A refused restore released the engine, so a valid one leaves the plugin
    // unprepared on purpose: the host must prepare before audio resumes.
    rangeFx.prepareToPlay(48000,64);
    check(rangeFx.ready(),"in-range restore re-arms the plugin, prepare brings it back");
    near(valueByID(rangeFx,"openvst.ts.attack.v1"),12.0f,"in-range value applied exactly");
    std::cout<<"PASS per-module state roundtrip, out-of-range and unknown-id refusal\n";
    // --- end per-module parameter tests ---------------------------------------------------
    std::ofstream raw(PLUGIN_ARTIFACT_DIR "/instrument.f32",std::ios::binary); for(int i=0;i<2049;++i) for(int c=0;c<2;++c) { float f=b.getSample(c,i); raw.write((char*)&f,4); }
    for(int n : {0,1,7,511,513}) { juce::AudioBuffer<float> small(2,n); juce::MidiBuffer m; ins.processBlock(small,m); for(int c=0;c<2;++c) for(int i=0;i<n;++i) check(std::isfinite(small.getSample(c,i)),"finite variable synth"); }
    ins.releaseResources(); ins.prepareToPlay(44100,7); check(ins.ready(),"synth reprepare rate");
    Processor badIns(true,{"/nonexistent/runtime",Paths::defaults().fixtures}); badIns.prepareToPlay(48000,64); check(!badIns.ready(),"bad runtime must fail, no fake engine"); auto silent=synth(badIns,false); for(int c=0;c<2;++c) for(int i=0;i<2049;++i) near(silent.getSample(c,i),0,"failed synth silence");
    Processor badFx(false,{Paths::defaults().runtime,"/nonexistent/fixtures"}); badFx.prepareToPlay(48000,64); check(!badFx.ready(),"missing fixture"); block(badFx,64,1);
    std::cout<<"PASS MIDI frame37, chord, offs, velocityzero, stereo reference, finite, reset, lifecycle, failure fallback\nALL PROCESSOR TESTS PASSED\n";
    return 0;
 } catch(const std::exception& e) { std::cerr<<"FAIL "<<e.what()<<'\n'; return 1; }
}
