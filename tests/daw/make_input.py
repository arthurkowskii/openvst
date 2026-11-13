"""Deterministic analytic input: L = 0.2, R = -0.3, one second at 48 kHz."""
import struct
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEST = ROOT / "artifacts/daw/input.wav"


def main() -> None:
    DEST.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(DEST), "wb") as stream:
        stream.setparams((2, 2, 48000, 48000, "NONE", "not compressed"))
        stream.writeframes(struct.pack("<hh", 6554, -9830) * 48000)


if __name__ == "__main__":
    main()
