// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <cmajor/API/cmaj_Engine.h>
#include <juce_cryptography/juce_cryptography.h>
#include <array>
#include <mutex>
#include <cmath>
namespace openvst {
namespace trusted {
inline constexpr const char* gainHash="36b6af6e18af5658a21515d1395cb713c548f7b5be14cd78c1c23e9906200309";
inline constexpr const char* noteHash="cf8940610b1ecd410de5998e006e5e69d647f702b29690e4bc670fd4daa499fe";
inline void require(bool ok,const std::string& s) { if(!ok) throw std::runtime_error(s); }
inline juce::MemoryBlock verified(const juce::File& f,const char* hash) {
    juce::MemoryBlock b; require(f.existsAsFile() && f.getSize()<65536 && f.loadFileAsData(b),"Missing/oversized trusted fixture: "+f.getFullPathName().toStdString());
    require(juce::SHA256(b).toHexString()==hash,"Trusted fixture SHA256 mismatch: "+f.getFileName().toStdString()); return b;
}
}
struct Processor::DSP {
    static constexpr int kMaxParams=kMaxParamsPerModule;
    cmaj::Engine engine;
    cmaj::Performer performer;
    cmaj::EndpointHandle input=0,midiEvent=0,output=0;
    // Parameter plumbing is resolved here, at preparation, from the module's
    // own table: no per-module special case anywhere else. hostParams[i] is
    // the host-facing parameter whose automation drives paramEvents[i]; both
    // are fixed at prepare time, so processBlock only reads them.
    std::array<cmaj::EndpointHandle,kMaxParams> paramEvents{};
    // AudioParameterFloat (not the AudioProcessorParameter base): its get()
    // returns the denormalised value straight from an atomic, allocation-free.
    std::array<juce::AudioParameterFloat*,kMaxParams> hostParams{};
    std::array<float,1024> in{},out{};
    int paramCount=0;
    explicit DSP(const ModuleInfo& m,const Paths& paths,double rate,const std::array<juce::AudioParameterFloat*,kMaxParams>& host) {
        // Cmajor's DLL singleton is not a loader for arbitrarily switchable engines.
        // Serialise initialisation and compilation; retain it for this process lifetime.
        static std::mutex compilerMutex;
        static juce::String loadedPath;
        std::lock_guard<std::mutex> lock(compilerMutex);
        const bool synth=m.instrument;
        trusted::require(juce::File::isAbsolutePath(paths.runtime) && juce::File(paths.runtime).existsAsFile(),"Explicit runtime file missing");
        trusted::require(loadedPath.isEmpty() || loadedPath==paths.runtime,"Runtime cannot change within this process");
        trusted::require(cmaj::Library::initialise(paths.runtime.toStdString()),"dlopen Cmajor release failed");
        loadedPath=paths.runtime;
        trusted::require(std::string(cmaj::Library::getVersion())=="1.0.3209","Cmajor version mismatch");
        trusted::require(std::isfinite(rate) && rate>=8000 && rate<=384000,"Unsupported sample rate");
        trusted::require(juce::File::isAbsolutePath(paths.fixtures),"Fixture directory must be explicit/absolute");
        const auto root=juce::File(paths.fixtures);
        auto source=root.getChildFile(m.sourceFile);
        auto manifest=trusted::verified(root.getChildFile(m.manifestFile),m.manifestSHA256);
        (void)manifest; // Exact frozen manifests, never interpreted as arbitrary paths.
        auto bytes=trusted::verified(source,m.sourceSHA256);
        cmaj::Program program; cmaj::DiagnosticMessageList diag;
        trusted::require(program.parse(diag,source.getFileName().toStdString(),std::string((const char*)bytes.getData(),bytes.getSize())),"parse: "+diag.toString());
        engine=cmaj::Engine::create(); trusted::require(bool(engine),"Cmajor engine creation failed");
        cmaj::BuildSettings settings; settings.setFrequency(rate).setMaxBlockSize(512).setMainProcessor(m.mainProcessor); engine.setBuildSettings(settings);
        trusted::require(engine.load(diag,program,{},{}),"load: "+diag.toString());
        auto inputs=engine.getInputEndpoints(),outputs=engine.getOutputEndpoints();
        auto endpoint=[](const cmaj::EndpointDetailsList& list,const char* id)->const cmaj::EndpointDetails& { for(auto& e:list) if(e.endpointID.toString()==id) return e; throw std::runtime_error(std::string("Missing endpoint: ")+id); };
        const auto stereo=choc::value::Type::createVectorFloat32(2);
        auto audio=[&](const cmaj::EndpointDetails& e){return e.isStream() && e.dataTypes.size()==1 && e.dataTypes[0]==stereo;};
        trusted::require(outputs.size()==1 && audio(endpoint(outputs,"audioOut")),"Output must be stereo float32 stream");
        output=engine.getEndpointHandle("audioOut");
        if(synth) {
            auto& e=endpoint(inputs,"midiIn"); trusted::require(inputs.size()==1+m.paramCount && e.isEvent() && e.dataTypes.size()==1 && e.isMIDI(),"MIDI endpoint must be single int32 Message object");
            midiEvent=engine.getEndpointHandle("midiIn");
        } else {
            trusted::require(inputs.size()==1+m.paramCount && audio(endpoint(inputs,"audioIn")),"FX endpoint contract mismatch");
            input=engine.getEndpointHandle("audioIn");
        }
        // Every declared parameter must exist as a float32 event endpoint, and
        // the patch must not carry any event/stream input the table ignores:
        // the count check above makes the table exhaustive for this module.
        paramCount=m.paramCount;
        for(int i=0;i<paramCount;++i) {
            const auto& p=m.params[i];
            auto& e=endpoint(inputs,p.endpoint);
            trusted::require(e.isEvent() && e.dataTypes.size()==1 && e.dataTypes[0].isFloat32(),"Parameter endpoint contract mismatch: "+std::string(p.endpoint));
            paramEvents[(std::size_t)i]=engine.getEndpointHandle(p.endpoint);
            hostParams[(std::size_t)i]=host[(std::size_t)i];
            trusted::require(paramEvents[(std::size_t)i] && hostParams[(std::size_t)i],"Unresolved parameter plumbing: "+std::string(p.endpoint));
        }
        trusted::require(output && (synth ? bool(midiEvent) : bool(input)),"Invalid endpoint handle");
        trusted::require(engine.link(diag),"link: "+diag.toString());
        performer=engine.createPerformer(); trusted::require(bool(performer) && performer.getMaximumBlockSize()>=512,"Performer maximum block contract");
        trusted::require(performer.getLatency()==0,"Unexpected fixture latency");
    }
};
}
