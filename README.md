# OpenVST

A single container plugin that hosts many audio modules, so a module can be added
without rebuilding or reinstalling the plugin. Effects and instruments live in
the same product, the way a modular host exposes its modules from one window.

Status: early prototype, private quality, not ready for third-party modules.

## What exists today

- `OpenVST FX` and `OpenVST Instrument`, two VST3 builds from one codebase.
- Cmajor as the DSP engine: modules are `.cmajor` sources compiled at load time.
- Two effects and one instrument, kept as trusted fixtures: a stereo gain, a
  transient shaper with attack, sustain and output gain, and a polyphonic sine
  instrument. Their job is to prove the container, not to sound good.
- Module selection by stable identity inside the FX slot, with a per-module
  hash, a per-module parameter table, permanent automation IDs and a state
  document that reads older projects. A parameter of an inactive module stays
  exposed to the host and has no effect.
- Host parameters, project state restore, and DAW automation, verified in REAPER
  on Linux and Windows with identical renders.
- A package validator that inspects a module archive without extracting or
  executing it.

## What does not exist yet

- A catalogue, downloads, publishing and accounts. Modules are compiled at load
  time from the trusted fixture folder only.
- Any HTML/JS module interface. The editor is a minimal native window.
- Any claim of real-time performance. No audio callback has been measured.
- Signing or any containment of third-party module code. A signature would prove
  provenance, not safety.

## Build

Requirements: CMake 3.22 or newer, a C++17 compiler, JUCE 9.0.5 sources, a
Cmajor runtime of version 1.0.3209, and the Cmajor headers for that same tag.

    sh scripts/build_plugin.sh

The script writes logs and test output under `artifacts/plugin/`. Paths to the
Cmajor runtime and to the trusted fixtures are explicit developer inputs, never
hardcoded into the product.

## Tests

    ctest --test-dir build/plugin --output-on-failure     # native processor tests
    python3 -m unittest discover -s tests/packages -v     # package validator
    sh scripts/daw_smoke.sh                               # REAPER render, Linux

The DAW test renders projects and checks samples against analytic values, so a
silent or bypassed plugin fails instead of passing.

## Layout

    src/plugin/      container: processor, state, editor, trusted module loading
    spikes/runtime/  engine harness and the two Cmajor fixtures
    openvst/         package validation, no execution of package content
    tests/           native, package and DAW tests
    docs/            architecture, module format, agent instructions

## Licence

AGPL-3.0-only for this repository. Cmajor is GPLv3-or-later or commercial, JUCE
is AGPLv3 or commercial. No final distribution licence has been chosen, and no
licence audit of every transitive dependency is complete.

## Contributing

Read `AGENTS.md` first: it lists the invariants that keep existing sessions and
automation working, and the verification each change must pass before it is
presented as done.
