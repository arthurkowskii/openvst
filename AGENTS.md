# Instructions for agents working on OpenVST

This file is the contract for any agent, human or model, that touches this
repository. Read it before writing code. It records what has already failed, so
the same hours are not spent twice.

## Product intent

One container plugin, many modules. A module is added to a folder and appears in
the product without rebuilding the container. Effects and instruments are two
builds of the same codebase, so they share the library, the catalogue and the
state format.

Modules are OpenVST modules, compiled from source by the container. Hosting
third-party VST3 plugins is explicitly out of scope.

## Invariants

These are not preferences. A change that breaks one is a regression even if the
tests pass.

1. **Existing sessions keep working.** Plugin identifier, parameter IDs, state
   schema and module identity are permanent. No silent substitution of a module
   or an engine version, no renaming of a parameter, no reordering that changes
   what an automation lane addresses.
2. **A version is immutable.** A new module version lives beside the old one.
   Projects reference an exact version and hash, never "latest".
3. **State is data, not behaviour.** `getStateInformation` writes a small,
   bounded, versioned document. It must not embed absolute paths, credentials or
   engine internals. A rejected restore stays rejected; it never turns into a
   silent success.
4. **No compilation, disk access or allocation of unbounded size inside the
   audio callback.** Module compilation happens in preparation, off the audio
   thread. This is currently true and must stay true.
5. **No invented proof.** "Works in REAPER", "real-time safe" and "sandboxed" are
   claims that require a measurement or a render. Absence of a crash is not a
   measurement.
6. **Unknown module, unknown archive, unknown anything: refuse.** Packages are
   hostile until validated. Paths are rejected, never repaired.

## Verification required before claiming a change is done

Run, in this order, and paste the real outcome:

    sh scripts/build_plugin.sh
    ctest --test-dir build/plugin --output-on-failure
    python3 -m unittest discover -s tests/packages -v
    sh scripts/daw_smoke.sh         # Linux; needs the REAPER tarball in research/

For Windows, `scripts/daw_smoke_windows.sh` renders the same projects on the
target machine. A Linux render does not prove Windows and vice versa; both have
been run once, and the samples matched.

## Hard-won platform facts

- **REAPER does not start from an SSH session on Windows.** The process lands in
  session 0, never initialises its engine, and neither ReaScript nor
  `-renderproject` produces anything. Launch it through a scheduled task whose
  principal is `Interactive`, in the logged-on user session.
- **ReaScript passed on the command line does not run** in that context, even a
  script whose only job is writing a file. Produce the `.rpp` on Linux and render
  it on Windows: the project format and the plugin state blob are portable.
- **`renderclosewhendone` must be 0** in a test profile. Otherwise REAPER keeps
  its render window open and the next invocation blocks forever.
- **A loaded DLL cannot be replaced.** Installing a new build while the host runs
  silently copies the bundle inside the old one. Copy to a staging name, compare
  SHA-256, then swap, then re-read the installed binary and check a string that
  only the new build contains.
- **A test profile scans its own VST folders.** Two copies of the plugin means
  two different builds, and the one being tested may not be the one installed for
  the user. Print the path being scanned.
- **REAPER adds its standard VST3 folders to any path list.** A test profile is
  therefore not a strict sandbox.
- **Non-ASCII literals in UI strings rot across code pages.** Keep user-visible
  strings ASCII.

## Style

- C++17, no exceptions across the plugin boundary, RAII everywhere.
- Never leave an error path that reports success.
- Comments explain why, not what.
- French is fine in internal documents and commit discussions; code, identifiers,
  file names and public documentation are English.

## Where things are

    src/plugin/Processor.cpp     audio callback, split into engine-sized blocks
    src/plugin/TrustedDSP.h      module loading, hashes, endpoint contract
    src/plugin/NativeEditor.h    minimal editor, no module UI yet
    spikes/runtime/              the harness that proved the engine works
    openvst/package_validation.py  read-only archive validation

## Known next step

Module selection. Today the FX build always loads the stereo gain fixture. The
transient shaper cannot be added cleanly until the container can select a module
by stable identity, with a per-module hash, without breaking existing projects.
