"""Regression tests for parent review findings (contract v0).

All packages built here are FICTIONAL test fixtures.

Run with:  python3 -m unittest discover -s tests/packages -v
"""

from __future__ import annotations

import hashlib
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

import openvst.package_validation as pv  # noqa: E402
from openvst.package_validation import Limits, validate_package  # noqa: E402
from test_package_validation import (  # noqa: E402
    MODULE_ID,
    VERSION,
    base_manifest,
    base_patch,
    make_package,
    resource,
)


class ReviewRegressionTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tempdir = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tempdir.name)

    def tearDown(self) -> None:
        self._tempdir.cleanup()

    def assert_error(self, report, code: str) -> None:
        self.assertFalse(report.ok, report.to_dict())
        self.assertIn(code, {issue.code for issue in report.errors},
                      [issue.to_dict() for issue in report.errors])

    def package_with_patch_fields(self, name: str, **fields: object) -> Path:
        patch = base_patch()
        patch.update(fields)
        return make_package(self.tmp, patch=patch, name=name)

    # worker / view / external references must never pass v0
    def test_patch_script_and_external_references_rejected(self) -> None:
        cases = {
            "worker_remote": {"worker": "https://example.invalid/worker.js"},
            "worker_local_absent": {"worker": "worker.js"},
            "view_remote": {"view": {"src": "https://example.invalid/view.js"}},
            "view_local": {"view": {"src": "view/index.js", "width": 800, "height": 600}},
            "view_array": {"view": [{"src": "view/index.js"}]},
            "views_array": {"views": [{"src": "view/index.js"}]},
            "source_transformer": {"sourceTransformer": "transform.js"},
            "patch_resources": {"resources": ["*.wav"]},
            "externals": {"externals": {"Main::samples": "https://example.invalid/a.wav"}},
        }
        for label, fields in cases.items():
            with self.subTest(case=label):
                package = self.package_with_patch_fields(f"script_{label}.zip", **fields)
                self.assert_error(validate_package(package), "patch_field_unknown")

    def test_patch_allows_standard_non_scripted_fields(self) -> None:
        patch = base_patch()
        patch.update({
            "description": "Fictional test patch",
            "mainProcessor": "Main",
            "manufacturer": "OpenVST Fixtures",
            "category": "fx",
            "isInstrument": False,
        })
        report = validate_package(make_package(self.tmp, patch=patch, name="allowed.zip"))
        self.assertTrue(report.ok, report.to_dict())

    def test_patch_kind_must_match_is_instrument(self) -> None:
        instrument_manifest = base_manifest()
        instrument_manifest["module"]["kind"] = "instrument"
        instrument_patch = base_patch()
        instrument_patch["isInstrument"] = True
        report = validate_package(make_package(
            self.tmp, manifest=instrument_manifest, patch=instrument_patch,
            name="instrument_ok.zip"))
        self.assertTrue(report.ok, report.to_dict())

        cases = {
            "effect_flagged_instrument": (base_manifest(), dict(isInstrument=True)),
            "instrument_flagged_effect": (instrument_manifest, dict(isInstrument=False)),
            "instrument_flag_absent": (instrument_manifest, {}),
        }
        for label, (manifest, fields) in cases.items():
            with self.subTest(case=label):
                patch = base_patch()
                patch.update(fields)
                if "isInstrument" in fields:
                    patch["isInstrument"] = fields["isInstrument"]
                else:
                    patch.pop("isInstrument", None)
                package = make_package(self.tmp, manifest=manifest, patch=patch,
                                       name=f"kind_{label}.zip")
                self.assert_error(validate_package(package), "patch_kind_mismatch")

    def test_windows_forbidden_characters_rejected(self) -> None:
        for label, char in (("question", "?"), ("star", "*"), ("less", "<"),
                            ("greater", ">"), ("pipe", "|"), ("quote", '"')):
            with self.subTest(case=label):
                package = make_package(self.tmp, extra=[(f"assets/a{char}.wav", b"x")],
                                       name=f"char_{label}.zip")
                self.assert_error(validate_package(package), "archive_path_unsafe")

    def test_windows_superscript_device_names_rejected(self) -> None:
        for label, entry in (("com1", "assets/COM\u00b9.wav"),
                             ("com2", "assets/com\u00b2.wav"),
                             ("lpt3", "assets/LPT\u00b3.wav")):
            with self.subTest(case=label):
                package = make_package(self.tmp, extra=[(entry, b"x")],
                                       name=f"device_{label}.zip")
                self.assert_error(validate_package(package), "archive_path_unsafe")

    def test_semver_is_ascii_only(self) -> None:
        valid = ("1.2.3", "0.0.1", "1.2.3-alpha.1", "1.2.3+build.5",
                 "1.2.3-rc.1+build.5")
        for version in valid:
            with self.subTest(valid=version):
                manifest = base_manifest()
                manifest["module"]["version"] = version
                patch = base_patch()
                patch["version"] = version
                report = validate_package(make_package(
                    self.tmp, manifest=manifest, patch=patch, name="semver_ok.zip"))
                self.assertTrue(report.ok, report.to_dict())

        invalid = ("1\u0662.0.0", "\u0661.2.3", "1.2.3-\u0662", "\uff11.\uff12.\uff13")
        for version in invalid:
            with self.subTest(invalid=version):
                manifest = base_manifest()
                manifest["module"]["version"] = version
                patch = base_patch()
                patch["version"] = version
                package = make_package(self.tmp, manifest=manifest, patch=patch,
                                       name="semver_bad.zip")
                self.assert_error(validate_package(package), "manifest_value")

    def test_hash_and_validated_bytes_share_one_snapshot(self) -> None:
        original = make_package(self.tmp, name="snapshot_original.zip")
        original_bytes = original.read_bytes()
        other_manifest = base_manifest()
        other_manifest["module"]["id"] = "io.openvst.fixtures.replacement"
        other_patch = base_patch()
        other_patch["ID"] = "io.openvst.fixtures.replacement"
        replacement = make_package(self.tmp, name="snapshot_replacement.zip",
                                   manifest=other_manifest, patch=other_patch)
        real_hash = pv._hash_package

        def hash_then_swap(*args):
            result = real_hash(*args)
            os.replace(replacement, original)
            return result

        with mock.patch.object(pv, "_hash_package", hash_then_swap):
            report = pv.validate_package(original)
        self.assertTrue(report.ok, report.to_dict())
        self.assertEqual(report.package_sha256,
                         hashlib.sha256(original_bytes).hexdigest())
        self.assertEqual(report.package_size, len(original_bytes))
        self.assertEqual(report.module["id"], MODULE_ID)

    def test_in_place_mutation_during_validation_is_detected(self) -> None:
        package = make_package(self.tmp, name="mutating.zip")
        real_hash = pv._hash_package

        def hash_then_append(*args):
            result = real_hash(*args)
            with open(package, "ab") as extra:
                extra.write(b"appended-after-hash")
            return result

        with mock.patch.object(pv, "_hash_package", hash_then_append):
            report = pv.validate_package(package)
        self.assert_error(report, "package_changed_during_validation")

    def test_unexpected_validator_failure_is_reported_not_raised(self) -> None:
        package = make_package(self.tmp, name="unexpected.zip")
        with mock.patch.object(pv, "_check_archive",
                               side_effect=RuntimeError("injected failure")):
            report = pv.validate_package(package)
        self.assert_error(report, "validation_error")
        self.assertIn("RuntimeError", report.errors[0].message)


if __name__ == "__main__":
    unittest.main()
