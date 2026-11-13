// SPDX-License-Identifier: AGPL-3.0-only
#include "Processor.h"
#include "TrustedDSP.h"
#include "NativeEditor.h"
#include <cstring>
namespace openvst {
Paths Paths::defaults() { return {OPENVST_RUNTIME,OPENVST_FIXTURES}; }
Processor::Processor(bool s, Paths p):AudioProcessor(s?BusesProperties().withOutput("Output",juce::AudioChannelSet::stereo(),true):BusesProperties().withInput("Input",juce::AudioChannelSet::stereo(),true).withOutput("Output",juce::AudioChannelSet::stereo(),true)),instrument(s),paths(std::move(p)),selectedModule(defaultModule(s)) {
    // The host-facing parameter set is the static union of every module of
    // this kind, in registry order, declared once and never reshaped: JUCE
    // cannot add or remove parameters at run time. A parameter of a module
    // that is not selected stays present and simply has no effect.
    hostParamCount=unionParamCount(instrument);
    for(int i=0;i<hostParamCount;++i) {
        const ParamInfo& info=*unionParam(instrument,i);
        auto* parameter=new juce::AudioParameterFloat(juce::ParameterID{info.paramID,i+1},info.name,juce::NormalisableRange<float>(info.min,info.max),info.def,info.unit);
        addParameter(parameter);
        paramObjects[(std::size_t)i]=parameter; paramIDs[(std::size_t)i]=info.paramID;
        if(cstrEq(info.paramID,"openvst.fx.gain.v1")) gainIndex=i;
    }
}
Processor::~Processor()=default;
const juce::String Processor::getName() const { return instrument?"OpenVST Instrument":"OpenVST FX"; }
juce::String Processor::moduleID() const { const ModuleInfo& m=*selectedModule.load(std::memory_order_relaxed); return m.id; }
juce::String Processor::moduleHash() const { const ModuleInfo& m=*selectedModule.load(std::memory_order_relaxed); return m.sourceSHA256; }
juce::String Processor::moduleVersion() const { const ModuleInfo& m=*selectedModule.load(std::memory_order_relaxed); return m.version; }
const ModuleInfo& Processor::module() const { return *selectedModule.load(std::memory_order_relaxed); }
int Processor::paramCount() const { return hostParamCount; }
const ParamInfo& Processor::paramInfo(int i) const { return *unionParam(instrument,i); }
float Processor::paramValue(int i) const { return (i>=0 && i<hostParamCount && paramObjects[(std::size_t)i]) ? paramObjects[(std::size_t)i]->get() : 0.0f; }
void Processor::setParamValue(int i,float v) {
    if(i<0 || i>=hostParamCount || !paramObjects[(std::size_t)i] || !std::isfinite(v)) return;
    const ParamInfo& info=*unionParam(instrument,i);
    auto* p=paramObjects[(std::size_t)i];
    p->setValueNotifyingHost(p->convertTo0to1(juce::jlimit(info.min,info.max,v)));
}
juce::AudioProcessorParameter* Processor::hostParameter(int i) const { return (i>=0 && i<hostParamCount) ? paramObjects[(std::size_t)i] : nullptr; }
int Processor::moduleParamHostIndex(int i) const {
    const ModuleInfo& m=module();
    if(i<0 || i>=m.paramCount) return -1;
    return unionParamIndex(instrument,m.params[i].paramID);
}
float Processor::moduleParamValue(int i) const { return paramValue(moduleParamHostIndex(i)); }
float Processor::gain() const { return gainIndex>=0 ? paramObjects[(std::size_t)gainIndex]->get() : 1.0f; }
void Processor::setGain(float value) {
    if(gainIndex<0 || !std::isfinite(value)) return;
    setParamValue(gainIndex,value);
}
void Processor::resetParamsToDefaults() {
    // Runs on the message thread during restore/selection; the legacy gain path
    // also used host notifications here, so this stays consistent with it.
    for(int i=0;i<hostParamCount;++i) {
        const ParamInfo& info=*unionParam(instrument,i);
        auto* p=paramObjects[(std::size_t)i];
        if(p) p->setValueNotifyingHost(p->convertTo0to1(info.def));
    }
}
bool Processor::setModuleID(const juce::String& id) {
    // Unknown or wrong-kind id: refused explicitly, never silently substituted.
    const ModuleInfo* m=findModule(id);
    if(!m || m->instrument!=instrument) return false;
    if(m==selectedModule.load(std::memory_order_relaxed)) return true;
    selectedModule.store(m,std::memory_order_relaxed);
    // The newly selected module starts from its own declared defaults; stale
    // values of a previous selection must not leak into it.
    for(int i=0;i<m->paramCount;++i) {
        const int u=unionParamIndex(instrument,m->params[i].paramID);
        if(u>=0) setParamValue(u,m->params[i].def);
    }
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
    try { const ModuleInfo& m=*selectedModule.load(std::memory_order_relaxed);
          // Resolve module parameter -> host union parameter once, on this
          // (non-audio) thread; the DSP then never compares strings.
          std::array<juce::AudioParameterFloat*,kMaxParams> host{};
          for(int i=0;i<m.paramCount;++i) {
              const int u=unionParamIndex(instrument,m.params[i].paramID);
              trusted::require(u>=0 && paramObjects[(std::size_t)u],"Module parameter missing from the static union: "+std::string(m.params[i].paramID));
              host[(std::size_t)i]=paramObjects[(std::size_t)u];
          }
          std::shared_ptr<DSP> fresh=std::make_shared<DSP>(m,paths,rate,host);
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
bool Processor::isBusesLayoutSupported(const BusesLayout& l) const { return l.getMainOutputChannelSet()==juce::AudioChannelSet::stereo() && l.getMainInputChannelSet()==(instrument?juce::AudioChannelSet::disabled():juce::AudioChannelSet::stereo()); }
juce::AudioProcessorEditor* Processor::createEditor() { return new NativeEditor(*this); }
void Processor::getStateInformation(juce::MemoryBlock& dest) {
    const ModuleInfo& m=module();
    auto* o=new juce::DynamicObject;
    // Schema 2 adds the active module's parameter values. Schema 1 (gain only)
    // remains readable; it never gains new writers.
    o->setProperty("schema",2); o->setProperty("variant",instrument?"instrument":"fx");
    o->setProperty("module",m.id); o->setProperty("version",m.version); o->setProperty("sourceSHA256",m.sourceSHA256);
    o->setProperty("engine","cmajor-1.0.3209"); o->setProperty("restoreValid",fault.load()!=3);
    auto* params=new juce::DynamicObject;
    for(int i=0;i<m.paramCount;++i) params->setProperty(m.params[i].paramID,paramValue(unionParamIndex(instrument,m.params[i].paramID)));
    o->setProperty("params",juce::var(params));
    auto text=juce::JSON::toString(juce::var(o),true); dest.replaceAll(text.toRawUTF8(),text.getNumBytesAsUTF8());
}
void Processor::setStateInformation(const void* data,int size) {
    // Fixed schemas only. Does not touch performer or filesystem. JSON parsing may
    // allocate: hosts must not call this on their real-time callback in this slice.
    if(!data || size<2 || size>4096) { fault.store(3); return; }
    juce::var v; auto parsed=juce::JSON::parse(juce::String::fromUTF8((const char*)data,size),v);
    auto* o=v.getDynamicObject();
    auto eq=[&](const char* key,const juce::String& expected){return v[key].isString() && v[key].toString()==expected;};
    // Selection comes from the state itself. A state with no module field is a
    // legacy state: it restores to the default module of this build. A state
    // with an unknown or wrong-kind module id is refused, never substituted.
    const auto moduleVar=v["module"];
    const bool hasModule=moduleVar.isString();
    const bool moduleAbsent=moduleVar.isVoid();
    const ModuleInfo* m=hasModule?findModule(moduleVar.toString()):(moduleAbsent?defaultModule(instrument):nullptr);
    bool valid=parsed.wasOk() && o && m && m->instrument==instrument
        && v["schema"].isInt() && ((int)v["schema"]==1 || (int)v["schema"]==2)
        && eq("variant",instrument?"instrument":"fx") && eq("version",m->version)
        && eq("sourceSHA256",m->sourceSHA256) && eq("engine","cmajor-1.0.3209")
        && v["restoreValid"].isBool() && (bool)v["restoreValid"];
    const int schema=valid?(int)v["schema"]:0;
    // Parameter values: schema 2 carries a map of paramID -> value for the
    // active module. Anything unknown, non-numeric, non-finite or out of the
    // module's declared range is refused, never corrected in silence.
    const auto paramsVar=v["params"];
    juce::DynamicObject* params=nullptr;
    if(valid && schema==2) {
        if(paramsVar.isVoid()) { /* absent: every parameter takes its default */ }
        else if((params=paramsVar.getDynamicObject())==nullptr) valid=false;
        else for(const auto& entry : params->getProperties()) {
            const juce::String key=entry.name.toString();
            bool known=false; float min=0,max=0;
            for(int i=0;i<m->paramCount;++i) if(key==m->params[i].paramID) { known=true; min=m->params[i].min; max=m->params[i].max; break; }
            const auto& value=entry.value;
            if(!known || !(value.isDouble() || value.isInt()) || !std::isfinite((double)value) || (double)value<min || (double)value>max) { valid=false; break; }
        }
    }
    const auto g=v["gain"];
    if(valid && schema==1) {
        // Legacy schema: exactly the old document, gain in the legacy 0..2 range.
        valid=o->getProperties().size()==(hasModule?9:(moduleAbsent?8:0))
            && eq("parameterID",instrument?"":"openvst.fx.gain.v1")
            && (g.isDouble() || g.isInt()) && std::isfinite((double)g) && (double)g>=0 && (double)g<=2 && (!instrument || (double)g==1);
    }
    if(!valid) { fault.store(3); return; }
    const ModuleInfo* previous=selectedModule.load(std::memory_order_relaxed);
    selectedModule.store(m,std::memory_order_relaxed);
    // Everything not stated by the document falls back to its declared default,
    // so an old single-gain state restores a clean per-module parameter set.
    resetParamsToDefaults();
    if(schema==2) {
        if(params!=nullptr) for(const auto& entry : params->getProperties()) {
            const int u=unionParamIndex(instrument,entry.name.toString().toRawUTF8());
            if(u>=0) setParamValue(u,(float)(double)entry.value);
        }
    } else setGain((float)(double)g); // schema 1: the gain value is the only stated value
    if(m!=previous && prepared.load()) { prepared.store(false); fault.store(1); } // recompile at next prepare
    else fault.store(prepared.load()?0:1);
}
void Processor::processBlock(juce::AudioBuffer<float>& buffer,juce::MidiBuffer& midi) {
    juce::ScopedNoDenormals noDenormals;
    const int frames=buffer.getNumSamples();
    // One reference for the whole block: a concurrent module swap cannot free the
    // DSP while this callback is using it.
    auto held=std::atomic_load(&dsp);
    // A refused restore must never look like a working plugin: mute instead of
    // passing unprocessed audio through. Before the first prepare the FX stays
    // transparent, so a host that probes it gets its own signal back.
    if(fault.load()==3) { buffer.clear(); midi.clear(); return; }
    if(!ready() || !held || buffer.getNumChannels()!=2) { if(instrument) buffer.clear(); midi.clear(); return; }
    if(frames==0) { midi.clear(); return; }
    auto& d=*held; auto& p=d.performer;
    auto ok=[](cmaj::Result r){return r==cmaj::Result::Ok;};
    auto fail=[&] { fault.store(4); if(instrument) buffer.clear(); midi.clear(); };
    // Every parameter of the active module is sent at the top of the call;
    // parameters of inactive modules are never sent (they are present for the
    // host but have no effect). get() reads the denormalised value from an
    // atomic: no allocation, no locks.
    for(int i=0;i<d.paramCount;++i)
        if(!ok(p.addInputEvent(d.paramEvents[(std::size_t)i],0,d.hostParams[(std::size_t)i]->get()))) { fail(); return; }
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
            if(!ok(p.addInputEvent(d.midiEvent,0,static_cast<const void*>(&packed)))) { fail(); return; }
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
