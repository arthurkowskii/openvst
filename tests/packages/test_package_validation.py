"""Tests for the OpenVST package validator (contract v0).

All packages built here are FICTIONAL test fixtures. They are not real
products, do not contain real DSP, and must never be distributed.

Run with:  python3 -m unittest discover -s tests/packages -v
"""

from __future__ import annotations

import hashlib
import json
import stat
import struct
import subprocess
import sys
import tempfile
import unittest
import warnings
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from openvst.package_validation import Limits, validate_package  # noqa: E402

MODULE_ID = "io.openvst.fixtures.validator"
VERSION = "1.2.0"
CMAJOR_SOURCE = b"processor Main [[main]]\n{\n    output stream float out;\n    out << 0.0f;\n}\n"


def _dump(obj: object) -> bytes:
    return json.dumps(obj, ensure_ascii=False).encode("utf-8")


def base_manifest() -> dict:
    return {
        "schema": "openvst.package/0",
        "module": {
            "id": MODULE_ID,
            "version": VERSION,
            "name": "Fixture Validator",
            "kind": "effect",
            "vendor": "OpenVST Fixtures",
            "license": "CC0-1.0",
        },
        "runtime": {
            "engine": "cmajor",
            "patch_format": 1,
            "host_api": "openvst-host/0",
            "patch": "patch/Main.cmajorpatch",
        },
        "parameters": [],
        "resources": [],
    }


def base_patch() -> dict:
    return {
        "CmajorVersion": 1,
        "ID": MODULE_ID,
        "version": VERSION,
        "name": "Fixture Validator",
        "manufacturer": "OpenVST Fixtures",
        "category": "fx",
        "isInstrument": False,
        "source": "Main.cmajor",
    }


def parameter(pid: str = "drive", name: str = "Drive", minimum: float = 0,
              maximum: float = 24, default: float = 6, **extra: object) -> dict:
    item = {"id": pid, "name": name, "min": minimum, "max": maximum, "default": default}
    item.update(extra)
    return item


def resource(path: str, data: bytes, license_name: str = "CC0-1.0") -> dict:
    return {"path": path, "sha256": hashlib.sha256(data).hexdigest(), "license": license_name}


def _write_zip(path: Path, entries: list[tuple[object, bytes]],
               compression: int = zipfile.ZIP_DEFLATED) -> Path:
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        with zipfile.ZipFile(path, "w", compression) as archive:
            for name, data in entries:
                archive.writestr(name, data)
    return path


def make_package(directory: Path, *, manifest: dict | None = None,
                 patch: dict | None = None, extra: list[tuple[object, bytes]] | None = None,
                 manifest_bytes: bytes | None = None, patch_bytes: bytes | None = None,
                 source: bytes = CMAJOR_SOURCE, omit_manifest: bool = False,
                 name: str = "fixture.zip",
                 compression: int = zipfile.ZIP_DEFLATED) -> Path:
    entries: list[tuple[object, bytes]] = []
    if not omit_manifest:
        entries.append(("openvst.json", manifest_bytes if manifest_bytes is not None
                        else _dump(base_manifest() if manifest is None else manifest)))
    entries.append(("patch/Main.cmajorpatch", patch_bytes if patch_bytes is not None
                    else _dump(base_patch() if patch is None else patch)))
    entries.append(("patch/Main.cmajor", source))
    entries.extend(extra or [])
    return _write_zip(directory / name, entries, compression)


def set_encrypted_flag(path: Path) -> None:
    data = bytearray(path.read_bytes())
    for signature, offset in ((b"PK\x03\x04", 6), (b"PK\x01\x02", 8)):
        position = 0
        while True:
            index = data.find(signature, position)
            if index < 0:
                break
            flags = struct.unpack_from("<H", data, index + offset)[0]
            struct.pack_into("<H", data, index + offset, flags | 0x01)
            position = index + 4
    path.write_bytes(bytes(data))


class PackageValidatorTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tempdir = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tempdir.name)

    def tearDown(self) -> None:
        self._tempdir.cleanup()

    def assert_error(self, report, code: str) -> None:
        self.assertFalse(report.ok, report.to_dict())
        self.assertIn(code, {issue.code for issue in report.errors},
                      [issue.to_dict() for issue in report.errors])

    # 1
    def test_valid_minimal_package(self) -> None:
        package = make_package(self.tmp)
        report = validate_package(package)
        self.assertTrue(report.ok, report.to_dict())
        self.assertEqual(report.errors, [])
        self.assertEqual(report.module["id"], MODULE_ID)
        self.assertEqual(report.module["version"], VERSION)
        self.assertEqual(report.package_sha256,
                         hashlib.sha256(package.read_bytes()).hexdigest())
        self.assertEqual({entry.path for entry in report.inventory},
                         {"openvst.json", "patch/Main.cmajorpatch", "patch/Main.cmajor"})
        self.assertTrue(report.to_dict()["not_validated"])

    # 2
    def test_valid_package_with_parameters_and_resources(self) -> None:
        impulse = b"RIFF-fictional-impulse-response"
        manifest = base_manifest()
        manifest["parameters"] = [
            parameter("drive", "Drive", 0, 24, 6, unit="dB", index=0),
            parameter("tone", "Tone", -1.0, 1.0, 0.0, index=1),
        ]
        manifest["resources"] = [resource("assets/ir_tape.wav", impulse)]
        package = make_package(self.tmp, manifest=manifest,
                               extra=[("assets/ir_tape.wav", impulse)])
        report = validate_package(package)
        self.assertTrue(report.ok, report.to_dict())
        inventory = {entry.path: entry for entry in report.inventory}
        self.assertEqual(inventory["assets/ir_tape.wav"].sha256,
                         hashlib.sha256(impulse).hexdigest())
        self.assertEqual(inventory["assets/ir_tape.wav"].size, len(impulse))
        self.assertEqual(len(report.parameters), 2)

    # 3
    def test_validation_is_read_only(self) -> None:
        package = make_package(self.tmp)
        before_bytes = package.read_bytes()
        before_mtime = package.stat().st_mtime_ns
        validate_package(package)
        self.assertEqual(package.read_bytes(), before_bytes)
        self.assertEqual(package.stat().st_mtime_ns, before_mtime)
        self.assertEqual(sorted(p.name for p in self.tmp.iterdir()), ["fixture.zip"])

    # 4
    def test_invalid_inputs(self) -> None:
        valid = make_package(self.tmp, name="valid.zip")
        not_zip = self.tmp / "not_zip.zip"
        not_zip.write_bytes(b"this is not a zip archive")
        truncated = self.tmp / "truncated.zip"
        truncated.write_bytes(valid.read_bytes()[: valid.stat().st_size // 2])
        prefixed = self.tmp / "prefixed.zip"
        prefixed.write_bytes(b"MZ" + valid.read_bytes())
        corrupt = make_package(self.tmp, name="corrupt.zip", compression=zipfile.ZIP_STORED)
        raw = bytearray(corrupt.read_bytes())
        raw[raw.find(CMAJOR_SOURCE) + 1] ^= 0xFF
        corrupt.write_bytes(bytes(raw))
        directory = self.tmp / "directory.zip"
        directory.mkdir()
        cases = [
            ("missing", self.tmp / "missing.zip", "package_not_found"),
            ("directory", directory, "package_not_file"),
            ("not_zip", not_zip, "archive_invalid"),
            ("prefixed", prefixed, "archive_invalid"),
            ("truncated", truncated, "archive_invalid"),
            ("corrupt_crc", corrupt, "archive_entry_corrupt"),
        ]
        for label, path, code in cases:
            with self.subTest(case=label):
                self.assert_error(validate_package(path), code)

    # 5
    def test_empty_archive_is_never_valid(self) -> None:
        empty = self.tmp / "empty.zip"
        with zipfile.ZipFile(empty, "w"):
            pass
        report = validate_package(empty)
        self.assertFalse(report.ok)
        self.assert_error(report, "manifest_missing")

    # 6
    def test_manifest_missing(self) -> None:
        package = make_package(self.tmp, omit_manifest=True)
        self.assert_error(validate_package(package), "manifest_missing")

    # 7
    def test_manifest_json_strictness(self) -> None:
        cases = [
            ("malformed", b'{"schema": ', "manifest_json_invalid"),
            ("duplicate_key", b'{"schema":"openvst.package/0","schema":"openvst.package/0"}',
             "manifest_duplicate_key"),
            ("nan", b'{"schema":NaN}', "manifest_json_invalid"),
        ]
        for label, raw, code in cases:
            with self.subTest(case=label):
                package = make_package(self.tmp, manifest_bytes=raw, name=f"{label}.zip")
                self.assert_error(validate_package(package), code)

    # 8
    def test_manifest_schema_and_unknown_fields(self) -> None:
        wrong_schema = base_manifest()
        wrong_schema["schema"] = "openvst.package/1"
        unknown_field = base_manifest()
        unknown_field["signature"] = "detached-signature-is-not-inside-the-zip"
        for label, manifest, code in (
            ("schema", wrong_schema, "manifest_schema_unsupported"),
            ("unknown", unknown_field, "manifest_field_unknown"),
        ):
            with self.subTest(case=label):
                package = make_package(self.tmp, manifest=manifest, name=f"schema_{label}.zip")
                self.assert_error(validate_package(package), code)

    # 9
    def test_manifest_strict_types(self) -> None:
        def patch_format_bool(manifest: dict) -> None:
            manifest["runtime"]["patch_format"] = True

        def parameter_min_bool(manifest: dict) -> None:
            manifest["parameters"] = [{"id": "x", "name": "X", "min": False,
                                       "max": 1, "default": 0}]

        def version_number(manifest: dict) -> None:
            manifest["module"]["version"] = 1.2

        for label, mutate in (("patch_format_bool", patch_format_bool),
                              ("parameter_min_bool", parameter_min_bool),
                              ("version_number", version_number)):
            with self.subTest(case=label):
                manifest = base_manifest()
                mutate(manifest)
                package = make_package(self.tmp, manifest=manifest, name=f"type_{label}.zip")
                self.assert_error(validate_package(package), "manifest_type")

    # 10
    def test_module_identity_invalid(self) -> None:
        for label, key, value in (
            ("uppercase_id", "id", "io.openvst.Bad_ID"),
            ("single_label_id", "id", "nodot"),
            ("short_version", "version", "1.2"),
        ):
            with self.subTest(case=label):
                manifest = base_manifest()
                manifest["module"][key] = value
                package = make_package(self.tmp, manifest=manifest, name=f"id_{label}.zip")
                self.assert_error(validate_package(package), "manifest_value")

    # 11
    def test_patch_identity_mismatch(self) -> None:
        cases = [
            ("version", {"version": "9.9.9"}),
            ("id", {"ID": "io.other.fixture.patch"}),
        ]
        for label, override in cases:
            with self.subTest(case=label):
                patch = base_patch()
                patch.update(override)
                package = make_package(self.tmp, patch=patch, name=f"identity_{label}.zip")
                self.assert_error(validate_package(package), "patch_identity_mismatch")

    # 12
    def test_patch_missing(self) -> None:
        manifest = base_manifest()
        manifest["runtime"]["patch"] = "patch/Absent.cmajorpatch"
        package = make_package(self.tmp, manifest=manifest)
        self.assert_error(validate_package(package), "patch_missing")

    # 13
    def test_patch_source_missing(self) -> None:
        patch = base_patch()
        patch["source"] = "Absent.cmajor"
        package = make_package(self.tmp, patch=patch)
        self.assert_error(validate_package(package), "patch_source_missing")

    # 14
    def test_parameter_duplicate_id(self) -> None:
        manifest = base_manifest()
        manifest["parameters"] = [parameter("drive"), parameter("drive", "Drive Again", 0, 10, 5)]
        package = make_package(self.tmp, manifest=manifest)
        self.assert_error(validate_package(package), "parameter_duplicate_id")

    # 15
    def test_parameter_bounds(self) -> None:
        for label, params in (
            ("default_above_max", [parameter(default=30)]),
            ("min_above_max", [parameter(minimum=24, maximum=0, default=6)]),
            ("duplicate_index", [parameter(index=0), parameter("tone", index=0)]),
        ):
            with self.subTest(case=label):
                manifest = base_manifest()
                manifest["parameters"] = params
                package = make_package(self.tmp, manifest=manifest, name=f"param_{label}.zip")
                self.assert_error(validate_package(package), "parameter_range")

    # 16
    def test_resources_hash_mismatch(self) -> None:
        declared = resource("assets/ir_tape.wav", b"other bytes")
        manifest = base_manifest()
        manifest["resources"] = [declared]
        package = make_package(self.tmp, manifest=manifest,
                               extra=[("assets/ir_tape.wav", b"actual bytes")])
        self.assert_error(validate_package(package), "resource_hash_mismatch")

    # 17
    def test_resource_declaration_errors(self) -> None:
        missing = base_manifest()
        missing["resources"] = [resource("assets/ir_tape.wav", b"declared but absent")]
        undeclared = base_manifest()
        for label, manifest, extra, code in (
            ("missing", missing, [], "resource_missing"),
            ("undeclared", undeclared, [("assets/extra.wav", b"not declared")],
             "resource_undeclared"),
        ):
            with self.subTest(case=label):
                package = make_package(self.tmp, manifest=manifest, extra=extra,
                                       name=f"resource_{label}.zip")
                self.assert_error(validate_package(package), code)

    # 18
    def test_dangerous_paths(self) -> None:
        cases = {
            "traversal": "../evil.txt",
            "absolute": "/etc/evil.txt",
            "backslash": "assets\\evil.wav",
            "windows_drive": "C:/evil.txt",
            "ads": "assets/ir.wav:hidden",
            "not_nfc": "assets/cafe\u0301.wav",
            "control_char": "assets/ev\x01il.wav",
            "windows_reserved": "assets/CON.wav",
            "trailing_dot": "assets/foo./ir.wav",
            "trailing_space": "assets/foo /ir.wav",
            "dot_component": "assets/./ir.wav",
        }
        for label, entry_name in cases.items():
            with self.subTest(case=label):
                package = make_package(self.tmp, extra=[(entry_name, b"x")],
                                       name=f"path_{label}.zip")
                self.assert_error(validate_package(package), "archive_path_unsafe")

    # 19
    def test_path_collisions(self) -> None:
        cases = (
            ("duplicate_name", [("openvst.json", _dump(base_manifest()))],
             "archive_path_collision"),
            ("casefold", [("assets/IR.wav", b"a"), ("assets/ir.wav", b"b")],
             "archive_path_collision"),
            ("file_directory", [("assets/ir.wav", b"a"),
                                ("assets/ir.wav/nested.wav", b"b")],
             "archive_file_directory_conflict"),
        )
        for label, extra, code in cases:
            with self.subTest(case=label):
                package = make_package(self.tmp, extra=extra, name=f"collision_{label}.zip")
                self.assert_error(validate_package(package), code)

    # 20
    def test_hostile_entries(self) -> None:
        symlink = zipfile.ZipInfo("patch/Link.cmajor")
        symlink.create_system = 3
        symlink.external_attr = (stat.S_IFLNK | 0o777) << 16
        package = make_package(self.tmp, extra=[(symlink, b"Main.cmajor")],
                               name="symlink.zip")
        self.assert_error(validate_package(package), "archive_entry_type")

        encrypted = make_package(self.tmp, name="encrypted.zip")
        set_encrypted_flag(encrypted)
        self.assert_error(validate_package(encrypted), "archive_encrypted")

    # 21
    def test_limits(self) -> None:
        cases = (
            ("entry_size", Limits(max_entry_uncompressed=256),
             [("assets/big.wav", b"\0" * 1024)], "archive_entry_too_large"),
            ("total_size", Limits(max_total_uncompressed=100), [],
             "archive_extracted_too_large"),
            ("ratio", Limits(max_compression_ratio=2),
             [("assets/bomb.wav", b"\0" * 8192)], "archive_ratio_exceeded"),
            ("entry_count", Limits(max_entries=2), [], "archive_entry_count"),
        )
        for label, limits, extra, code in cases:
            with self.subTest(case=label):
                package = make_package(self.tmp, extra=extra, name=f"limits_{label}.zip")
                self.assert_error(validate_package(package, limits=limits), code)

    # 22
    def test_forbidden_suffixes(self) -> None:
        for label, entry_name in (
            ("javascript", "patch/worker.js"),
            ("native_library", "bin/plugin.dll"),
            ("executable", "assets/payload.exe"),
        ):
            with self.subTest(case=label):
                package = make_package(self.tmp, extra=[(entry_name, b"payload")],
                                       name=f"suffix_{label}.zip")
                self.assert_error(validate_package(package), "file_forbidden_suffix")

    # 23
    def test_cli_valid(self) -> None:
        package = make_package(self.tmp, name="cli_valid.zip")
        result = subprocess.run([sys.executable, str(ROOT / "scripts/package_validate.py"),
                                 str(package)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        payload = json.loads(result.stdout)
        self.assertTrue(payload["ok"])
        self.assertEqual(payload["module"]["id"], MODULE_ID)

    # 24
    def test_cli_invalid(self) -> None:
        manifest = base_manifest()
        manifest["schema"] = "openvst.package/1"
        package = make_package(self.tmp, manifest=manifest, name="cli_invalid.zip")
        result = subprocess.run([sys.executable, str(ROOT / "scripts/package_validate.py"),
                                 str(package)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 1, result.stderr)
        payload = json.loads(result.stdout)
        self.assertFalse(payload["ok"])
        self.assertIn("manifest_schema_unsupported",
                      {issue["code"] for issue in payload["errors"]})

    # 25
    def test_cli_usage_errors(self) -> None:
        package = make_package(self.tmp, name="cli_usage.zip")
        for label, args in (
            ("missing_path", []),
            ("unknown_option", ["--verbose"]),
            ("two_paths", [str(package), str(package)]),
            ("stdin_dash", ["-"]),
        ):
            with self.subTest(case=label):
                result = subprocess.run([sys.executable,
                                         str(ROOT / "scripts/package_validate.py"), *args],
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 2, result.stderr)
                payload = json.loads(result.stdout)
                self.assertFalse(payload["ok"])
                self.assertEqual(payload["errors"][0]["code"], "cli_usage")

        missing = subprocess.run([sys.executable,
                                  str(ROOT / "scripts/package_validate.py"),
                                  str(self.tmp / "absent.zip")],
                                 capture_output=True, text=True)
        self.assertEqual(missing.returncode, 1, missing.stderr)
        self.assertEqual(json.loads(missing.stdout)["errors"][0]["code"],
                         "package_not_found")


if __name__ == "__main__":
    unittest.main()
