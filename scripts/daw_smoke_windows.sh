#!/bin/sh
# Render the verified projects on Arthur's Windows desktop and check the audio.
#
# Requires: the Linux smoke run has produced artifacts/daw/*.rpp, the VST3
# bundles exist under build/plugin, and the Cmajor runtime plus the VST3 copies
# are already staged in %LOCALAPPDATA%\OpenVST-test (see artifacts/windows/REPORT.md).
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
STAGE_WIN='C:\Users\Arthur\AppData\Local\OpenVST-test'
KEY="$HOME/.ssh/id_ed25519_windows"
SSH="ssh -i $KEY -o HostKeyAlias=windows -o BatchMode=yes"
OUT=artifacts/windows/renders

test -s artifacts/daw/fx.rpp || { echo "run scripts/daw_smoke.sh first" >&2; exit 1; }
test -s artifacts/daw/tremolo.rpp || { echo "run scripts/daw_smoke_tremolo.sh first" >&2; exit 1; }
python3 tests/windows/prepare_projects.py artifacts/daw "$STAGE_WIN\\daw" artifacts/windows/dawkit

scp -i "$KEY" -o HostKeyAlias=windows -o BatchMode=yes \
    artifacts/windows/dawkit/*.rpp artifacts/windows/dawkit/input.wav \
    "Arthur@192.168.1.31:C:/Users/Arthur/AppData/Local/OpenVST-test/daw/" 2>&1 | grep -v -e post-quantum -e 'store now' -e 'may need to be upgraded' || true
scp -i "$KEY" -o HostKeyAlias=windows -o BatchMode=yes \
    tests/windows/render_task.ps1 tests/windows/render_worker.ps1 "Arthur@192.168.1.31:C:/Users/Arthur/AppData/Local/OpenVST-test/" 2>&1 | grep -v -e post-quantum -e 'store now' -e 'may need to be upgraded' || true

$SSH Arthur@192.168.1.31 'powershell -NoProfile -ExecutionPolicy Bypass -File "C:\Users\Arthur\AppData\Local\OpenVST-test\render_task.ps1" -Stage "C:\Users\Arthur\AppData\Local\OpenVST-test" -Projects "C:\Users\Arthur\AppData\Local\OpenVST-test\daw"'

mkdir -p "$OUT"
scp -i "$KEY" -o HostKeyAlias=windows -o BatchMode=yes \
    "Arthur@192.168.1.31:C:/Users/Arthur/AppData/Local/OpenVST-test/daw/*-render.wav" "$OUT/" 2>&1 | grep -v -e post-quantum -e 'store now' -e 'may need to be upgraded' || true
python3 tests/windows/check_windows_render.py "$OUT"
python3 tests/daw/check_tremolo_render.py "$OUT/tremolo-render.wav"
