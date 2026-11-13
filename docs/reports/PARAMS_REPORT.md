# Per-module parameters: what changed and what was measured

Commit series on `main`. Files: `src/plugin/ModuleRegistry.h`, `TrustedDSP.h`,
`Processor.cpp/.h`, `NativeEditor.h`, `tests/plugin/ProcessorTests.cpp`,
`scripts/build_plugin.sh`.

## The model

- `ParamInfo` declares, per parameter: permanent automation ID, ASCII display
  name, Cmajor endpoint, min, max, default, unit. `ModuleInfo` carries a table of
  them, so the container has no `gain` special case anywhere.
- The FX variant exposes the **union** of every FX module's parameters, in a
  frozen order. Position 0 stays `openvst.fx.gain.v1`, so existing projects keep
  their automation lane. Four parameters are exposed today: `openvst.fx.gain.v1`,
  `openvst.ts.attack.v1`, `openvst.ts.sustain.v1`, `openvst.ts.gain.v1`. The
  instrument variant exposes none.
- `processBlock` sends only the active module's parameters, resolved once at
  prepare time into endpoint handles and parameter pointers. No string comparison,
  no allocation, no lock in the callback.
- State schema 2 stores the active module's parameter values. Schema 1 documents
  are still read: absent parameters take their declared defaults, the gain value
  is applied as before. An unknown ID, an unknown module or an out-of-range value
  is refused, and the refusal is sticky.

## Registered module

`org.openvst.spike.transient-shaper` 0.0.1, processor `TransientShaper`,
source `27ee6305dba4fc46cf011298debe7889ae93d707221d4f6fc626006d3c6d8c32`,
manifest `d87cd4f2f719d2109adb53c3b398757f39d1a17acd258172c74201ac2696b428`,
recomputed from disk and compared with `artifacts/runtime/TRANSIENT_REPORT.md`.

## Measured

| Check | Result |
|---|---|
| Native suite, ctest `processor_native` | 100 % tests passed, 1 test, exit 0 |
| Named blocks | 12 PASS, ending `ALL PROCESSOR TESTS PASSED` |
| Package validator suite | 34 tests OK |
| DAW render, REAPER 7.85 Linux | FX max error 6.1035e-06 over 48000 frames |
| Instrument render | peak 0.18858 at frame 26, silent tail |
| State recall in REAPER | gain recalled at 0.250000000 |
| Automation ramp | 0.00426 to 0.40002, monotonic over ten deciles |
| AddressSanitizer run of the same suite | exit 0, no report, `ALL PROCESSOR TESTS PASSED` |

The DAW numbers are identical to the run made before parameters existed, which is
the point: adding per-module parameters did not move the stereo-gain render.

Commands, in order:

    sh scripts/build_plugin.sh
    ctest --test-dir build/plugin --output-on-failure
    python3 -m unittest discover -s tests/packages -v
    sh scripts/daw_smoke.sh
    LD_PRELOAD=$(gcc -print-file-name=libasan.so) ./build/asan/openvst_processor_tests

The sanitizer build is a separate tree, `build/asan`, configured with
`-fsanitize=address`. The runtime must be preloaded because the JUCE core library
loads before it; without `LD_PRELOAD` the run aborts before main.

## Behaviour chosen on purpose

- **A parameter of an inactive module stays exposed and has no effect.** Removing
  it from the host list would renumber automation lanes and break projects.
- **A rejected state mutes the plugin instead of passing audio through.** Before
  this change an FX with a refused restore kept passing its input, so a broken
  project sounded like a working plugin at default settings. The test that
  asserted the old passthrough was itself relying on uninitialised memory.
- **A valid restore after a refusal does not resurrect the engine by itself.**
  The refusal released it; the host prepares again, as it does at load.

## Defects found in the sub-agent work

- The state tests built a `DynamicObject`, replaced its `params` child, then wrote
  through the freed child pointer. That is a use-after-free in the test, and it
  crashed the suite as soon as the earlier stale assertion stopped masking it.
  Rewritten with one fresh document per case.
- An assertion expected attack to survive a roundtrip at 6 dB after the same test
  had reset it to 0 dB. Replaced by three distinct non-default values, so a
  roundtrip that silently keeps defaults cannot pass.
- A debug `std::cerr` dump was left inside the render helper, printing on every
  sample mismatch. Removed.

## Not proven

- No macOS build or run.
- Automation of the transient shaper's three parameters has not been rendered in
  a DAW; only the native calls and the Linux stereo-gain automation were measured.
- The native editor was not inspected visually: it now creates one slider and one
  label per active parameter, but no screenshot was taken of the transient shaper
  selected.
- Real-time safety is unchanged in kind: prepared handles, no allocation in the
  callback, still no measured worst-case execution time.
- Inactive-module values are not stored in the state document. Switching module
  and switching back therefore returns that module's parameters to their
  defaults; a project that relies on the previous values of another module would
  need schema 3.
