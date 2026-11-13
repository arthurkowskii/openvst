"""Check the WAV files rendered by REAPER on Windows against analytic values.

Same expectations as the Linux checker, minus state recall, which is only
proven on Linux for now.

Usage: check_windows_render.py DIRECTORY
"""
import json
import sys
import wave
from pathlib import Path


def samples(path: Path) -> list[float]:
    with wave.open(str(path)) as stream:
        if stream.getnchannels() != 2 or stream.getframerate() != 48000:
            raise SystemExit(f"{path.name}: unexpected {stream.getparams()}")
        width = stream.getsampwidth()
        raw = stream.readframes(stream.getnframes())
    scale = float(1 << (8 * width - 1))
    return [int.from_bytes(raw[i:i + width], "little", signed=True) / scale
            for i in range(0, len(raw), width)]


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print("usage: check_windows_render.py DIRECTORY", file=sys.stderr)
        return 2
    root = Path(argv[1])
    fx = samples(root / "fx-render.wav")
    left, right = fx[0::2], fx[1::2]
    fx_error = max(max(abs(x - 0.1) for x in left),
                   max(abs(x + 0.15) for x in right))
    synth = samples(root / "synth-render.wav")[0::2]
    onset = next((i for i, x in enumerate(synth) if abs(x) > 1e-4), None)
    peak = max((abs(x) for x in synth), default=0.0)
    auto = samples(root / "automation-render.wav")[0::2]
    step = len(auto) // 10
    ramp = [max(abs(x) for x in auto[i * step:(i + 1) * step]) for i in range(10)]
    report = {
        "fx": {"frames": len(left), "maximum_error": fx_error,
               "left_first": left[0], "right_first": right[0]},
        "instrument": {"frames": len(synth), "onset_frame": onset, "peak": peak},
        "automation": {"decile_peaks": ramp, "start": ramp[0], "end": ramp[-1],
                       "monotonic": all(ramp[i] <= ramp[i + 1] + 1e-4
                                        for i in range(9))},
    }
    (root / "windows-render-report.json").write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2))
    failures = []
    if fx_error > 2 ** -14:
        failures.append("FX render does not match gain 0.5 on both channels")
    if onset is None or peak <= 0.01:
        failures.append("instrument render silent")
    if not report["automation"]["monotonic"] or report["automation"]["end"] < 0.35:
        failures.append("gain automation not applied by the host")
    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
