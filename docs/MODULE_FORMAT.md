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
      "resources": []
    }

Rules enforced by `openvst/package_validation.py`:

- `module.id` is a lowercase reverse-DNS identifier and never changes.
- `module.version` is SemVer, ASCII digits only.
- The patch file declares the same ID and version.
- Parameter IDs are unique, permanent, and bounded by `min` and `max`.
- Every declared resource exists and matches its SHA-256.
- The archive contains no workers, no author script, no external reference.
- Paths that are dangerous on Windows are rejected, not sanitised.

## DSP contract

An effect declares `audioIn` and `audioOut` as `float32<2>` streams. An
instrument declares `midiIn` as an event endpoint and `audioOut` as a
`float32<2>` stream. The container validates endpoints before linking, so a
module with a different shape fails at load with an explicit message.

Parameters are events the container writes at the start of each block. Sample
accurate automation is not claimed in this prototype.

## What the format does not promise yet

- Custom HTML/JS interfaces. Blocked until isolation is demonstrated.
- Sample or convolution resources beyond declared, hashed files.
- Any signature, author identity or revocation semantics.
