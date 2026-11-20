#!/bin/sh
# Exercise the packaged module in REAPER with the editor closed. Reuse the
# installed-module host probes but keep all files in a separate test directory.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
REF="$ROOT/artifacts/daw"
test -s "$REF/fx-chunk.txt" && test -s "$REF/input.wav" || {
  printf 'Run sh scripts/daw_smoke.sh first.\n' >&2
  exit 1
}
mkdir -p "$ROOT/artifacts/creature"
ART=$(mktemp -d "$ROOT/artifacts/creature/reaper.XXXXXX")
printf 'Writing REAPER evidence to %s\n' "$ART"
REAPER="$ROOT/research/reaper/reaper_linux_x86_64/REAPER/reaper"
RUNTIME="$ROOT/research/toolchain/1.0.3209/linux/x64/libCmajPerformer.so"
MODULE=org.openvst.creature-voice
mkdir -p "$ART/home/.vst3" "$ART/profile"
FX="$ROOT/build/plugin/OpenVSTFX_artefacts/Release/VST3/OpenVST FX.vst3"
ln -s "$FX" "$ART/home/.vst3/OpenVST FX.vst3"
# The explicit path plus the standard per-user directory matches the proven
# baseline discovery setup, without copying a cached REAPER profile.
printf '[reaper]\nloadlastproj=0\nrenderclosewhendone=0\nvstpath64=%s\n' "$(dirname "$FX")" > "$ART/profile/reaper.ini"
export HOME="$ART/home" XDG_CONFIG_HOME="$ART/home/.config" OPENVST_TEST_ROOT="$ART"

PACKAGE="$ART/creature-voice.openvst.zip"
python3 tools/make_creature_package.py "$PACKAGE" > "$ART/package.json"
build/plugin/openvst_catalogue_probe --store "$XDG_CONFIG_HOME/OpenVST/modules" \
  --runtime "$RUNTIME" --fixtures "$ROOT/artifacts/plugin/fixtures" --id "$MODULE" \
  --package "$PACKAGE" --state-out "$ART/state.json"
python3 tests/daw/tremolo_chunk.py "$ART/state.json" "$REF/fx-chunk.txt" "$ART/tremolo-chunk.b64"

# Optional listening input; the caller provides a 48 kHz stereo 16-bit WAV
# of exactly one second. The default input drives each channel distinctly.
if [ "$#" -gt 0 ]; then
  cp -- "$1" "$ART/input.wav"
else
  cp "$REF/input.wav" "$ART/input.wav"
fi
run_reaper() {
  timeout 60s xvfb-run -a "$REAPER" -newinst -nosplash -ignoreerrors \
    -cfgfile "$ART/profile/reaper.ini" "$@" >> "$ART/reaper.log" 2>&1 || true
  if [ -s "$ART/error.txt" ]; then
    printf 'REAPER probe failed; see %s/error.txt\n' "$ART" >&2
    return 1
  fi
}
run_reaper -new "$ROOT/tests/daw/create_tremolo_project.lua"
test -s "$ART/tremolo-created.txt"
run_reaper -renderproject "$ART/tremolo.rpp"
test -s "$ART/tremolo-render.wav"
run_reaper -new "$ROOT/tests/daw/reopen_tremolo.lua"
test -s "$ART/tremolo-reopened.txt"
python3 - "$ART" <<'PY'
import base64
import json
import math
import sys
import wave
from pathlib import Path

root = Path(sys.argv[1])
expected = json.loads((root / 'state.json').read_text())
chunk = base64.b64decode((root / 'tremolo-chunk-reopened.txt').read_text())
actual, _ = json.JSONDecoder().raw_decode(chunk[8:].decode('utf-8'))
for key in ('module', 'params'):
    assert actual[key] == expected[key], (actual[key], expected[key])


def samples(name):
    with wave.open(str(root / name)) as wav:
        width = wav.getsampwidth()
        assert (wav.getnchannels(), wav.getframerate(), wav.getnframes()) == (2, 48000, 48000)
        assert width in (2, 3), width
        raw = wav.readframes(wav.getnframes())
        scale = 1 << (8 * width - 1)
        return [int.from_bytes(raw[i:i + width], 'little', signed=True) / scale
                for i in range(0, len(raw), width)]


dry, wet = samples('input.wav'), samples('tremolo-render.wav')
peak = max(map(abs, wet))
rms_delta = math.sqrt(sum((a - b) ** 2 for a, b in zip(dry, wet)) / len(wet))
assert 0.001 < peak < 1.0 and rms_delta > 0.001, (peak, rms_delta)
print(f'REAPER: 48000 stereo frames, peak={peak:.6f}, dry/wet RMS delta={rms_delta:.6f}; exact state recalled')
print(f'Artifacts: {root}')
PY
