# Creature Voice: Dehumaniser 2 workflow study

Creature Voice is an independent OpenVST FX module. Its goal is a performable creature voice: keep a pitched body during soft delivery and bring in a metallic layer as the performer leans into a word. It is not a replacement for Dehumaniser 2 and does not import its projects, presets or sample libraries.

| Reference evidence | Scope in Creature Voice |
|---|---|
| Krotos shows two processing chains with one reacting at a higher amplitude.[1] | Fixed parallel routing: pitch body plus an envelope-controlled ring-modulated layer. |
| The manual documents pitch shifting, ring modulation and routing as separate blocks.[2] | Two processing branches with six exposed controls. No editable patch cables, sample triggering, convolution or vocoder. |
| Krotos demonstrates ring-mod and vocoder workflows and shows examples based on tweaked presets.[3][5] | Musical targets for original patches only. No preset coefficients or sonic parity established from the videos. |

No official licensed source repository was identified through the web and GitHub searches. One similarly named repository was not owned by the vendor and its metadata did not identify a license. No third-party binary was downloaded or inspected; the independent design was sufficient for this milestone, and the vendor EULA addresses disassembly and reverse engineering separately (section 2.1.4).[4]

## Starting points to dial in

These are starting values, not loadable factory presets. Begin with clean dialogue and adjust Threshold to match the recording level. Audition both quiet and loud lines before deciding if the threshold is effective.

| Usage | Pitch | Body | Carrier | Texture | Threshold | Mix |
|---|---:|---:|---:|---:|---:|---:|
| Buried Sentinel, a deep speaking voice | -9 st | 0.78 | 49 Hz | 0.32 | 0.13 | 0.85 |
| Fractured Intercom, a metallic alert voice | -2 st | 0.26 | 127 Hz | 0.88 | 0.08 | 0.90 |
| Masked Whisper, mostly intelligible with grit on peaks | -5 st | 0.40 | 72 Hz | 0.56 | 0.18 | 0.55 |

The threshold is linear amplitude, not dB. Plan enough silence after the input when bouncing complete voice assets: the current FX host does not declare a module-specific tail to the DAW. The UI is native, with the controls described in the existing bounded `openvst.ui/0` package data; no author script, web view or sample-bank dependency is introduced.

## Reproduce locally

- `python3 tools/make_creature_package.py`: build and validate the ZIP without rebuilding the container.
- `python3 -m unittest discover -s tests/integration -v`: seed a temporary local catalogue, download the package, prepare the engine, render and reopen the state with the server stopped.
- `sh scripts/daw_smoke.sh` then `sh scripts/daw_smoke_creature.sh`: render the installed module with the editor closed, then reopen and verify saved settings in Linux REAPER.
- For a reference A/B, use the same unprocessed vocal take and matched levels in both plugins. Start with one pitch branch and one amplitude-dependent ring branch in Krotos. Compare the threshold response, spectral peaks, intelligibility and behavior on consonants. A successful OpenVST render alone is not evidence of equivalence.

## Verification on Linux

- Built both VST3 targets. Native CTest: 2/2; package validation: 62 cases; catalogue server: 45 cases; end-to-end catalogue: 2/2, including Creature Voice across a server shutdown. Package SHA-256: `17e024581b69600d59008464a43c90c6f441f1dc5862ee6c82697a6e5758f544`.
- At 48 kHz, a 400 Hz left-only tone with body disabled and carrier at 100 Hz produced a 500 Hz component of `3.02721e-15` relative to the quiet input amplitude and `0.499988` relative to the loud input amplitude. The silent right channel stayed exactly silent for both stimuli. This verifies the reactive ring behavior, not the fidelity of the pitch process to Krotos.
- Linux REAPER rendered a synthetic voice with the editor closed: 48,000 stereo frames, peak `0.912183`, dry/wet RMS difference `0.062118`. On reopen the saved module identity and parameter values matched, and REAPER reported `0.375` for all six generic slots. Separate regression renders kept the gain, synth and automation tests green.
- The dedicated Creature Voice smoke run on a tone failed before project creation with `OpenVST FX not discovered`. The successful voice render is evidence for the module, not for reliable startup of a fresh smoke profile.
- The native editor was rendered and inspected at 720x560 and 640x420. This is visual evidence, not an interactive accessibility audit. No macOS test, real-time CPU percentile, low-buffer xrun, acoustic voice listening test, or Krotos A/B is claimed.

## Windows laptop verification

- Built the FX VST3 and catalogue probe on the Windows laptop with JUCE 9.0.5 and Cmajor 1.0.3209. The probe installed the exact ZIP, rendered the module, and restored the saved settings in a separate offline invocation. Both runs reported quiet/loud sideband ratios of `3.04166e-15` and `0.499988` and passed the right-channel silence check. FX VST3 SHA-256: `e0991631dfbf53c06aac45f58832365983ae7e536ff2ba3ba8b0959d4feac4fd`.
- Two REAPER 7.84 runs with fresh configurations stopped at the VST scan without exporting a WAV. The second showed the `VST Plug-in Scan` dialog and other plug-in windows. Neither run identifies the blocking plug-in. The native verification does not establish that REAPER completed its scan or rendered the module.
- With a copy of the laptop's existing scan cache in a fresh profile, REAPER 7.84 discovered the staged FX and rendered the previously saved project at 48 kHz: 48,000 stereo frames, peak `0.912182689`, RMS delta from dry `0.0621180784`, RMS delta from Linux `1.33280037e-09`. This verifies playback of the saved settings; individual settings were read back by the native probe, not by Windows ReaScript.

## Sources

[1] https://krotos.studio/dehumaniser-2
[2] https://s3-us-west-2.amazonaws.com/dehumaniser/Manuals/Dehumaniser+2+Manual.pdf
[3] https://www.youtube.com/watch?v=aJUxZ3QU9WY
[4] https://krotos.studio/terms
[5] https://www.youtube.com/watch?v=NzgGzVBKLm8
