# Starting prompt: OpenVST catalogue milestone handoff

You are taking over OpenVST, a C++17 / JUCE 9.0.5 VST3 container for Cmajor audio
modules. The repository is `/home/randomaccessmemories/Documents/GIT/OpenVST`.
The public remote is `https://github.com/arthurkowskii/openvst`, branch `main`.
Arthur is the product owner. He is a technical sound designer and composer, not
a passive client: challenge assumptions, report uncertainty, and prove claims
with a measurement or a render. Answer him in French when he writes in French.

## Read first, in this order

1. `AGENTS.md`: product invariants and platform traps. It is a protected file.
   Do not edit it without Arthur's explicit approval; put new evidence in
   `docs/reports/` and propose the diff.
2. `README.md`: honest inventory of what exists and what does not.
3. `docs/PROJECT_BRIEF.md`, `docs/design/ARCHITECTURE.md`,
   `docs/design/CATALOGUE_DESIGN.md`.
4. `docs/MODULE_FORMAT.md`, `docs/design/MODULE_UI.md`,
   `docs/schema/module-ui.schema.json`.
5. `~/.hermes/projects/openvst/status.md`: dated narrative, useful as a lead but
   not authoritative. Trust `git log` plus fresh executions.
6. Code: `src/plugin/Processor.{h,cpp}`, `ModuleRegistry.h`, `ModuleSpec.h`,
   `ModuleStore.{h,cpp}`, `CatalogueClient.{h,cpp}`, `ModuleUI.{h,cpp}`,
   `NativeEditor.h`, `server/openvst_catalogue.py`.

## State at handoff, 2026-11-18

The catalogue milestone v0 is implemented and verified on Linux and Windows.
Commit `325faf7` on `main` is the state: the Windows build was installed and
rendered, and the report `docs/reports/2026-11-18-windows-catalogue.md` records
the commands, hashes and remaining gaps. Read it before re-running anything.

What works and is verified by the parent, not only by workers:

- `sh scripts/build_plugin.sh` exits 0. CTest: 2/2 passed.
  - `processor_native`: installed-module path, permanent generic slots, state
    schema 3, refusal of absent or tampered modules, plus all earlier tests.
  - `module_store`: 14 named PASS blocks including hostile archives, concurrent
    installs, stale staging, cancel, immutable versions and UI path.
- `python3 -m unittest discover -s tests/packages -v`: 62 tests, OK.
- `python3 -m unittest discover -s tests/server -v`: 45 tests, OK.
- `python3 -m unittest discover -s tests/integration -v`: 1 test, OK.
  It starts a real server, downloads and installs the demo Tremolo package,
  renders audio, stops the server, then restores the module and its state from
  the store alone.
- `sh scripts/daw_smoke.sh`: exit 0. FX error 6.1e-6, instrument onset 26,
  peak 0.188582, state recall 0.25, automation monotone.

What is implemented:

- Content-addressed module store: download with Range resume, SHA-256
  verification, bounded ZIP inspection, staging, advisory lock, atomic publish,
  `install.json`, scan and exact-version removal.
- Catalogue server v0: Python standard library only, SQLite WAL,
  content-addressed packages, Range and ETag, package validation, submissions,
  approval, rejection, audit and reports. One bearer admin token, no accounts.
- In-plugin Discover/Install panel and module selector for built-ins and
  installed modules.
- Permanent generic host slots: 8 per variant, IDs
  `openvst.fx.module.1.v1`...`openvst.fx.module.8.v1` and
  `openvst.instr.module.1.v1`...`openvst.instr.module.8.v1`, normalised 0..1.
  Built-in parameter positions never move. Installed module parameters keep
  their real names, ranges and values in the editor and in state.
- State schema 3 records module id, version, source SHA-256, package SHA-256 and
  real parameter values. Schema 1 and 2 remain readable. A missing exact
  version is refused and the plugin mutes; it is never substituted.
- Declarative module UI `openvst.ui/0`: JSON data only, native JUCE widgets,
  validated by the package validator and parsed by the renderer. The demo
  `Tremolo` package exercises it.
- Demo package builder `tools/make_demo_package.py`. The generated package is
  not committed; regenerate it. The current demo digest after the Cmajor fix is
  `786d019eba8298c292a12c90745cd1c3e87a02a5f9210a4388135fcc5795d96f`, but
  any source change changes it.

What is not done, and must not be claimed:

- No macOS build, install or DAW render.
- The module UI and the Discover panel are verified in Arthur's REAPER on
  Windows as a static panel (module installed, selected, prepared). The
  Discover/Refresh/Install clicks were exercised by the panel's own code and by
  the headless probe, not filmed in his session. A DPI scale other than his is
  not photographed.
- No deployed public catalogue, no TLS termination, no accounts, no 2FA, no
  quotas beyond a token and a simple rate limit.
- No sandbox, no signature, no containment of package DSP. Cmajor compiles the
  package source in-process. A signature would prove provenance, not safety.
- No real-time performance measurement. CPU, p99 callback time, xruns and PDC
  are unmeasured.
- The catalogue panel performs network and install work synchronously on the
  message thread in v0. A slow or dead server can freeze the editor; this is a
  known limitation, not a hidden feature.
- `AGENTS.md` still says the next step is module selection. That section is
  stale. Propose an update, do not edit the file unilaterally.

## Immediate mission, in priority order

### 1. Windows build, install and DAW render of this commit

This is the main deliverable. Follow the existing Windows procedure, do not
invent a new one:

- The Windows kit is under `%LOCALAPPDATA%\OpenVST-test`. `deploy_windows_kit.sh`
  does not resynchronise `src/plugin` or `fixtures` automatically. Archive them,
  push them, extract them into the kit, and verify the remote SHA-256 against
  the local file before rebuilding. Otherwise the DLLs contain the old code.
- After a header change that alters a shared class layout, use a clean build or
  `cmake --build ... --config Release --clean-first`, then probe both variants.
- Build the two VST3s with MSVC. Install into the per-user folder
  `%LOCALAPPDATA%\Programs\Common\VST3`, which is already in Arthur's
  `vstpath64`. Close REAPER first. A loaded DLL cannot be replaced in place:
  copy to a staging name, compare SHA-256, swap, then re-read the installed
  binary and check a string that only this build contains.
- Arthur's only REAPER is `C:\Program Files\REAPER (x64)\reaper.exe`, currently
  7.85. For his visual test, launch his REAPER without `-cfgfile`. A test profile
  is not his configuration and he refuses the screenshot.
- For automated renders, REAPER does not start from an SSH session: the process
  lands in session 0. Use a scheduled task whose principal is `Interactive` in
  the logged-on user session, then delete the task. ReaScript passed on the
  command line does not run in that context. Produce the `.rpp` on Linux and
  render it on Windows; project format and plugin state are portable.
- `renderclosewhendone` must be 0 in a test profile, otherwise the render
  window stays open and the next invocation blocks forever.
- `CmajPerformer.dll` has a capital C. A case-sensitive filter makes it look
  missing after a silent install.
- A build embeds the Cmajor runtime path and the fixtures path. If either
  disappears, the plugin shows `Preparation failed`, not silence.
- Verify the installed build by its SHA-256 and by strings specific to this
  version, not by the mtime and not by a frozen UI sentence.
- After a parameter or state-schema change, rerun the DAW render. A matching
  file hash proves the content, not that the host reads it.
- Compare Linux and Windows renders on samples after 16-bit quantisation, not
  on process exit codes.

Definition of done: both VST3s installed on Windows; REAPER loads OpenVST FX and
Instrument; the installed demo Tremolo is selectable, processes audio, saves its
state, closes, and reopens offline with the same values; a Windows render is
compared with the Linux render; hashes and commands are in a report.

### 2. Visual check of the module UI and Discover panel

Open the plugin editor in Arthur's REAPER and inspect the Tremolo panel and the
Discover/Install flow. Check label readability, focus, resizing, high DPI,
keyboard access, and that no meaning depends on colour alone. Arthur is
colour-blind. Fix layout defects, then repeat the DAW render if the audio path
changed (it should not).

### 3. Hardening after the visual test

Only after the above:

- Decide whether the synchronous catalogue panel needs a worker thread. Do not
  put network or disk work on the audio thread.
- Add the missing account and moderation flows from `CATALOGUE_DESIGN.md` only
  if Arthur asks. Do not imply they exist.
- Measure real-time behaviour before any performance claim.

## Commands

Build and test:

    cd /home/randomaccessmemories/Documents/GIT/OpenVST
    sh scripts/build_plugin.sh
    ctest --test-dir build/plugin --output-on-failure
    python3 -m unittest discover -s tests/packages -v
    python3 -m unittest discover -s tests/server -v
    python3 -m unittest discover -s tests/integration -v
    sh scripts/daw_smoke.sh

Do not pipe `build_plugin.sh` into `tail` when you need its exit code; a pipeline
returns the exit code of `tail`. Redirect to a log, then read `$?`.

Demo catalogue:

    python3 tools/make_demo_package.py
    python3 server/openvst_catalogue.py \
        --data ~/.local/share/openvst-catalogue \
        --admin-token CHOOSE_A_LONG_TOKEN \
        --seed artifacts/catalogue/seed/tremolo-1.0.0.openvst.zip
    python3 server/openvst_catalogue.py \
        --data ~/.local/share/openvst-catalogue \
        --host 127.0.0.1 --port 8080 --admin-token CHOOSE_A_LONG_TOKEN

The plugin defaults to `http://127.0.0.1:8080`. Override with
`OPENVST_CATALOGUE_URL`. Installed modules live under the user application data
directory, in `OpenVST/modules`.

## Invariants that must survive

- Existing sessions keep working. Plugin identifier, parameter IDs, state
  schema and module identity are permanent.
- A version is immutable. A new version lives beside the old one. Projects
  reference an exact version and hash, never "latest".
- State is data, small, bounded and versioned. No paths, credentials or engine
  internals. A rejected restore stays rejected and mutes.
- No compilation, disk access or unbounded allocation inside the audio callback.
- No invented proof. "Works in REAPER", "real-time safe" and "sandboxed" need a
  measurement or a render.
- Unknown module, unknown archive, unknown anything: refuse. Paths are rejected,
  never repaired.

## Reporting

Write a dated report under `docs/reports/`. Include the exact commands, real
output, hashes, WAV or screenshot paths, and the gaps you did not close. Commit
and push to `main` only after the full verification chain is green. Never
substitute a plausible result for one you did not measure.
