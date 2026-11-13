// SPDX-License-Identifier: GPL-3.0-or-later
// Native headless integration, using tag-matched Cmajor C++ API wrappers.
#define CMAJOR_DLL 1
#include <cmajor/API/cmaj_Engine.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <cmath>
#include <algorithm>
#include <array>
#include <cstring>

using Clock = std::chrono::steady_clock;
static double ms(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now()-t).count(); }
static void require(bool b, const std::string& m) { if (!b) throw std::runtime_error(m); }
static void check(cmaj::Result r) { require(r == cmaj::Result::Ok, "performer result=" + std::to_string(int(r))); }
static std::string read(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary); require(bool(f), "open: " + p.string());
    return {std::istreambuf_iterator<char>(f), {}};
}
struct Event { uint32_t frame; int note, velocity; float gain; };
struct Render { std::vector<float> samples; double time; uint32_t blocks; };

int main(int argc, char** argv) {
    try {
        require(argc >= 3, "usage: harness LIB --version | LIB MANIFEST BLOCK PREFIX SCENARIO [voice-mask=0]");
        require(std::filesystem::is_regular_file(argv[1]), "runtime path must be an explicit existing file");
        require(cmaj::Library::initialise(argv[1]), "dlopen runtime failed");
        std::string version = cmaj::Library::getVersion();
        require(version == "1.0.3209", "runtime version mismatch: " + version);
        if (std::string(argv[2]) == "--version") { std::cout << version << '\n'; return 0; }
        require(argc == 6 || argc == 7, "expected LIB MANIFEST BLOCK PREFIX [voice-mask]");
        int block = std::stoi(argv[3]);
        require(block >= 0 && block <= 512, "block must be 0 (irregular) or 1..512");
        int mask = argc == 7 ? std::stoi(argv[6]) : 0;
        require(mask >= 0 && mask <= 4, "voice-mask must be 0..4");
        auto manifest = choc::json::parse(read(argv[2]));
        require(manifest["CmajorVersion"].getWithDefault<int>(0) == 1, "manifest version");
        bool synth = manifest["isInstrument"].getWithDefault<bool>(false);
        auto source = std::filesystem::path(argv[2]).parent_path() / manifest["source"].get<std::string>();
        cmaj::DiagnosticMessageList diag;
        cmaj::Program program;
        auto compileStart = Clock::now();
        if (!program.parse(diag, source.string(), read(source))) throw std::runtime_error("parse: " + diag.toString());
        auto engine = cmaj::Engine::create(); require(bool(engine), "engine creation");
        cmaj::BuildSettings settings;
        settings.setFrequency(48000).setMaxBlockSize(512).setMainProcessor(manifest["mainProcessor"].get<std::string>());
        engine.setBuildSettings(settings);
        if (!engine.load(diag, program, {}, {})) throw std::runtime_error("load: " + diag.toString());
        auto inputs = engine.getInputEndpoints(), outputs = engine.getOutputEndpoints();
        auto endpoint = [&](const cmaj::EndpointDetailsList& list, const std::string& id) -> const cmaj::EndpointDetails& {
            for (auto& e : list) if (e.endpointID.toString() == id) return e;
            throw std::runtime_error("missing endpoint: " + id);
        };
        auto audioType = choc::value::Type::createVectorFloat32(2);
        auto& out = endpoint(outputs, "audioOut");
        require(outputs.size() == 1 && out.isStream() && out.dataTypes.size() == 1 && out.dataTypes[0] == audioType, "output must be float32<2> stream");
        auto outputHandle = engine.getEndpointHandle("audioOut");
        cmaj::EndpointHandle inputHandle = 0, eventHandle = 0;
        if (synth) {
            auto& e = endpoint(inputs, "midiIn");
            require(inputs.size() == 1 && e.isEvent() && e.dataTypes.size() == 1 && e.isMIDI(), "MIDI endpoint type");
            eventHandle = engine.getEndpointHandle("midiIn");
        } else {
            auto& a = endpoint(inputs, "audioIn"); auto& e = endpoint(inputs, "gain");
            require(inputs.size() == 2 && a.isStream() && a.dataTypes.size() == 1 && a.dataTypes[0] == audioType, "input audio type");
            require(e.isEvent() && e.dataTypes.size() == 1 && e.dataTypes[0].isFloat32(), "gain event type");
            inputHandle = engine.getEndpointHandle("audioIn"); eventHandle = engine.getEndpointHandle("gain");
        }
        require(outputHandle && eventHandle && (synth || inputHandle), "endpoint handles");
        if (!engine.link(diag)) throw std::runtime_error("link: " + diag.toString());
        double compileMs = ms(compileStart);
        auto performer = engine.createPerformer(); require(bool(performer), "performer creation");
        require(performer.getMaximumBlockSize() >= 512, "maximum block size");
        std::vector<Event> events;
        if (!synth) events = {{0,0,0,0.5f},{37,0,0,0.25f},{1001,0,0,1.5f},{8003,0,0,0.0f}};
        else {
            auto add = [&](int note, int bit, uint32_t on, uint32_t off) {
                if (mask == 0 || mask == bit) { events.push_back({on,note,100,0}); events.push_back({off,note,0,0}); }
            };
            add(69,4,37,8033); add(60,1,12017,24029); add(64,2,12017,24029); add(67,3,12017,24029);
            std::stable_sort(events.begin(), events.end(), [](auto& a, auto& b){ return a.frame < b.frame; });
        }
        uint32_t total = synth ? 36000 : 12000;
        auto render = [&]() -> Render {
            Render result; result.samples.resize(total*2); result.blocks = 0;
            std::array<float, 1024> input{};
            std::array<float, 1026> output{}; // one guard float before and after maximum payload
            std::array<uint32_t,7> irregular{7,1,113,32,509,16,3};
            uint32_t pos = 0, hostEnd = 0, hostIndex = 0; size_t eventIndex = 0;
            auto start = Clock::now();
            while (pos < total) {
                if (pos == hostEnd) hostEnd = std::min(total, pos + (block ? uint32_t(block) : irregular[hostIndex++ % irregular.size()]));
                uint32_t end = hostEnd;
                // Events at pos are sent before advancing exactly that sample; future
                // event offsets split the host block, never batch-shifted to its start.
                while (eventIndex < events.size() && events[eventIndex].frame == pos) {
                    auto e = events[eventIndex++];
                    if (synth) {
                        uint8_t status = e.velocity ? 0x90 : (e.note == 67 ? 0x90 : 0x80);
                        auto midi = cmaj::MIDIEvents::createMIDIMessageObject(choc::midi::ShortMessage(status, uint8_t(e.note), uint8_t(e.velocity)));
                        check(performer.addInputEvent(eventHandle, 0, midi));
                    } else check(performer.addInputEvent(eventHandle, 0, e.gain));
                }
                if (eventIndex < events.size()) end = std::min(end, events[eventIndex].frame);
                uint32_t n = end-pos; require(n && n <= 512, "scheduler progress");
                check(performer.setBlockSize(n));
                if (!synth) {
                    for (uint32_t i=0; i<n; ++i) {
                        input[2*i] = float(int((pos+i)%101)-50)/128.0f;
                        input[2*i+1] = float(int((pos+i)%67)-33)/256.0f;
                    }
                    check(performer.setInputFrames(inputHandle,input.data(),n));
                }
                output.fill(std::numeric_limits<float>::quiet_NaN());
                check(performer.advance()); check(performer.copyOutputFrames(outputHandle,output.data()+1,n));
                for (uint32_t i=1; i<=2*n; ++i) require(std::isfinite(output[i]), "nonfinite/unwritten output sample");
                require(std::isnan(output[0]) && std::all_of(output.begin()+1+2*n, output.end(), [](float f){ return std::isnan(f); }), "output overrun sentinel");
                std::copy_n(output.begin()+1,2*n,result.samples.begin()+2*pos);
                pos = end; ++result.blocks;
            }
            result.time = ms(start);
            require(performer.getXRuns() == 0, "API stream xrun");
            auto error = performer.getRuntimeError(); require(error == nullptr || *error == 0, error ? error : "runtime error");
            return result;
        };
        auto first = render(); check(performer.reset()); auto second = render();
        require(first.samples.size() == second.samples.size() && std::memcmp(first.samples.data(), second.samples.data(), first.samples.size()*sizeof(float)) == 0, "reset not bit deterministic");
        std::string prefix = argv[4];
        // argv[5] is an explicit scenario label, recorded for auditability.
        std::ofstream raw(prefix + ".f32",std::ios::binary);
        raw.write(reinterpret_cast<const char*>(first.samples.data()),std::streamsize(first.samples.size()*sizeof(float)));
        require(bool(raw), "write raw output");
        auto info = choc::json::create("version",version,"scenario",std::string(argv[5]),"sample_rate",48000,"frames",int(total),
            "compile_ms",compileMs,"render_ms",first.time,"reset_render_ms",second.time,"processed_subblocks",int(first.blocks),
            "reset_bit_exact",true,"finite_samples",int(first.samples.size()),"api_xruns",int(performer.getXRuns()),
            "inputs",inputs.toJSON(false),"outputs",outputs.toJSON(false));
        std::ofstream json(prefix + ".json"); json << choc::json::toString(info,true); require(bool(json), "write JSON");
        std::cout << choc::json::toString(info) << '\n';
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
