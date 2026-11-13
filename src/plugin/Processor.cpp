// SPDX-License-Identifier: AGPL-3.0-only
#include "Processor.h"
#include "TrustedDSP.h"
#include "NativeEditor.h"
namespace openvst {
Paths Paths::defaults() { return {OPENVST_RUNTIME,OPENVST_FIXTURES}; }
Processor::Processor(bool s, Paths p):AudioProcessor(s?BusesProperties().withOutput("Output",juce::AudioChannelSet::stereo(),true):BusesProperties().withInput("Input",juce::AudioChannelSet::stereo(),true).withOutput("Output",juce::AudioChannelSet::stereo(),true)),instrument(s),paths(std::move(p)),selectedModule(defaultModule(s)) {
    if(!instrument) { gainParameter=new juce::AudioParameterFloat(juce::ParameterID{"openvst.fx.gain.v1",1},"Gain",juce::NormalisableRange<float>(0,2),1); addParameter(gainParameter); }
}
Processor::~Processor()=default;
const juce::String Processor::getName() const { return instrument?"OpenVST Instrument":"OpenVST FX"; }
juce::String Processor::moduleID() const { const ModuleInfo& m=*selectedModule.load(std::memory_order_relaxed); return m.id; }
juce::String Processor::moduleHash() const { const ModuleInfo& m=*selectedModule.load(std::memory_order_relaxed); return m.sourceSHA256; }
juce::String Processor::moduleVersion() const { const ModuleInfo& m=*selectedModule.load(std::memory_order_relaxed); return m.version; }
const ModuleInfo& Processor::module() const { return *selectedModule.load(std::memory_order_relaxed); }
bool Processor::setModuleID(const juce::String& id) {
    // Unknown or wrong-kind id: refused explicitly, never silently substituted.
    const ModuleInfo* m=findModule(id);
    if(!m || m->instrument!=instrument) return false;
    if(m==selectedModule.load(std::memory_order_relaxed)) return true;
    selectedModule.store(m,std::memory_order_relaxed);
    prepared.store(false); fault.store(1); // compiled at the next prepareToPlay
    return true;
}
void Processor::reprepare() {
    const double rate=preparedRate.load(std::memory_order_relaxed);
    if(rate>0) prepareToPlay(rate,512);
}
void Processor::prepareToPlay(double rate,int) {
    // Prototype lifecycle: host must serialise prepare/release/reset against audio.
    // Compilation happens ONLY here, never in state setter or processBlock.
    // The pending selection is read once here, before any compilation.
    preparedRate.store(rate); prepared.store(false); std::atomic_store(&dsp,std::shared_ptr<DSP>{});
    if(fault.load()==3) return; // A rejected restore is sticky until a valid restore.
    fault.store(1);
    // Build first, publish after: a failed compilation never replaces a working
    // performer, and the old object is destroyed only when the audio thread is
    // done with it.
    try { std::shared_ptr<DSP> fresh=std::make_shared<DSP>(*selectedModule.load(std::memory_order_relaxed),paths,rate);
          std::atomic_store(&dsp,std::move(fresh)); prepared.store(true); fault.store(0); }
    catch(const std::exception& e) { juce::ScopedLock lock(errorLock); preparationError=e.what(); fault.store(2); }
}
void Processor::releaseResources() { prepared.store(false); std::atomic_store(&dsp,std::shared_ptr<DSP>{}); if(fault.load()!=3) fault.store(1); }
void Processor::reset() { auto d=std::atomic_load(&dsp); if(d && d->performer.reset()!=cmaj::Result::Ok) fault.store(4); }
bool Processor::ready() const { return fault.load()==0; }
juce::String Processor::status() const {
    switch(fault.load()) {
        case 0:return "Ready - "+moduleID();
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
    const ModuleInfo& m=module();
    auto* o=new juce::DynamicObject;
    o->setProperty("schema",1); o->setProperty("variant",instrument?"instrument":"fx");
    o->setProperty("module",m.id); o->setProperty("version",m.version); o->setProperty("sourceSHA256",m.sourceSHA256);
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
    // Selection comes from the state itself. A state with no module field is a
    // legacy state: it restores to the default module of this build. A state
    // with an unknown or wrong-kind module id is refused, never substituted.
    const auto moduleVar=v["module"];
    const bool hasModule=moduleVar.isString();
    const bool moduleAbsent=moduleVar.isVoid();
    const ModuleInfo* m=hasModule?findModule(moduleVar.toString()):(moduleAbsent?defaultModule(instrument):nullptr);
    bool valid=parsed.wasOk() && o && m && m->instrument==instrument
        && o->getProperties().size()==(hasModule?9:(moduleAbsent?8:0)) && v["schema"].isInt() && (int)v["schema"]==1
        && eq("variant",instrument?"instrument":"fx") && eq("version",m->version)
        && eq("sourceSHA256",m->sourceSHA256) && eq("engine","cmajor-1.0.3209") && eq("parameterID",instrument?"":"openvst.fx.gain.v1")
        && v["restoreValid"].isBool() && (bool)v["restoreValid"] && (g.isDouble() || g.isInt()) && std::isfinite((double)g) && (double)g>=0 && (double)g<=2 && (!instrument || (double)g==1);
    if(!valid) { fault.store(3); return; }
    const ModuleInfo* previous=selectedModule.load(std::memory_order_relaxed);
    selectedModule.store(m,std::memory_order_relaxed);
    setGain((float)(double)g);
    if(m!=previous && prepared.load()) { prepared.store(false); fault.store(1); } // recompile at next prepare
    else fault.store(prepared.load()?0:1);
}
void Processor::processBlock(juce::AudioBuffer<float>& buffer,juce::MidiBuffer& midi) {
    juce::ScopedNoDenormals noDenormals;
    const int frames=buffer.getNumSamples();
    // One reference for the whole block: a concurrent module swap cannot free the
    // DSP while this callback is using it.
    auto held=std::atomic_load(&dsp);
    if(!ready() || !held || buffer.getNumChannels()!=2) { if(instrument) buffer.clear(); midi.clear(); return; }
    if(frames==0) { midi.clear(); return; }
    auto& d=*held; auto& p=d.performer;
    auto ok=[](cmaj::Result r){return r==cmaj::Result::Ok;};
    auto fail=[&] { fault.store(4); if(instrument) buffer.clear(); midi.clear(); };
    if(!instrument && d.event && !ok(p.addInputEvent(d.event,0,gain()))) { fail(); return; }
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
