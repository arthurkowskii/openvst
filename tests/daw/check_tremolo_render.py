"""Check the REAPER render of the catalogue-installed Tremolo module.

The project was produced by injecting the plugin's own saved state through
REAPER's ``vst_chunk`` parameter, so this checker proves three separate things:

1. the saved project names the installed module and its real parameter values;
2. the host exposes those parameters on the permanent generic slots;
3. the rendered audio matches the module's analytic sine tremolo sample by
   sample, not merely "not silent".

Optionally compares the Windows render with the Linux one after 16-bit
quantisation.

Usage: check_tremolo_render.py [WINDOWS_WAV]
"""

from __future__ import annotations

import base64
import json
import math
import struct
import sys
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DAW = ROOT / "artifacts/daw"
MODULE = "org.openvst.demo.tremolo"
EXPECTED = {"speed": 5.0, "amount": 0.75, "level": 1.5}
# Host union for the FX build: gain, ts attack, ts sustain, ts gain, then the
# eight permanent generic slots. Tremolo maps onto slots 0..2 of that range.
GENERIC_BASE = 4
RANGES = {"speed": (0.1, 20.0), "amount": (0.0, 1.0), "level": (0.0, 2.0)}


def samples(path: Path) -> list[float]:
    with wave.open(str(path)) as stream:
        if stream.getnchannels() != 2 or stream.getframerate() != 48000:
            raise SystemExit(f"{path.name}: unexpected {stream.getparams()}")
        width = stream.getsampwidth()
        raw = stream.readframes(stream.getnframes())
    scale = float(1 << (8 * width - 1))
    return [int.from_bytes(raw[i:i + width], "little", signed=True) / scale
            for i in range(0, len(raw), width)]


def project_state(path: Path) -> dict:
    lines = path.read_text(encoding="utf-8", errors="surrogateescape").splitlines()
    start = next(i for i, line in enumerate(lines) if line.strip().startswith("<VST"))
    blocks = []
    for line in lines[start + 1:]:
        text = line.strip()
        if text.startswith(">"):
            break
        blocks.append(base64.b64decode(text))
    chunk = b"".join(blocks)
    # REAPER wraps the plugin state in its own chunk header; the container's
    # document is inside it, followed by JUCE's private data.
    start = chunk.find(b'{"schema"')
    if start < 0:
        raise SystemExit("project chunk holds no container state document")
    document, _ = json.JSONDecoder().raw_decode(chunk[start:].decode("utf-8"))
    return document


def chunk_document(chunk: bytes) -> dict:
    start = chunk.find(b'{"schema"')
    if start < 0:
        raise SystemExit("plugin chunk holds no container state document")
    document, _ = json.JSONDecoder().raw_decode(chunk[start:].decode("utf-8"))
    return document


def host_params(path: Path) -> dict[int, float]:
    values = {}
    for line in path.read_text().splitlines():
        index, _, rest = line.partition("\t")
        if rest:
            values[int(index)] = float(rest.rsplit("\t", 1)[1])
    return values


def expected_output(n: int, state: dict, input_left: float, input_right: float):
    """Cmajor's own arithmetic: float32 throughout, phase accumulated in float32.

    A double-precision model drifts from the module by a few 1e-4 over a second,
    which is above 16-bit quantisation and would hide a real defect behind
    "close enough". Every step is rounded to float32 exactly as the module does.
    """
    def f32(value: float) -> float:
        return struct.unpack("<f", struct.pack("<f", value))[0]

    speed, amount, level = (float(state["params"][k]) for k in ("speed", "amount", "level"))
    inc = f32(speed / 48000.0)
    phase = 0.0
    for _ in range(n):
        sine = f32(math.sin(6.283185307179586 * phase))
        lfo = f32(0.5 + f32(0.5 * sine))
        amp = f32(level * f32(f32(1.0 - amount) + f32(amount * lfo)))
        yield f32(input_left * amp), f32(input_right * amp)
        phase = f32(phase + inc)
        if phase >= 1.0:
            phase = f32(phase - 1.0)


def main(argv: list[str]) -> int:
    windows = Path(argv[1]) if len(argv) == 2 else None
    if len(argv) > 2:
        print("usage: check_tremolo_render.py [WINDOWS_WAV]", file=sys.stderr)
        return 2

    state = project_state(DAW / "tremolo.rpp")
    failures = []
    # Offline reopen: the host hands the plugin its saved chunk again, and the
    # plugin must report the same module and the same values.
    reopened_file = DAW / "tremolo-chunk-reopened.txt"
    if reopened_file.exists():
        reopened = chunk_document(base64.b64decode(reopened_file.read_text().strip()))
        if reopened != state:
            failures.append("reopened state differs from the saved state")
        reopened_params = host_params(DAW / "tremolo-params-reopened.txt")
        if reopened_params != host_params(DAW / "tremolo-params.txt"):
            failures.append("host parameter values changed across save and reopen")
    else:
        failures.append("no reopened project state: reopen was not exercised")
    if state.get("module") != MODULE:
        failures.append(f"project state names {state.get('module')!r}, not the installed module")
    for key, value in EXPECTED.items():
        if abs(float(state.get("params", {}).get(key, -1e9)) - value) > 1e-6:
            failures.append(f"project state {key} = {state.get('params', {}).get(key)!r}, expected {value}")

    params = host_params(DAW / "tremolo-params.txt")
    host_values = {}
    for slot, key in enumerate(("speed", "amount", "level")):
        low, high = RANGES[key]
        wanted = (EXPECTED[key] - low) / (high - low)
        got = params.get(GENERIC_BASE + slot)
        host_values[key] = got
        if got is None or abs(got - wanted) > 1e-6:
            failures.append(f"host slot {GENERIC_BASE + slot} ({key}) = {got!r}, expected {wanted:.6f}")

    audio = samples(DAW / "tremolo-render.wav")
    left, right = audio[0::2], audio[1::2]
    # The input WAV is 16-bit, so read the real values instead of assuming them.
    source = samples(DAW / "input.wav")
    input_left, input_right = source[0::2][0], source[1::2][0]
    error = 0.0
    for index, (want_l, want_r) in enumerate(expected_output(len(left), state, input_left, input_right)):
        error = max(error, abs(left[index] - want_l), abs(right[index] - want_r))
    tolerance = 2 ** -14
    if error > tolerance:
        failures.append(f"tremolo render deviates from the analytic modulation by {error:.3e}")

    report = {
        "module": state.get("module"),
        "state_params": state.get("params"),
        "host_generic_slots": host_values,
        "frames": len(left),
        "maximum_error": error,
        "first_frame": {"left": left[0], "right": right[0]},
        "peak": max(max(abs(x) for x in left), max(abs(x) for x in right)),
        "tolerance": tolerance,
    }
    if windows is not None:
        other = samples(windows)
        if len(other) != len(audio):
            failures.append(f"windows render has {len(other) // 2} frames, linux {len(left)}")
        difference = max((abs(a - b) for a, b in zip(audio, other)), default=1.0)
        report["windows_max_difference"] = difference
        report["windows_identical_after_quantisation"] = difference <= 2 ** -15
        if difference > 2 ** -15:
            failures.append(f"windows render differs from linux by {difference:.3e}")
    (DAW / "tremolo-report.json").write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2))
    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
