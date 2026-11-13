#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$ROOT"
mkdir -p artifacts/runtime/bin
# One compiler process; no LLVM/Boost build or network. Memory ceiling 2 GiB.
ulimit -v 2097152
export TMPDIR="$ROOT/artifacts/runtime/build-tmp"
mkdir -p "$TMPDIR"
exec timeout 180 g++ -std=c++17 -O1 -g0 -Wall -Wextra -Wpedantic -I research/cmajor-release/include spikes/runtime/harness.cpp -o artifacts/runtime/bin/runtime_harness -ldl -pthread
