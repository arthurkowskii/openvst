"""Check the REAPER render output against the analytic expectations.

FX: trusted StereoGain at 0.5 linear, so L = 0.1 and R = -0.15.
Instrument: three sine notes starting at playhead 0 (REAPER quantises the MIDI
item, so the true onset is measured from the rendered audio, not assumed).
"""
import json
import sys
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DAW = ROOT / "artifacts/daw"


def samples(path: Path) -> list[float]:
    with wave.open(str(path)) as stream:
        assert stream.getnchannels() == 2, path
        assert stream.getframerate() == 48000, path
        width = stream.getsampwidth()
        raw = stream.readframes(stream.getnframes())
    scale = float(1 << (8 * width - 1))
    return [int.from_bytes(raw[i:i + width], "little", signed=True) / scale
            for i in range(0, len(raw), width)]


def main() -> int:
    report = {}
    fx = samples(DAW / "fx-render.wav")
    left = fx[0::2]
    right = fx[1::2]
    tolerance = 2**-14  # 16-bit quantisation
    fx_error = max(max(abs(x - 0.1) for x in left), max(abs(x + 0.15) for x in right))
    report["fx"] = {"frames": len(left), "maximum_error": fx_error,
                    "left_first": left[:3], "right_first": right[:3],
                    "bypass_or_fade": fx_error > tolerance}
    synth = samples(DAW / "synth-render.wav")
    mono = synth[0::2]
    onset = next((i for i, x in enumerate(mono) if abs(x) > 1e-4), None)
    peak = max((abs(x) for x in mono), default=0.0)
    silent_tail = max((abs(x) for x in mono[-4800:]), default=0.0)
    report["instrument"] = {"frames": len(mono), "onset_frame": onset,
                            "peak": peak, "tail_peak": silent_tail}
    recall = {}
    recall_file = DAW / "recall.txt"
    if recall_file.exists():
        for line in recall_file.read_text().splitlines():
            key, _, value = line.partition("=")
            recall[key.strip()] = value.strip()
    report["state_recall"] = recall
    automation = samples(DAW / "automation-render.wav")
    auto_left = automation[0::2]
    # Gain envelope 0 -> 2 over one second on a constant 0.2 input: the output
    # must follow 0.4 * t, so it is silent at the start and loud at the end.
    start_level = max(abs(x) for x in auto_left[:480])
    end_level = max(abs(x) for x in auto_left[47520:])
    step = len(auto_left) // 10
    ramp = [max(abs(x) for x in auto_left[i * step:(i + 1) * step]) for i in range(10)]
    monotonic = all(ramp[i] <= ramp[i + 1] + 1e-4 for i in range(9))
    report["automation"] = {"start_level": start_level, "end_level": end_level,
                            "decile_peaks": ramp, "monotonic": monotonic}
    (DAW / "render-report.json").write_text(json.dumps(report, indent=2))
    failures = []
    if report["fx"]["bypass_or_fade"]:
        failures.append("FX render does not match gain 0.5 on both channels")
    if onset is None or peak <= 0.01:
        failures.append("instrument render silent")
    if silent_tail > 1e-4:
        failures.append("instrument tail not silent after release")
    if recall.get("normalized") is None or abs(float(recall["normalized"]) - 0.25) > 1e-6:
        failures.append("gain not restored after reopening the project")
    if not monotonic or end_level < 0.35 or start_level > 0.01:
        failures.append("gain automation was not applied by the host")
    print(json.dumps(report, indent=2))
    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
