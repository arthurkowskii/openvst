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
- A catalogue-installed module path: a package is downloaded from the v0
  catalogue server, SHA-256 verified, extracted into an immutable
  content-addressed store, loaded from the store, saved with its exact package
  digest, and reopened offline after the server is stopped.
- Eight permanent generic host parameter slots per variant for catalogue
  modules. Built-in parameter positions never move; a catalogue module maps its
  own parameters onto the slots and keeps its real names and ranges in the
  editor and in the saved state.
- A declarative module UI (`openvst.ui/0`): JSON layout data with knobs,
  sliders, toggles, values, labels and groups, bound to module parameters and
  rendered natively. No author JavaScript, HTML or remote resource is executed.
  The `Tremolo` demo package exercises it.
- A small catalogue server in Python's standard library: SQLite WAL,
  content-addressed packages, Range downloads, package validation, approval and
  audit. A single admin token, no accounts yet.
- Host parameters, project state restore, and DAW automation, verified in REAPER
  on Linux and Windows with identical renders.
- A REAPER proof for a catalogue-installed module: the demo `Tremolo` is
  installed from the store, selected through the plugin's own saved state,
  rendered, saved, reopened offline and compared sample by sample with its
  analytic sine modulation. The Windows and Linux renders are bit-identical
  after 16-bit quantisation.
- An offscreen editor snapshot tool (`openvst_editor_snapshot`) that renders the
  real editor to PNG, so layout, contrast and clipping are measured without a
  DAW window.
- A package validator that inspects a module archive without extracting or
  executing it.

## What does not exist yet

- Accounts, 2FA, quotas beyond a simple token, public deployment and TLS
  termination. The server runs behind a reverse proxy; there is no hosted
  instance and no Publish screen in the plugin.
- A catalogue UI beyond the minimal Discover/Install panel: search, filters,
  demos, submissions, moderation and Unapproved are server/API or design only.
- Custom HTML/CSS/JS module interfaces. Only the declarative UI v0 is
  implemented; author JavaScript remains unimplemented and is not claimed safe.
- Any sandbox, signature or containment of third-party module code. Cmajor
  compiles the package source in-process. A signature would prove provenance,
  not safety.
- Any claim of real-time performance. No audio callback has been measured.
- A macOS build or render. Linux and Windows only; a Linux render does not
  prove macOS.

## Build

Requirements: CMake 3.22 or newer, a C++17 compiler, JUCE 9.0.5 sources, a
Cmajor runtime of version 1.0.3209, and the Cmajor headers for that same tag.

    sh scripts/build_plugin.sh

The script writes logs and test output under `artifacts/plugin/`. Paths to the
Cmajor runtime and to the trusted fixtures are explicit developer inputs, never
hardcoded into the product.

## Tests

    ctest --test-dir build/plugin --output-on-failure     # native processor + store tests
    python3 -m unittest discover -s tests/packages -v     # package and UI validator
    python3 -m unittest discover -s tests/server -v       # catalogue server
    python3 -m unittest discover -s tests/integration -v  # server -> install -> DSP -> offline
    sh scripts/daw_smoke.sh                               # REAPER render, Linux
    sh scripts/daw_smoke_tremolo.sh                       # catalogue module in REAPER, Linux

The DAW test renders projects and checks samples against analytic values, so a
silent or bypassed plugin fails instead of passing. The integration test starts
a real catalogue server, downloads and installs the demo package, renders audio,
stops the server, then restores the module and its state from the store alone.

## Layout

    src/plugin/      container: processor, store, catalogue client, module UI
    server/          stdlib-only catalogue server and SQLite schema
    spikes/runtime/  engine harness and the Cmajor fixtures
    openvst/         package validation, no execution of package content
    tools/           demo package builder
    tests/           native, store, package, server, integration and DAW tests
    docs/            architecture, module format, UI schema, agent instructions

## Licence

AGPL-3.0-only for this repository. Cmajor is GPLv3-or-later or commercial, JUCE
is AGPLv3 or commercial. No final distribution licence has been chosen, and no
licence audit of every transitive dependency is complete.

## Contributing

Read `AGENTS.md` first: it lists the invariants that keep existing sessions and
automation working, and the verification each change must pass before it is
presented as done.
