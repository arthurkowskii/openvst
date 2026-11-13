#!/usr/bin/env python3
"""Validate one OpenVST package archive and print a JSON report.

Usage: python3 scripts/package_validate.py <package.zip>

Exit codes: 0 valid, 1 invalid/unreadable package, 2 malformed command line.
The archive is only read; nothing is extracted or executed.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from openvst.package_validation import validate_package  # noqa: E402

USAGE = "usage: python3 scripts/package_validate.py <package.zip>"


def _print(payload: object) -> None:
    print(json.dumps(payload, ensure_ascii=False, indent=2, sort_keys=True))


def _usage_error(message: str) -> int:
    print(USAGE, file=sys.stderr)
    _print({"ok": False, "errors": [{"code": "cli_usage", "message": message,
                                     "path": None}]})
    return 2


def main(argv: list[str]) -> int:
    if len(argv) != 1:
        return _usage_error("exactly one package path is required")
    argument = argv[0]
    if argument.startswith("-"):
        return _usage_error(f"options are not supported: {argument!r}")
    report = validate_package(argument)
    _print(report.to_dict())
    return 0 if report.ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
