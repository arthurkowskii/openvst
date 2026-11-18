#!/bin/sh
# REAPER proof for a catalogue-installed module on the current platform.
#
# 1. install the demo package into the isolated store (no server involved);
# 2. turn the plugin's own saved state into a host chunk and load it in REAPER;
# 3. render, then reopen the project and read back what the host restored.
#
# Everything stays inside artifacts/daw/isolated-home; no user project, config
# or plugin folder is touched. The Windows side reuses the same project.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
ART="$ROOT/artifacts/daw"
PROBE="$ROOT/build/plugin/openvst_catalogue_probe"
PACKAGE="$ROOT/artifacts/catalogue/seed/tremolo-1.0.0.openvst.zip"
MODULE=org.openvst.demo.tremolo
# JUCE maps userApplicationDataDirectory to XDG_CONFIG_HOME (~/.config) on
# Linux, so the container's default store lives there, not under ~/.local/share.
STORE="$ART/isolated-home/.config/OpenVST/modules"

test -x "$PROBE" || { echo "build the plugin first: sh scripts/build_plugin.sh" >&2; exit 1; }
test -s "$ART/fx.rpp" || { echo "run scripts/daw_smoke.sh first (needs the reference chunk)" >&2; exit 1; }
test -s "$ART/fx-chunk.txt" || { echo "run scripts/daw_smoke.sh first (no host chunk reference)" >&2; exit 1; }
test -s "$PACKAGE" || python3 tools/make_demo_package.py "$PACKAGE" >/dev/null

rm -f "$ART/tremolo-render.wav" "$ART/tremolo.rpp" "$ART/error.txt" \
      "$ART/tremolo-chunk-saved.txt" "$ART/tremolo-chunk-reopened.txt" \
      "$ART/tremolo-params-reopened.txt" "$ART/tremolo-reopened.txt"

HOME="$ART/isolated-home" "$PROBE" --store "$STORE" \
  --runtime "$ROOT/research/toolchain/1.0.3209/linux/x64/libCmajPerformer.so" \
  --fixtures "$ROOT/artifacts/plugin/fixtures" --id "$MODULE" \
  --package "$PACKAGE" --state-out "$ART/tremolo-state.json"
python3 tests/daw/tremolo_chunk.py "$ART/tremolo-state.json" "$ART/fx-chunk.txt" "$ART/tremolo-chunk.b64"

HOME="$ART/isolated-home" OPENVST_TEST_ROOT="$ART" \
  timeout 60s xvfb-run -a research/reaper/reaper_linux_x86_64/REAPER/reaper \
  -newinst -nosplash -ignoreerrors -cfgfile "$ART/profile/reaper.ini" \
  -new "$ROOT/tests/daw/create_tremolo_project.lua" 2>/dev/null || true
if [ -s "$ART/error.txt" ]; then cat "$ART/error.txt" >&2; exit 1; fi
test -s "$ART/tremolo.rpp" || { echo "tremolo project not created" >&2; exit 1; }

HOME="$ART/isolated-home" timeout 90s xvfb-run -a \
  research/reaper/reaper_linux_x86_64/REAPER/reaper -newinst -nosplash -ignoreerrors \
  -cfgfile "$ART/profile/reaper.ini" -renderproject "$ART/tremolo.rpp" 2>/dev/null || true
test -s "$ART/tremolo-render.wav" || { echo "tremolo render missing" >&2; exit 1; }
rm -f "$ART/error.txt"

HOME="$ART/isolated-home" OPENVST_TEST_ROOT="$ART" \
  timeout 60s xvfb-run -a research/reaper/reaper_linux_x86_64/REAPER/reaper \
  -newinst -nosplash -ignoreerrors -cfgfile "$ART/profile/reaper.ini" \
  -new "$ROOT/tests/daw/reopen_tremolo.lua" 2>/dev/null || true
if [ -s "$ART/error.txt" ]; then cat "$ART/error.txt" >&2; exit 1; fi

python3 tests/daw/check_tremolo_render.py
