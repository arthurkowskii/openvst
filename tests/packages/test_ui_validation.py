"""Tests for the OpenVST module UI document validation (openvst.ui/0).

All packages built here are FICTIONAL test fixtures. They are not real
products, do not contain real DSP, and must never be distributed.

Run with:  python3 -m unittest discover -s tests/packages -v
"""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from openvst.package_validation import validate_package  # noqa: E402
from test_package_validation import (  # noqa: E402
    base_manifest,
    base_patch,
    make_package,
    parameter,
)

UI_PATH = "ui.json"


def _dump(obj: object) -> bytes:
    return json.dumps(obj, ensure_ascii=False).encode("utf-8")


def ui_document(**overrides: object) -> dict:
    document: dict = {
        "schema": "openvst.ui/0",
        "title": "Transient Shaper",
        "layout": {
            "type": "column",
            "gap": 8,
            "padding": 12,
            "children": [
                {"type": "knob", "param": "attack", "label": "Attack", "unit": "dB"},
                {"type": "value", "param": "attack", "label": "Attack", "precision": 1},
            ],
        },
    }
    document.update(overrides)
    return document


def manifest_with_ui(ui_path: str = UI_PATH) -> dict:
    manifest = base_manifest()
    manifest["parameters"] = [
        parameter("attack", "Attack", -12, 12, 0, unit="dB", index=0),
        parameter("sustain", "Sustain", -12, 12, 0, unit="dB", index=1),
        parameter("gain", "Output gain", 0, 2, 1, index=2),
        parameter("bypass", "Bypass", 0, 1, 0, index=3),
    ]
    manifest["ui"] = ui_path
    return manifest


class UiValidationTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tempdir = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tempdir.name)

    def tearDown(self) -> None:
        self._tempdir.cleanup()

    def assert_error(self, report, code: str) -> None:
        self.assertFalse(report.ok, report.to_dict())
        self.assertIn(code, {issue.code for issue in report.errors},
                      [issue.to_dict() for issue in report.errors])

    def package_with_ui(self, document: object | None = None, *,
                        manifest: dict | None = None,
                        ui_bytes: bytes | None = None,
                        ui_path: str = UI_PATH,
                        name: str = "ui.zip",
                        compression: int = zipfile.ZIP_DEFLATED):
        manifest = manifest if manifest is not None else manifest_with_ui(ui_path)
        payload = ui_bytes if ui_bytes is not None else _dump(
            ui_document() if document is None else document)
        return make_package(self.tmp, manifest=manifest,
                            extra=[(ui_path, payload)], name=name,
                            compression=compression)

    # 1: a valid package with a UI document
    def test_valid_package_with_ui_document(self) -> None:
        package = self.package_with_ui()
        report = validate_package(package)
        self.assertTrue(report.ok, report.to_dict())
        inventory = {entry.path: entry for entry in report.inventory}
        self.assertIn(UI_PATH, inventory)
        self.assertEqual(inventory[UI_PATH].size, len(_dump(ui_document())))

    def test_repository_example_ui_document_is_valid(self) -> None:
        example = ROOT / "fixtures" / "ui" / "transient-shaper.ui.json"
        self.assertTrue(example.is_file(), example)
        document = json.loads(example.read_text(encoding="utf-8"))
        manifest = manifest_with_ui()
        manifest["parameters"] = [
            parameter("attack", "Attack", -12, 12, 0, unit="dB", index=0),
            parameter("sustain", "Sustain", -12, 12, 0, unit="dB", index=1),
            parameter("gain", "Output gain", 0, 2, 1, index=2),
        ]
        package = self.package_with_ui(
            document, manifest=manifest, ui_bytes=example.read_bytes(),
            name="example.zip")
        report = validate_package(package)
        self.assertTrue(report.ok, report.to_dict())

    # 2: the declared UI document is absent from the archive
    def test_ui_document_missing_from_archive(self) -> None:
        manifest = manifest_with_ui()
        package = make_package(self.tmp, manifest=manifest, name="missing_ui.zip")
        self.assert_error(validate_package(package), "ui_missing")

    # 3: an unknown top-level key
    def test_unknown_top_level_key(self) -> None:
        document = ui_document()
        document["stylesheet"] = "body { color: red }"
        package = self.package_with_ui(document, name="unknown_top.zip")
        self.assert_error(validate_package(package), "ui_unknown_key")

    # 4: an unknown component type
    def test_unknown_component_type(self) -> None:
        document = ui_document()
        document["layout"]["children"].append({"type": "xy-pad", "param": "attack"})
        package = self.package_with_ui(document, name="unknown_type.zip")
        self.assert_error(validate_package(package), "ui_unknown_type")

    # 5: a parameter that the manifest does not declare
    def test_param_not_declared_in_manifest(self) -> None:
        document = ui_document()
        document["layout"]["children"].append({"type": "knob", "param": "release"})
        package = self.package_with_ui(document, name="param_unknown.zip")
        self.assert_error(validate_package(package), "ui_param_unknown")

    # 6: a third binding of the same parameter
    def test_param_bound_three_times(self) -> None:
        document = ui_document()
        document["layout"]["children"].append({"type": "slider", "param": "attack"})
        package = self.package_with_ui(document, name="param_triple.zip")
        self.assert_error(validate_package(package), "ui_param_duplicate_binding")

    def test_param_bound_twice_is_accepted(self) -> None:
        # The example document binds 'attack' twice, once as a knob and once as
        # a readout. Two bindings are legal; the third is not.
        package = self.package_with_ui(name="param_twice.zip")
        report = validate_package(package)
        self.assertTrue(report.ok, report.to_dict())

    # 7: bounds
    def test_depth_limit(self) -> None:
        node: dict = {"type": "label", "text": "leaf"}
        for _ in range(8):
            node = {"type": "column", "children": [node]}
        document = {"schema": "openvst.ui/0", "layout": node}
        package = self.package_with_ui(document, name="deep.zip")
        self.assert_error(validate_package(package), "ui_bounds")

    def test_node_count_limit(self) -> None:
        children = [{"type": "label", "text": f"row {index}"} for index in range(128)]
        document = {"schema": "openvst.ui/0",
                    "layout": {"type": "column", "children": children}}
        package = self.package_with_ui(document, name="wide.zip")
        self.assert_error(validate_package(package), "ui_bounds")

    def test_label_length_limit(self) -> None:
        document = ui_document()
        document["layout"]["children"].append({"type": "label", "text": "x" * 121})
        package = self.package_with_ui(document, name="long_label.zip")
        self.assert_error(validate_package(package), "ui_bounds")

    # 8: forbidden keys
    def test_forbidden_keys(self) -> None:
        cases = {
            "url": {"type": "knob", "param": "attack", "url": "https://example.invalid/x"},
            "script": {"type": "knob", "param": "attack", "script": "onload()"},
            "style": {"type": "knob", "param": "attack", "style": "color: red"},
            "html": {"type": "knob", "param": "attack", "html": "<b>x</b>"},
        }
        for label, node in cases.items():
            with self.subTest(case=label):
                document = ui_document()
                document["layout"]["children"].append(node)
                package = self.package_with_ui(document, name=f"forbidden_{label}.zip")
                self.assert_error(validate_package(package), "ui_unknown_key")

    def test_forbidden_top_level_keys(self) -> None:
        for label, key in (("url", "url"), ("script", "script"),
                           ("style", "style"), ("html", "html")):
            with self.subTest(case=label):
                document = ui_document()
                document[key] = "anything"
                package = self.package_with_ui(document, name=f"top_{label}.zip")
                self.assert_error(validate_package(package), "ui_unknown_key")

    # 9: malformed JSON and duplicate keys
    def test_ui_json_strictness(self) -> None:
        cases = {
            "malformed": b'{"schema": "openvst.ui/0", ',
            "duplicate_key": b'{"schema":"openvst.ui/0","schema":"openvst.ui/0"}',
            "nan": b'{"schema":"openvst.ui/0","layout":{"gap":NaN}}',
        }
        for label, raw in cases.items():
            with self.subTest(case=label):
                package = self.package_with_ui(ui_bytes=raw, name=f"json_{label}.zip")
                self.assert_error(validate_package(package), "ui_invalid_json")

    # 10: unsafe manifest paths
    def test_unsafe_ui_paths_are_never_accepted(self) -> None:
        cases = {
            "traversal": "../ui.json",
            "absolute": "/ui.json",
            "backslash": "ui\\panel.json",
            "drive": "C:/ui.json",
        }
        for label, path in cases.items():
            with self.subTest(case=label):
                manifest = manifest_with_ui(path)
                package = make_package(self.tmp, manifest=manifest,
                                       extra=[("ui.json", _dump(ui_document()))],
                                       name=f"unsafe_{label}.zip")
                report = validate_package(package)
                self.assertFalse(report.ok, report.to_dict())
                codes = {issue.code for issue in report.errors}
                self.assertTrue(codes & {"ui_missing", "manifest_value",
                                         "archive_path_unsafe"},
                                [issue.to_dict() for issue in report.errors])

    def test_ui_path_must_end_with_json(self) -> None:
        # The archive holds no such file, so the manifest declaration itself is
        # what must be refused.
        manifest = manifest_with_ui("ui.txt")
        package = make_package(self.tmp, manifest=manifest, name="suffix.zip")
        self.assert_error(validate_package(package), "manifest_value")

    # 11: a toggle over a parameter whose range is wider than 0..1
    def test_toggle_requires_a_unit_range_parameter(self) -> None:
        document = {
            "schema": "openvst.ui/0",
            "layout": {
                "type": "column",
                "children": [
                    {"type": "toggle", "param": "attack", "label": "Attack"},
                ],
            },
        }
        package = self.package_with_ui(document, name="toggle_range.zip")
        self.assert_error(validate_package(package), "ui_schema")

    def test_toggle_over_a_unit_range_parameter_is_accepted(self) -> None:
        document = {
            "schema": "openvst.ui/0",
            "layout": {
                "type": "column",
                "children": [
                    {"type": "toggle", "param": "bypass", "label": "Bypass"},
                ],
            },
        }
        package = self.package_with_ui(document, name="toggle_ok.zip")
        report = validate_package(package)
        self.assertTrue(report.ok, report.to_dict())

    # 12: a package without a UI document stays valid
    def test_package_without_ui_is_still_valid(self) -> None:
        package = make_package(self.tmp, name="no_ui.zip")
        report = validate_package(package)
        self.assertTrue(report.ok, report.to_dict())
        self.assertNotIn(UI_PATH, {entry.path for entry in report.inventory})

    def test_ui_key_is_rejected_inside_other_manifest_objects(self) -> None:
        manifest = manifest_with_ui()
        manifest["module"]["ui"] = "ui.json"
        package = self.package_with_ui(manifest=manifest, name="nested_ui.zip")
        self.assert_error(validate_package(package), "manifest_field_unknown")

    def test_undeclared_json_file_is_refused(self) -> None:
        package = make_package(self.tmp, manifest=base_manifest(),
                               extra=[("extra.json", b"{}")], name="undeclared.zip")
        self.assert_error(validate_package(package), "file_unexpected_location")

    def test_ui_document_is_too_large(self) -> None:
        payload = b"{" + b" " * (64 * 1024) + b"}"
        # Stored, not deflated: a highly compressible payload would trip the
        # archive ratio guard before the UI size guard.
        package = self.package_with_ui(ui_bytes=payload, name="huge_ui.zip",
                                        compression=zipfile.ZIP_STORED)
        report = validate_package(package)
        self.assert_error(report, "ui_too_large")

    def test_validation_stays_read_only_with_a_ui(self) -> None:
        package = self.package_with_ui(name="readonly.zip")
        before = package.read_bytes()
        before_mtime = package.stat().st_mtime_ns
        validate_package(package)
        self.assertEqual(package.read_bytes(), before)
        self.assertEqual(package.stat().st_mtime_ns, before_mtime)
        self.assertEqual(sorted(p.name for p in self.tmp.iterdir()), ["readonly.zip"])

    def test_report_fields_are_unchanged_by_a_ui(self) -> None:
        package = self.package_with_ui(name="fields.zip")
        report = validate_package(package)
        payload = report.to_dict()
        for key in ("ok", "package", "module", "parameters", "resources",
                    "inventory", "errors", "warnings", "limits", "not_validated"):
            self.assertIn(key, payload)
        self.assertEqual(payload["module"]["id"], base_manifest()["module"]["id"])

    def test_ui_error_path_points_at_the_offending_node(self) -> None:
        document = ui_document()
        document["layout"]["children"].append(
            {"type": "knob", "param": "attack", "style": "color: red"})
        package = self.package_with_ui(document, name="error_path.zip")
        report = validate_package(package)
        self.assert_error(report, "ui_unknown_key")
        paths = {issue.path for issue in report.errors}
        self.assertIn("layout.children[2].style", paths)

    # Rules that keep the validator and the native renderer in step: a document
    # that validates must also render.
    def test_group_requires_a_title(self) -> None:
        document = {
            "schema": "openvst.ui/0",
            "layout": {"type": "group", "children": [
                {"type": "label", "text": "inside"}]},
        }
        package = self.package_with_ui(document, name="group_no_title.zip")
        self.assert_error(validate_package(package), "ui_schema")

    def test_empty_column_is_refused(self) -> None:
        document = {"schema": "openvst.ui/0", "layout": {"type": "column", "children": []}}
        package = self.package_with_ui(document, name="empty_column.zip")
        self.assert_error(validate_package(package), "ui_schema")

    def test_empty_group_is_accepted(self) -> None:
        document = {"schema": "openvst.ui/0",
                    "layout": {"type": "group", "title": "Empty", "children": []}}
        package = self.package_with_ui(document, name="empty_group.zip")
        report = validate_package(package)
        self.assertTrue(report.ok, report.to_dict())


if __name__ == "__main__":
    unittest.main()
