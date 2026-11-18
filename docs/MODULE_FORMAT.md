# OpenVST module format (draft)

A module is a folder of source plus a manifest. The container compiles the
source itself; nothing is executed before the archive has been validated.

    my-module.zip
      openvst.json
      patch/my-module.cmajorpatch
      patch/my-module.cmajor
      assets/...            (optional, declared and hashed)

## openvst.json

    {
      "schema": "openvst.package/0",
      "module": {
        "id": "org.example.transient-shaper",
        "version": "0.1.0",
        "name": "Transient Shaper",
        "kind": "effect",
        "vendor": "Example",
        "license": "ISC"
      },
      "runtime": {
        "engine": "cmajor",
        "patch_format": 1,
        "host_api": "openvst-host/0",
        "patch": "patch/my-module.cmajorpatch"
      },
      "parameters": [
        { "id": "attack", "name": "Attack", "min": -12, "max": 12, "default": 0, "unit": "dB" }
      ],
      "resources": [],
      "ui": "ui.json"
    }

The `ui` key is optional and lives at the top level only. It points to a
declarative interface document; see "Module interface document" below.

Rules enforced by `openvst/package_validation.py`:

- `module.id` is a lowercase reverse-DNS identifier and never changes.
- `module.version` is SemVer, ASCII digits only.
- The patch file declares the same ID and version.
- Parameter IDs are unique, permanent, and bounded by `min` and `max`.
- Every declared resource exists and matches its SHA-256.
- The archive contains no workers, no author script, no external reference.
- Paths that are dangerous on Windows are rejected, not sanitised.

## Module interface document

A module may ship a declarative interface document, `openvst.ui/0`. It is
data, not code: no script, no markup, no remote resource, no styling key. The
container renders it with native widgets. The full design, the component set
and the bounds live in `docs/design/MODULE_UI.md`; the JSON Schema lives in
`docs/schema/module-ui.schema.json`; a complete example lives in
`fixtures/ui/transient-shaper.ui.json`.

    ui.json
      {
        "schema": "openvst.ui/0",
        "title": "Transient Shaper",
        "layout": {
          "type": "column",
          "gap": 8,
          "padding": 12,
          "children": [
            { "type": "knob", "param": "attack", "label": "Attack", "unit": "dB" }
          ]
        }
      }

Manifest rules:

- `ui` is optional. A package without one stays valid.
- `ui` is accepted at the top level of `openvst.json` only. Inside `module`,
  `runtime`, `parameters` or `resources` it is an unknown key, and unknown
  keys are refused.
- The value is a safe relative POSIX path ending in `.json`, 1..240
  characters: no traversal, no backslash, no drive letter, no absolute path.
  Unsafe paths are rejected, never repaired.
- The file must exist in the archive, be at most 64 KiB, and be hashed like
  any other entry.

Document rules, all enforced by the validator:

- The top level accepts exactly `schema`, `title` and `layout`. `schema` must
  be `openvst.ui/0`; `title` is optional; `layout` is required.
- Component types: `column`, `row`, `group`, `knob`, `slider`, `toggle`,
  `value`, `label`, `spacer`. An unknown type is refused.
- An unknown key in any object is refused, never ignored.
- A `column` or a `row` needs at least one child. A `group` needs a `title`.
- Bounds: depth 8, 128 nodes, 120 characters per string, 64 KiB per
  document, `gap` and `padding` 0..32, `width` and `height` `"auto"`,
  `"fill"` or 24..400, `precision` 0..6, `spacer` size 0..64.
- User-visible strings are printable ASCII.
- `param` must name a parameter declared in the manifest `parameters` array.
  A parameter may be bound at most twice.
- A `toggle` may only drive a parameter whose declared range is within 0..1.

Validation reports structured errors: `ui_missing`, `ui_too_large`,
`ui_invalid_json`, `ui_schema`, `ui_unknown_key`, `ui_unknown_type`,
`ui_bounds`, `ui_param_unknown`, `ui_param_duplicate_binding`. Each error
carries the JSON path of the offending node.

## DSP contract

An effect declares `audioIn` and `audioOut` as `float32<2>` streams. An
instrument declares `midiIn` as an event endpoint and `audioOut` as a
`float32<2>` stream. The container validates endpoints before linking, so a
module with a different shape fails at load with an explicit message.

Parameters are events the container writes at the start of each block. Sample
accurate automation is not claimed in this prototype.

## Installed modules and host parameters

A catalogue package is installed under a content-addressed directory:

    <user-data>/OpenVST/modules/<module.id>/<version>-<package-digest>/
      install.json
      openvst.json
      patch/...
      ui.json

`install.json` is the installer's own document (schema `openvst.install/0`).
It records the package digest, the source and manifest digests, the validated
relative paths and the parameter table. The container never scans the archive
again: it reads `install.json`, verifies the directory name against the
package digest, and verifies the source bytes before compiling.

JUCE fixes the host parameter list when the processor is constructed, so a
catalogue module cannot add parameters at run time. The container therefore
exposes a frozen host union:

- built-in parameters keep their original positions and automation IDs;
- eight permanent generic slots follow them, `openvst.fx.module.1.v1` ...
  `openvst.fx.module.8.v1` for effects and `openvst.instr.module.1.v1` ...
  `openvst.instr.module.8.v1` for instruments, always normalised 0..1;
- an installed module maps its own parameters onto the slots in manifest
  order. The editor and the saved state keep the module's real parameter IDs,
  names and ranges; the host sees a normalised lane.

State schema 3 records `module`, `version`, `sourceSHA256`, `packageSHA256` and
the active module's parameter values. A restore looks for that exact source
digest and package digest in the built-in table and in the installed store. If
the version is absent, the restore is refused and the plugin mutes; it is never
substituted by another version or by a built-in with the same id. Schema 1 and
2 remain readable.

The patch file's `source` field is relative to the patch file itself, not to the
archive root: `patch/my-module.cmajorpatch` with `"source": "my-module.cmajor"`
resolves to `patch/my-module.cmajor`.

## What the format does not promise yet

- Custom HTML/JS interfaces. Blocked until isolation is demonstrated. The
  declarative document of contract v0 is data, not a web page, and it does not
  open the door to one.
- A visually verified module panel. The `openvst.ui/0` document is validated,
  hashed and parsed by the native renderer in `src/plugin/ModuleUI.cpp`, but
  the panel has not been opened in a DAW on a machine with a display yet.
- Sample or convolution resources beyond declared, hashed files.
- Any signature, author identity or revocation semantics.
