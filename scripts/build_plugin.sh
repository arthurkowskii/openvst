#!/bin/sh
# Internal AGPLv3-route prototype. No install target, no copying to user VST dirs.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
mkdir -p artifacts/plugin/fixtures build/plugin
# Originals are read-only inputs; exact SHA256 allowlist is enforced at prepare.
cp spikes/runtime/modules/stereo_gain.cmajor spikes/runtime/modules/stereo_gain.cmajorpatch spikes/runtime/modules/note_grid.cmajor spikes/runtime/modules/note_grid.cmajorpatch artifacts/plugin/fixtures/
RUNTIME=${OPENVST_RUNTIME:-"$ROOT/research/toolchain/1.0.3209/linux/x64/libCmajPerformer.so"}
FIXTURES=${OPENVST_FIXTURES:-"$ROOT/artifacts/plugin/fixtures"}
cmake -S . -B build/plugin -G 'Unix Makefiles' -DCMAKE_BUILD_TYPE=Release -DOPENVST_RUNTIME="$RUNTIME" -DOPENVST_FIXTURES="$FIXTURES" > artifacts/plugin/configure.log 2>&1
cmake --build build/plugin --parallel 1 > artifacts/plugin/build.log 2>&1
ctest --test-dir build/plugin --output-on-failure -V > artifacts/plugin/ctest.log 2>&1
printf 'Build and CTest passed. Logs: %s/artifacts/plugin/\n' "$ROOT"
