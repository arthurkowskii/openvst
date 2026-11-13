#!/usr/bin/env python3
"""Real native engine renders; analytic fixtures are expectations, never audio substitutes."""
import array
import hashlib
import json
import math
import os
import pathlib
import platform
import resource
import struct
import subprocess
import sys
import time
import wave

ROOT = pathlib.Path(__file__).resolve().parents[2]
OUT = ROOT / 'artifacts/runtime'
EXE = OUT / 'bin/runtime_harness'
LIB = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / 'research/toolchain/1.0.3209/linux/x64/libCmajPerformer.so'
MODULES = ROOT / 'spikes/runtime/modules'
BLOCKS = [1, 16, 32, 64, 128, 512, 0]
report = {'status': 'running', 'commands': [], 'renders': [], 'checks': {}, 'failures': [], 'blocks': BLOCKS, 'irregular_pattern': [7,1,113,32,509,16,3]}

def bounded():
    resource.setrlimit(resource.RLIMIT_AS, (2*1024**3, 2*1024**3))
    resource.setrlimit(resource.RLIMIT_CPU, (40, 40))

def command(args, timeout=60):
    start = time.perf_counter()
    p = subprocess.run([str(a) for a in args], cwd=ROOT, capture_output=True, text=True, timeout=timeout, preexec_fn=bounded)
    report['commands'].append({'argv': [str(a) for a in args], 'returncode': p.returncode, 'wall_ms': (time.perf_counter()-start)*1000, 'stdout': p.stdout, 'stderr': p.stderr})
    return p

def check(name, condition, **metrics):
    report['checks'][name] = {'pass': bool(condition), **metrics}
    if not condition:
        raise AssertionError(name)

def load_audio(prefix):
    a = array.array('f')
    a.frombytes(prefix.with_suffix('.f32').read_bytes())
    if sys.byteorder != 'little': a.byteswap()
    return a

def render(module, block, mask=0):
    name = f'{module}_b{block}_v{mask}'
    prefix = OUT / 'renders' / name
    p = command([EXE, LIB, MODULES / f'{module}.cmajorpatch', block, prefix, name, mask])
    if p.returncode: raise RuntimeError(f'{name}: {p.stderr}')
    info = json.loads(prefix.with_suffix('.json').read_text())
    samples = load_audio(prefix)
    if len(samples) != 2*info['frames'] or not all(map(math.isfinite, samples)):
        raise AssertionError(f'{name}: invalid sample buffer')
    info['name'] = name
    info['duration_seconds'] = info['frames']/48000
    info['render_to_audio_ratio'] = info['render_ms']/(1000*info['duration_seconds'])
    info['peak_L'] = max(map(abs, samples[0::2]))
    info['peak_R'] = max(map(abs, samples[1::2]))
    info['rms_L'] = math.sqrt(sum(x*x for x in samples[0::2])/info['frames'])
    info['raw_sha256'] = hashlib.sha256(prefix.with_suffix('.f32').read_bytes()).hexdigest()
    report['renders'].append(info)
    return samples

def maxdiff(a,b): return max(abs(x-y) for x,y in zip(a,b))

def wav(name, samples):
    # PCM24 WAV, quantised directly from native float output, no normalisation.
    pcm = bytearray()
    for x in samples:
        v = max(-8388608, min(8388607, round(x*8388608)))
        pcm.extend(int(v).to_bytes(3, 'little', signed=True))
    with wave.open(str(OUT / name), 'wb') as w:
        w.setnchannels(2); w.setsampwidth(3); w.setframerate(48000); w.writeframes(pcm)
    with wave.open(str(OUT / name), 'rb') as w:
        assert w.getnchannels() == 2 and w.getnframes()*2 == len(samples) and w.getframerate() == 48000


def rejected(name, source, stage, main='Bad'):
    folder = OUT / 'negative'; folder.mkdir(exist_ok=True)
    src = folder / f'{name}.cmajor'; src.write_text(source)
    manifest = folder / f'{name}.cmajorpatch'
    manifest.write_text(json.dumps({'CmajorVersion':1,'source':src.name,'mainProcessor':main,'isInstrument':False}))
    p = command([EXE, LIB, manifest, 64, folder / name, name])
    check(name, p.returncode != 0 and stage in p.stderr, returncode=p.returncode, diagnostic=p.stderr)

start = time.perf_counter()
try:
    (OUT / 'renders').mkdir(parents=True, exist_ok=True)
    report['environment'] = {'platform':platform.platform(), 'cpu': subprocess.check_output(['lscpu'], text=True), 'compiler': subprocess.check_output(['g++','--version'], text=True), 'library':str(LIB), 'library_sha256':hashlib.sha256(LIB.read_bytes()).hexdigest(), 'source_sha': subprocess.check_output(['git','-C',str(ROOT/'research/cmajor-release'),'rev-parse','HEAD'], text=True).strip(), 'choc_sha':subprocess.check_output(['git','-C',str(ROOT/'research/cmajor-release/include/choc'),'rev-parse','HEAD'], text=True).strip()}
    smoke = command([sys.executable, ROOT / 'tests/runtime/test_smoke.py'])
    check('smoke_GREEN', smoke.returncode == 0)
    gains = {b: render('stereo_gain',b) for b in BLOCKS}
    expected = []
    for n in range(12000):
        gain = 0.5 if n < 37 else 0.25 if n < 1001 else 1.5 if n < 8003 else 0.0
        expected.extend([((n%101)-50)/128*gain, ((n%67)-33)/256*gain])
    errors = {str(b):maxdiff(a,expected) for b,a in gains.items()}
    check('stereo_gain_analytical_and_exact_offsets', max(errors.values()) == 0.0, max_abs_error_by_block=errors, automation_frames=[0,37,1001,8003], LR_distinct=gains[64][0::2] != gains[64][1::2])
    synths = {b: render('note_grid',b) for b in BLOCKS}
    diffs = {str(b):maxdiff(a,synths[64]) for b,a in synths.items()}
    check('synth_block_invariance', max(diffs.values()) < 1e-6, max_abs_error_by_block=diffs)
    audio = synths[64]
    check('MIDI_onset_at_offset', max(map(abs,audio[:2*38])) == 0 and abs(audio[2*38]) > 1e-6, first_nonzero_frame=next(n for n in range(36000) if audio[2*n] != 0))
    tail = max(map(abs,audio[2*24520:]))
    check('MIDI_note_off_and_release_silence', tail == 0.0, tail_peak=tail, off_frames=[8033,24029], release_frames=480, note67_off='note-on velocity zero')
    solo = [render('note_grid',64,m) for m in (1,2,3,4)]
    summed = [sum(v) for v in zip(*solo)]
    super_error = maxdiff(audio,summed)
    check('polyphony_superposition_four_independent_renders', super_error < 5e-7, max_abs_error=super_error, chord_notes=[60,64,67])
    projection = {}
    for note in (60,64,67):
        freq = 440*2**((note-69)/12)
        segment = audio[2*13000:2*23000:2]
        re = sum(x*math.cos(2*math.pi*freq*n/48000) for n,x in enumerate(segment))
        im = sum(x*math.sin(2*math.pi*freq*n/48000) for n,x in enumerate(segment))
        projection[str(note)] = {'frequency_hz':freq,'projected_amplitude':2*math.hypot(re,im)/len(segment)}
    check('three_simultaneous_chord_frequencies', all(v['projected_amplitude'] > 0.05 for v in projection.values()), notes=projection)
    # Independent sine equation for sustained A4, no DSP-generated fixture.
    def f32(x): return struct.unpack('<f', struct.pack('<f', x))[0]
    amplitude = f32(f32(100*f32(1/127))*f32(0.08))
    sine_error = max(abs(audio[2*n]-amplitude*math.sin(2*math.pi*440*(n-37)/48000)) for n in range(38,8000))
    check('A4_analytical_sine', sine_error < 1e-6, max_abs_error=sine_error, expected_frequency_hz=440, expected_amplitude=amplitude)
    check('all_renders_finite_reset_and_api_buffers', all(r['reset_bit_exact'] and r['api_xruns']==0 for r in report['renders']), render_count=len(report['renders']), sentinel_guards='NaN before/after payload checked per subblock; not a security test')
    rejected('invalid_parse', 'processor Bad { this is invalid !!! }', 'parse:')
    rejected('invalid_load', 'processor Bad { output stream float32 x; void main() { loop { x <- 0.0f; advance(); } } }', 'load:', main='MissingProcessor')
    rejected('invalid_semantic_load', 'processor Bad { input stream float32<2> audioIn; input event float32 gain; output stream float32<2> audioOut; event gain (float32 x) {} void main() { loop { audioOut <- audioIn * missing; advance(); } } }', 'load:')
    rejected('invalid_link', 'processor Bad { input stream float32<2> audioIn; input event float32 gain; output stream float32<2> audioOut; event gain (float32 x) {} void main() { loop { audioOut <- audioIn; } } }', 'link:')
    rejected('invalid_endpoints', 'processor Bad { output stream float32 wrong; void main() { loop { wrong <- 0.0f; advance(); } } }', 'missing endpoint:')
    missing = command([EXE, OUT/'not-a-runtime.so', '--version'])
    check('invalid_runtime_path', missing.returncode != 0 and 'explicit existing file' in missing.stderr, diagnostic=missing.stderr)
    wav('stereo_gain.wav', gains[64]); wav('polyphonic_synth.wav',audio)
    report['wav'] = [{'path':str(OUT/n),'sha256':hashlib.sha256((OUT/n).read_bytes()).hexdigest()} for n in ('stereo_gain.wav','polyphonic_synth.wav')]
    report['status'] = 'passed'
except Exception as e:
    report['status'] = 'failed'; report['failures'].append(repr(e)); raise
finally:
    report['suite_wall_seconds'] = time.perf_counter()-start
    (OUT/'results.json').write_text(json.dumps(report, indent=2, ensure_ascii=False)+'\n')
    print(json.dumps({'status':report['status'],'checks':len(report['checks']),'renders':len(report['renders']),'failures':report['failures'],'wall_seconds':report['suite_wall_seconds']}))
