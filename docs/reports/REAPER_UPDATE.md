# Installing and updating OpenVST in REAPER

## The question

Can ReaPack provide an "update" button for OpenVST?

## What ReaPack can and cannot do

ReaPack installs packages from an XML index. The index format defines a fixed set
of package types: `script`, `effect`, `extension`, `data`, `theme`, `langpack`,
`webinterface`, `projectpl`, `tracktpl`, `midinotenames`, `autoitem` and
`keymap`. There is no VST or VST3 type, and the `file` attribute of a `source`
element is relative to the category name, so every package lands somewhere under
the REAPER resource directory. A VST3 must live in a folder REAPER scans for
VST3, which is outside that tree and user-specific, so ReaPack cannot install or
replace it.

The native `.ReaperPkg` format (REAPER 7.86+) has the same shape: it writes only
into whitelisted resource subfolders such as `Scripts`, `Effects`, `Data` and
`UserPlugins`, and its own documentation calls it an installer, not an updater.

There is a second, independent blocker. Windows keeps the loaded DLL locked for
as long as REAPER has the plug-in open, and Linux keeps the shared object mapped.
No in-host package manager can swap that file under a running REAPER. Every
approach has to apply the new binary while the host is closed, or stage it for
the next start.

Conclusion: ReaPack can distribute a ReaScript helper, but it cannot deliver the
plug-in binary, and no button inside REAPER can replace the file it is executing.

## What this repository provides

    scripts/reaper/openvst_update.ps1   installer (Windows)
    scripts/reaper/openvst-update.cmd   one-click launcher, waits for REAPER
    scripts/reaper/update.example.json  configuration template

The installer reads `%LOCALAPPDATA%\OpenVST\update.json`:

    kit       build root: build\, runtime\, fixtures\, probe
    vst3Dir   %LOCALAPPDATA%\Programs\Common\VST3 (already in vstpath64)
    store     %APPDATA%\OpenVST\modules
    packages  module ZIPs to install through the catalogue probe

It performs, in this order:

1. Refuse to run while REAPER is open unless `-WaitForReaper <seconds>` is
   given; the wait applies the update after the host exits.
2. Install each module package into the content-addressed store by running
   `openvst_catalogue_probe --package`, and require its `OK id=... digest=...`
   marker. Modules are installed first because they do not need the new
   container.
3. Compare each built bundle with the installed one. If the manifests are
   identical, skip it. Otherwise copy it to a staging directory, verify every
   file by SHA-256, move the installed bundle to a timestamped backup, move the
   staged bundle into place and verify it again. A failure restores the bundles
   this run already replaced.
4. Write `%LOCALAPPDATA%\OpenVST\last-update.json` with the previous and new
   hashes, the package digests and the backup path, then relaunch REAPER if the
   installer closed it.

Exit codes: `0` success, `1` verification or install failure, `20` REAPER is
running and waiting was not allowed, `21` the wait timed out. Nothing is written
before the REAPER check and before every source file is verified.

## Verified behaviour (Windows desktop, 2026-11-20)

- Dry run with the current build: FX reported as already current, the missing
  Instrument target skipped, no write.
- Real swap: the previous FX bundle `8A55B473...14C7EE` was replaced by
  `0932DBA3...5D9C0C`, the backup at
  `%LOCALAPPDATA%\OpenVST\backups\20261120-140737\OpenVST FX.vst3` still hashed
  `8A55B473...14C7EE` after the swap, and the receipt recorded both.
- Idempotent run: both the FX bundle and the module were reported unchanged.
- Guard while the user's REAPER was open: exit `20`, target hash unchanged.
- Guard with a three second wait against a process that did not exit: exit `21`,
  target hash unchanged.
- After install, `reaper-vstplugins64.ini` contains a full `OpenVST_FX.vst3=`
  entry (identity, not just a bare ID), so the host discovered the new bundle.
- `openvst_editor_snapshot --module org.openvst.creature-voice` renders the
  editor with `Creature Voice` selected, the status line
  `Ready - org.openvst.creature-voice` and the six controls.

Not tested: the `.cmd` launcher inside an interactive console (its PowerShell
body is the tested part), macOS, a Linux installer, and any public release.

## Limits and the next honest step

- The updater installs what is in the build root. Producing a public download
  would require two product changes first: a relocatable Cmajor runtime lookup
  (the build currently embeds the absolute runtime and fixtures paths) and a
  redistribution decision for the Cmajor runtime, which
  `research-dependencies.json` still records as undecided.
- A ReaScript action could call this installer detached and then let the user
  quit REAPER; the update would apply during the shutdown. That is a wrapper
  around the tested script, not a way to avoid the restart.
