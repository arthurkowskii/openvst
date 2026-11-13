#!/bin/sh
# Two isolated phases in a fresh REAPER resource folder: create the projects
# through ReaScript, then render them with the documented -renderproject switch.
# No user project, configuration or installer is touched.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
ART="$ROOT/artifacts/daw"
python3 tests/daw/make_input.py
python3 - <<'EOF'
from pathlib import Path
root = Path('artifacts/daw').resolve()
profile = root / 'profile'
profile.mkdir(parents=True, exist_ok=True)
vst = [Path('build/plugin/OpenVSTFX_artefacts/Release/VST3').resolve(),
       Path('build/plugin/OpenVSTInstrument_artefacts/Release/VST3').resolve()]
(profile / 'reaper.ini').write_text(
    '[reaper]\nloadlastproj=0\nvstpath64=' + ';'.join(map(str, vst)) + '\n')
link = root / 'isolated-home' / '.vst3'
link.mkdir(parents=True, exist_ok=True)
for name, sub in (('OpenVST FX', 'OpenVSTFX'), ('OpenVST Instrument', 'OpenVSTInstrument')):
    target = (Path('build/plugin') / (sub + '_artefacts') / 'Release/VST3' / (name + '.vst3')).resolve()
    if not (link / (name + '.vst3')).exists():
        (link / (name + '.vst3')).symlink_to(target, target_is_directory=True)
EOF
rm -f "$ART/fx-render.wav" "$ART/synth-render.wav" "$ART/error.txt"
HOME="$ART/isolated-home" OPENVST_TEST_ROOT="$ART" \
  timeout 60s xvfb-run -a research/reaper/reaper_linux_x86_64/REAPER/reaper \
  -newinst -nosplash -ignoreerrors -cfgfile "$ART/profile/reaper.ini" \
  -new "$ROOT/tests/daw/create_projects.lua" 2>/dev/null || true
if [ -s "$ART/error.txt" ]; then cat "$ART/error.txt" >&2; exit 1; fi
test -s "$ART/fx.rpp" && test -s "$ART/synth.rpp" || { echo "projects not created" >&2; exit 1; }
rm -f "$ART/error.txt"
HOME="$ART/isolated-home" OPENVST_TEST_ROOT="$ART" \
  timeout 60s xvfb-run -a research/reaper/reaper_linux_x86_64/REAPER/reaper \
  -newinst -nosplash -ignoreerrors -cfgfile "$ART/profile/reaper.ini" \
  -new "$ROOT/tests/daw/automation_projects.lua" 2>/dev/null || true
if [ -s "$ART/error.txt" ]; then cat "$ART/error.txt" >&2; exit 1; fi
test -s "$ART/automation.rpp" || { echo "automation project not created" >&2; exit 1; }
for name in fx synth automation; do
  HOME="$ART/isolated-home" timeout 90s xvfb-run -a \
    research/reaper/reaper_linux_x86_64/REAPER/reaper -newinst -nosplash -ignoreerrors \
    -cfgfile "$ART/profile/reaper.ini" -renderproject "$ART/$name.rpp" 2>/dev/null || true
done
python3 tests/daw/check_render.py
