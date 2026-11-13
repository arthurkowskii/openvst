// SPDX-License-Identifier: AGPL-3.0-only
#include "Processor.h"
#include "TrustedDSP.h"
#include "NativeEditor.h"
namespace openvst {
Paths Paths::defaults() { return {OPENVST_RUNTIME,OPENVST_FIXTURES}; }
Processor::Processor(bool s, Paths p):AudioProcessor(s?BusesProperties().withOutput("Output",juce::AudioChannelSet::stereo(),true):BusesProperties().withInput("Input",juce::AudioChannelSet::stereo(),true).withOutput("Output",juce::AudioChannelSet::stereo(),true)),instrument(s),paths(std::move(p)) {
    if(!instrument) { gainParameter=new juce::AudioParameterFloat(juce::ParameterID{"openvst.fx.gain.v1",1},"Gain",juce::NormalisableRange<float>(0,2),1); addParameter(gainParameter); }
}
Processor::~Processor()=default;
const juce::String Processor::getName() const { return instrument?"OpenVST Instrument":"OpenVST FX"; }
juce::String Processor::moduleID() const { return instrument?"org.openvst.spike.note-grid":"org.openvst.spike.stereo-gain"; }
juce::String Processor::moduleHash() const { return instrument?trusted::noteHash:trusted::gainHash; }
void Processor::prepareToPlay(double rate,int) {
    // Prototype lifecycle: host must serialise prepare/release/reset against audio.
    // Compilation happens ONLY here, never in state setter or processBlock.
    prepared.store(false); dsp.reset();
    if(fault.load()==3) return; // A rejected restore is sticky until a valid restore.
    fault.store(1);
    try { dsp=std::make_unique<DSP>(instrument,paths,rate); prepared.store(true); fault.store(0); }
    catch(const std::exception& e) { juce::ScopedLock lock(errorLock); preparationError=e.what(); fault.store(2); }
}
void Processor::releaseResources() { prepared.store(false); dsp.reset(); if(fault.load()!=3) fault.store(1); }
void Processor::reset() { if(dsp && dsp->performer.reset()!=cmaj::Result::Ok) fault.store(4); }
bool Processor::ready() const { return fault.load()==0; }
juce::String Processor::status() const {
    switch(fault.load()) {
        case 0:return "Ready - trusted fixture only";
        case 1:return "Not prepared / released";
        case 2:{ juce::ScopedLock lock(errorLock); return "Preparation failed: "+preparationError; }
        case 3:return "State rejected: schema/module/version/hash/parameter mismatch; restore NOT successful";
        default:return "Cmajor runtime error; fallback active";
    }
}
float Processor::gain() const { return gainParameter?gainParameter->get():1.0f; }
void Processor::setGain(float value) { if(gainParameter && std::isfinite(value)) gainParameter->setValueNotifyingHost(gainParameter->convertTo0to1(juce::jlimit(0.0f,2.0f,value))); }
bool Processor::isBusesLayoutSupported(const BusesLayout& l) const { return l.getMainOutputChannelSet()==juce::AudioChannelSet::stereo() && l.getMainInputChannelSet()==(instrument?juce::AudioChannelSet::disabled():juce::AudioChannelSet::stereo()); }
juce::AudioProcessorEditor* Processor::createEditor() { return new NativeEditor(*this); }
void Processor::getStateInformation(juce::MemoryBlock& dest) {
    auto* o=new juce::DynamicObject;
    o->setProperty("schema",1); o->setProperty("variant",instrument?"instrument":"fx");
    o->setProperty("module",moduleID()); o->setProperty("version","0.0.1"); o->setProperty("sourceSHA256",moduleHash());
    o->setProperty("engine","cmajor-1.0.3209"); o->setProperty("parameterID",instrument?"":"openvst.fx.gain.v1");
    o->setProperty("gain",gain()); o->setProperty("restoreValid",fault.load()!=3);
    auto text=juce::JSON::toString(juce::var(o),true); dest.replaceAll(text.toRawUTF8(),text.getNumBytesAsUTF8());
}
void Processor::setStateInformation(const void* data,int size) {
    // Fixed schema only. Does not touch performer or filesystem. JSON parsing may
    // allocate: hosts must not call this on their real-time callback in this slice.
    if(!data || size<2 || size>4096) { fault.store(3); return; }
    juce::var v; auto parsed=juce::JSON::parse(juce::String::fromUTF8((const char*)data,size),v);
    auto* o=v.getDynamicObject();
    auto eq=[&](const char* key,const juce::String& expected){return v[key].isString() && v[key].toString()==expected;};
    const auto g=v["gain"];
    bool valid=parsed.wasOk() && o && o->getProperties().size()==9 && v["schema"].isInt() && (int)v["schema"]==1
        && eq("variant",instrument?"instrument":"fx") && eq("module",moduleID()) && eq("version","0.0.1")
        && eq("sourceSHA256",moduleHash()) && eq("engine","cmajor-1.0.3209") && eq("parameterID",instrument?"":"openvst.fx.gain.v1")
        && v["restoreValid"].isBool() && (bool)v["restoreValid"] && (g.isDouble() || g.isInt()) && std::isfinite((double)g) && (double)g>=0 && (double)g<=2 && (!instrument || (double)g==1);
    if(!valid) { fault.store(3); return; }
    setGain((float)(double)g); fault.store(prepared.load()?0:1);
}
void Processor::processBlock(juce::AudioBuffer<float>& buffer,juce::MidiBuffer& midi) {
    juce::ScopedNoDenormals noDenormals;
    const int frames=buffer.getNumSamples();
    if(!ready() || !dsp || buffer.getNumChannels()!=2) { if(instrument) buffer.clear(); midi.clear(); return; }
    if(frames==0) { midi.clear(); return; }
    auto& d=*dsp; auto& p=d.performer;
    auto ok=[](cmaj::Result r){return r==cmaj::Result::Ok;};
    auto fail=[&] { fault.store(4); if(instrument) buffer.clear(); midi.clear(); };
    if(!instrument && !ok(p.addInputEvent(d.event,0,gain()))) { fail(); return; }
    auto it=midi.cbegin(),last=midi.cend();
    int pos=0;
    while(pos<frames) {
        if(instrument) while(it!=last && (*it).samplePosition<=pos) {
            const auto e=*it; ++it;
            // This fixture implements channel-1 note on/off only; ignore SysEx,
            // transport and other non-note messages, without constructing Value.
            if(e.samplePosition<0 || e.numBytes!=3 || ((e.data[0]&0xf0)!=0x90 && (e.data[0]&0xf0)!=0x80)) continue;
            const int32_t packed=(int32_t(e.data[0])<<16)|(int32_t(e.data[1])<<8)|int32_t(e.data[2]);
            // Validated MIDI object has exactly one int32 member. Raw payload per
            // tag API avoids allocating a choc::value::Value on the audio thread.
            if(!ok(p.addInputEvent(d.event,0,static_cast<const void*>(&packed)))) { fail(); return; }
        }
        int end=std::min(frames,pos+512);
        if(instrument && it!=last) end=std::min(end,(*it).samplePosition);
        const int n=end-pos;
        if(n<=0 || !ok(p.setBlockSize((uint32_t)n))) { fail(); return; }
        if(!instrument) {
            for(int i=0;i<n;++i) { d.in[2*i]=buffer.getSample(0,pos+i); d.in[2*i+1]=buffer.getSample(1,pos+i); }
            if(!ok(p.setInputFrames(d.input,d.in.data(),(uint32_t)n))) { fail(); return; }
        }
        if(!ok(p.advance()) || !ok(p.copyOutputFrames(d.output,d.out.data(),(uint32_t)n))) { fail(); return; }
        const char* error=p.getRuntimeError();
        if((error && *error) || p.getXRuns()!=0) { fail(); return; }
        for(int i=0;i<2*n;++i) if(!std::isfinite(d.out[i])) { fail(); return; }
        for(int i=0;i<n;++i) { buffer.setSample(0,pos+i,d.out[2*i]); buffer.setSample(1,pos+i,d.out[2*i+1]); }
        pos=end;
    }
    midi.clear();
}
}
