#!/usr/bin/env python3
"""Build the OpenVST demo module package used by the catalogue end-to-end test.

The module is a small stereo sine tremolo. It is a real Cmajor processor, not a
mock: the container compiles it from the installed package and renders audio.
The UI document is declarative data (openvst.ui/0), rendered natively.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from openvst.package_validation import validate_package  # noqa: E402

MODULE_ID = "org.openvst.demo.tremolo"
VERSION = "1.0.0"

CMAJOR = r"""// SPDX-License-Identifier: ISC
// OpenVST demo module: stereo sine tremolo.
processor Tremolo
{
    input stream float32<2> audioIn;
    input event float32 speed [[ name: "Rate", min: 0.1, max: 20.0, init: 2.0 ]];
    input event float32 amount [[ name: "Depth", min: 0.0, max: 1.0, init: 0.5 ]];
    input event float32 level [[ name: "Output gain", min: 0.0, max: 2.0, init: 1.0 ]];
    output stream float32<2> audioOut;

    float32 speedHz = 2.0f, amountAmt = 0.5f, outGain = 1.0f;
    float32 phase = 0.0f;

    event speed (float32 v)  { if (v == v) speedHz = clamp (v, 0.1f, 20.0f); }
    event amount (float32 v) { if (v == v) amountAmt = clamp (v, 0.0f, 1.0f); }
    event level (float32 v)  { if (v == v) outGain = clamp (v, 0.0f, 2.0f); }

    void main()
    {
        let sampleRate = float64 (processor.frequency);
        let inc = float32 (float64 (speedHz) / sampleRate);
        loop
        {
            float32[2] x;
            x[0] = audioIn[0];
            x[1] = audioIn[1];
            let lfo = 0.5f + 0.5f * float32 (sin (6.283185307179586 * float64 (phase)));
            let amp = outGain * (1.0f - amountAmt + amountAmt * lfo);
            float32[2] g;
            g[0] = amp;
            g[1] = amp;
            audioOut <- float32<2> (x[0] * g[0], x[1] * g[1]);
            advance();
            phase += inc;
            if (phase >= 1.0f) phase -= 1.0f;
        }
    }
}
"""

PATCH = {
    "CmajorVersion": 1,
    "ID": MODULE_ID,
    "version": VERSION,
    "name": "Tremolo",
    "mainProcessor": "Tremolo",
    "isInstrument": False,
    "source": "Tremolo.cmajor",
}

MANIFEST = {
    "schema": "openvst.package/0",
    "module": {
        "id": MODULE_ID,
        "version": VERSION,
        "name": "Tremolo",
        "kind": "effect",
        "vendor": "OpenVST",
        "license": "ISC",
    },
    "runtime": {
        "engine": "cmajor",
        "patch_format": 1,
        "host_api": "openvst-host/0",
        "patch": "patch/Tremolo.cmajorpatch",
    },
    "parameters": [
        {"id": "speed", "name": "Rate", "min": 0.1, "max": 20.0, "default": 2.0, "unit": "Hz"},
        {"id": "amount", "name": "Depth", "min": 0.0, "max": 1.0, "default": 0.5},
        {"id": "level", "name": "Output gain", "min": 0.0, "max": 2.0, "default": 1.0},
    ],
    "resources": [],
    "ui": "ui.json",
}

UI = {
    "schema": "openvst.ui/0",
    "title": "Tremolo",
    "layout": {
        "type": "column",
        "gap": 10,
        "padding": 14,
        "children": [
            {
                "type": "row",
                "gap": 12,
                "children": [
                    {"type": "knob", "param": "speed", "label": "Rate", "unit": "Hz", "size": "large"},
                    {"type": "knob", "param": "amount", "label": "Depth", "size": "large"},
                    {"type": "knob", "param": "level", "label": "Output", "size": "large"},
                ],
            },
            {
                "type": "group",
                "title": "Modulation",
                "gap": 6,
                "children": [
                    {"type": "value", "param": "speed", "label": "Rate", "precision": 2},
                    {"type": "value", "param": "amount", "label": "Depth", "precision": 2},
                    {"type": "value", "param": "level", "label": "Output", "precision": 2},
                ],
            },
            {"type": "label", "text": "Stereo amplitude modulation with a sine LFO."},
        ],
    },
}


def build(out: Path) -> dict:
    out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="openvst-demo-") as tmp:
        root = Path(tmp)
        (root / "patch").mkdir()
        (root / "openvst.json").write_text(json.dumps(MANIFEST, indent=2) + "\n", encoding="utf-8")
        (root / "patch" / "Tremolo.cmajor").write_text(CMAJOR, encoding="utf-8")
        (root / "patch" / "Tremolo.cmajorpatch").write_text(json.dumps(PATCH, indent=2) + "\n", encoding="utf-8")
        (root / "ui.json").write_text(json.dumps(UI, indent=2) + "\n", encoding="utf-8")

        # A module version is immutable: rebuilding the same sources must give
        # the same package bytes, or the catalogue digest would change under a
        # version that was already published. Fixed timestamps and a fixed
        # entry order remove the two sources of variation zipfile leaves in.
        with zipfile.ZipFile(out, "w", compression=zipfile.ZIP_DEFLATED) as zf:
            for rel in ("openvst.json", "patch/Tremolo.cmajor", "patch/Tremolo.cmajorpatch", "ui.json"):
                info = zipfile.ZipInfo(rel, date_time=(1980, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o644 << 16
                zf.writestr(info, (root / rel).read_bytes())

    report = validate_package(out)
    if not report.ok:
        raise SystemExit("demo package rejected: " + json.dumps(report.to_dict()["errors"], indent=2))
    digest = hashlib.sha256(out.read_bytes()).hexdigest()
    return {
        "path": str(out),
        "sha256": digest,
        "size": out.stat().st_size,
        "module": report.module,
        "parameters": report.parameters,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", nargs="?", default=str(ROOT / "artifacts" / "catalogue" / "seed" / "tremolo-1.0.0.openvst.zip"))
    args = parser.parse_args()
    result = build(Path(args.output))
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
