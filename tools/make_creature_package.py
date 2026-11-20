#!/usr/bin/env python3
"""Build and validate the reproducible Creature Voice package."""
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

SOURCE = ROOT / "modules" / "creature-voice"
FILES = {
    "openvst.json": "openvst.json",
    "patch/CreatureVoice.cmajor": "CreatureVoice.cmajor",
    "patch/CreatureVoice.cmajorpatch": "CreatureVoice.cmajorpatch",
    "ui.json": "ui.json",
}


def build(out: Path) -> dict:
    out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".creature-package-", dir=out.parent) as temp:
        draft = Path(temp) / out.name
        with zipfile.ZipFile(draft, "w", compression=zipfile.ZIP_STORED) as archive:
            for entry, file in FILES.items():
                info = zipfile.ZipInfo(entry, date_time=(1980, 1, 1, 0, 0, 0))
                info.create_system = 3
                info.external_attr = 0o100644 << 16
                archive.writestr(info, (SOURCE / file).read_text(encoding="utf-8").encode("utf-8"))
        report = validate_package(draft)
        if not report.ok:
            raise SystemExit("Creature Voice package rejected: " + json.dumps(report.to_dict()["errors"]))
        draft.replace(out)
    return {
        "path": str(out), "sha256": hashlib.sha256(out.read_bytes()).hexdigest(),
        "size": out.stat().st_size, "module": report.module, "parameters": report.parameters,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, nargs="?",
                        default=ROOT / "artifacts/creature/creature-voice-0.1.0.openvst.zip")
    print(json.dumps(build(parser.parse_args().output), indent=2))


if __name__ == "__main__":
    main()
