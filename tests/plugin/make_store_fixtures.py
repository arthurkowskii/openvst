#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
"""Generate the ZIP fixtures used by ModuleStoreTests.

Deterministic: fixed timestamps, fixed entry order, no extra metadata. A
regenerated tree only changes when this file changes, so a test failure is
never a rebuild artefact.

Usage: python3 tests/plugin/make_store_fixtures.py OUTDIR
"""

import json
import os
import sys
import zipfile

MODULE_ID = "org.openvst.test.tone"
VERSION = "1.0.0"
NEWER_VERSION = "1.1.0"

SOURCE = """processor Tone
{
    input  stream float32<2> audioIn;
    output stream float32<2> audioOut;
    input  event  float32   freq;
    input  event  float32   gain;

    void main()
    {
        loop
        {
            let frame = audioIn;
            audioOut <- frame * gain;
            advance();
        }
    }
}
"""

UI_DOCUMENT = {
    "schema": "openvst.ui/0",
    "title": "Test Tone",
    "layout": {
        "type": "column",
        "children": [
            {"type": "label", "text": "Test Tone"}
        ],
    },
}


def manifest(version, ui=False):
    document = {
        "schema": "openvst.package/0",
        "module": {
            "id": MODULE_ID,
            "version": version,
            "name": "Test Tone",
            "kind": "effect",
            "vendor": "OpenVST",
            "license": "ISC",
        },
        "runtime": {
            "engine": "cmajor",
            "patch_format": 1,
            "host_api": "openvst-host/0",
            "patch": "patch/tone.cmajorpatch",
        },
        "parameters": [
            {
                "id": "tone.freq",
                "name": "Frequency",
                "min": 20,
                "max": 20000,
                "default": 440,
                "unit": "Hz",
            },
            {
                "id": "tone.gain",
                "name": "Gain",
                "min": 0,
                "max": 1,
                "default": 0.5,
            },
        ],
        "resources": [],
    }
    if ui:
        document["ui"] = "ui.json"
    return document


def patch(version, patch_id=MODULE_ID, extra=None):
    document = {
        "CmajorVersion": 1,
        "ID": patch_id,
        "version": version,
        "name": "Test Tone",
        "isInstrument": False,
        # The patch resolves its source against its own directory, as
        # openvst/package_validation.py enforces.
        "source": "tone.cmajor",
        "mainProcessor": "Tone",
    }
    if extra:
        document.update(extra)
    return document


def document_bytes(document):
    return (json.dumps(document, indent=2, sort_keys=True) + "\n").encode("ascii")


def package_entries(version=VERSION, patch_id=MODULE_ID, patch_extra=None, ui=False):
    entries = [
        ("openvst.json", document_bytes(manifest(version, ui))),
        ("patch/tone.cmajorpatch", document_bytes(patch(version, patch_id, patch_extra))),
        ("patch/tone.cmajor", SOURCE.encode("ascii")),
    ]
    if ui:
        entries.append(("ui.json", document_bytes(UI_DOCUMENT)))
    return entries


def write_zip(path, entries):
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
        for name, data, *rest in entries:
            info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            if rest:
                info.external_attr = rest[0]
            archive.writestr(info, data)


SYMLINK_ATTRIBUTES = 0o120777 << 16


def main():
    if len(sys.argv) != 2:
        print("usage: make_store_fixtures.py OUTDIR", file=sys.stderr)
        return 2

    out = os.path.abspath(sys.argv[1])
    os.makedirs(out, exist_ok=True)

    write_zip(os.path.join(out, "valid.openvst.zip"), package_entries())
    write_zip(os.path.join(out, "valid-ui.openvst.zip"), package_entries(ui=True))
    write_zip(os.path.join(out, "valid-newer.openvst.zip"), package_entries(version=NEWER_VERSION))

    write_zip(os.path.join(out, "traversal.openvst.zip"),
              package_entries() + [("../evil.cmajor", SOURCE.encode("ascii"))])
    write_zip(os.path.join(out, "absolute.openvst.zip"),
              package_entries() + [("/evil.cmajor", SOURCE.encode("ascii"))])
    write_zip(os.path.join(out, "backslash.openvst.zip"),
              package_entries() + [("patch\\evil.cmajor", SOURCE.encode("ascii"))])

    write_zip(os.path.join(out, "mismatch.openvst.zip"),
              package_entries(patch_id="org.openvst.test.other"))
    write_zip(os.path.join(out, "worker.openvst.zip"),
              package_entries(patch_extra={"worker": "tone.js"}))

    write_zip(os.path.join(out, "symlink.openvst.zip"),
              package_entries() + [("patch/link.cmajor", "../tone.cmajor", SYMLINK_ATTRIBUTES)])

    write_zip(os.path.join(out, "duplicate-case.openvst.zip"),
              package_entries() + [
                  ("UI.json", document_bytes(UI_DOCUMENT)),
                  ("ui.json", document_bytes(UI_DOCUMENT)),
              ])

    print("fixtures written to " + out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
