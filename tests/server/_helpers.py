"""Shared helpers for the OpenVST catalogue server tests.

Package construction mirrors tests/packages/test_package_validation.py but is
built locally so the server tests do not depend on private helpers of another
suite. All packages built here are FICTIONAL fixtures: no real DSP, never
distributed.

Run the suite with:  python3 -m unittest discover -s tests/server -v
"""

from __future__ import annotations

import hashlib
import http.client
import json
import os
import sys
import tempfile
import threading
import warnings
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
for entry in (str(ROOT), str(ROOT / "server")):
    if entry not in sys.path:
        sys.path.insert(0, entry)

import openvst_catalogue  # noqa: E402
from openvst.package_validation import validate_package  # noqa: E402

ADMIN_TOKEN = "test-admin-token-0123456789"


def dump_json(obj: object) -> bytes:
    return json.dumps(obj, ensure_ascii=False).encode("utf-8")


def base_manifest(module_id: str = "io.openvst.fixtures.catalogue",
                  version: str = "1.0.0", name: str = "Fixture Catalogue",
                  kind: str = "effect", parameters: list | None = None) -> dict:
    return {
        "schema": "openvst.package/0",
        "module": {
            "id": module_id,
            "version": version,
            "name": name,
            "kind": kind,
            "vendor": "OpenVST Fixtures",
            "license": "CC0-1.0",
        },
        "runtime": {
            "engine": "cmajor",
            "patch_format": 1,
            "host_api": "openvst-host/0",
            "patch": "patch/Main.cmajorpatch",
        },
        "parameters": parameters if parameters is not None else [],
        "resources": [],
    }


def base_patch(module_id: str = "io.openvst.fixtures.catalogue",
               version: str = "1.0.0", name: str = "Fixture Catalogue",
               kind: str = "effect") -> dict:
    return {
        "CmajorVersion": 1,
        "ID": module_id,
        "version": version,
        "name": name,
        "manufacturer": "OpenVST Fixtures",
        "category": "fx" if kind == "effect" else "synth",
        "isInstrument": kind == "instrument",
        "source": "Main.cmajor",
    }


CMAJOR_SOURCE = (b"processor Main [[main]]\n{\n"
                 b"    output stream float out;\n    out << 0.0f;\n}\n")


def build_package(directory: Path, *, module_id: str = "io.openvst.fixtures.catalogue",
                  version: str = "1.0.0", name: str = "Fixture Catalogue",
                  kind: str = "effect", parameters: list | None = None,
                  extra: list[tuple[object, bytes]] | None = None,
                  filename: str | None = None,
                  expect_valid: bool = True) -> Path:
    """Build a valid, minimal OpenVST package ZIP and validate it locally."""
    entries = [
        ("openvst.json", dump_json(base_manifest(module_id, version, name, kind,
                                                 parameters))),
        ("patch/Main.cmajorpatch",
         dump_json(base_patch(module_id, version, name, kind))),
        ("patch/Main.cmajor", CMAJOR_SOURCE),
    ]
    entries.extend(extra or [])
    path = directory / (filename or "package-%s.zip" % uuid_hex())
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
            for entry_name, data in entries:
                archive.writestr(entry_name, data)
    report = validate_package(path)
    if expect_valid:
        assert report.ok, [issue.to_dict() for issue in report.errors]
    return path


def uuid_hex() -> str:
    return os.urandom(8).hex()


class ServerHarness:
    """In-process catalogue server on an ephemeral port, with cleanup."""

    def __init__(self, data_dir: str, *, max_package_bytes: int = 512 * 1024 * 1024):
        self.catalogue = openvst_catalogue.Catalogue(
            data_dir, max_package_bytes=max_package_bytes)
        self.server = openvst_catalogue.CatalogueServer(
            ("127.0.0.1", 0), self.catalogue, ADMIN_TOKEN)
        self.port = self.server.server_address[1]
        self.thread = threading.Thread(target=self.server.serve_forever,
                                       daemon=True)
        self.thread.start()

    def stop(self) -> None:
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5)

    def request(self, method: str, path: str, *, body: bytes | None = None,
                token: str | None = ADMIN_TOKEN, headers: dict | None = None,
                idempotency_key: str | None = None) -> tuple[int, dict, bytes]:
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=15)
        try:
            final_headers = dict(headers or {})
            if token is not None:
                final_headers["Authorization"] = "Bearer " + token
            if idempotency_key is not None:
                final_headers["Idempotency-Key"] = idempotency_key
            conn.request(method, path, body=body, headers=final_headers)
            response = conn.getresponse()
            data = response.read()
            return response.status, dict(response.getheaders()), data
        finally:
            conn.close()

    def submit_package(self, package: Path, *, idempotency_key: str | None = None,
                       token: str | None = ADMIN_TOKEN,
                       headers: dict | None = None) -> tuple[int, dict, bytes]:
        data = package.read_bytes()
        extra_headers = {"Content-Type": "application/zip"}
        extra_headers.update(headers or {})
        key = idempotency_key or ("key-" + uuid_hex())
        status, response_headers, body = self.request(
            "POST", "/v1/submissions", body=data, token=token,
            headers=extra_headers, idempotency_key=key)
        return status, response_headers, body

    def approve(self, digest: str) -> tuple[int, dict, bytes]:
        return self.request("POST", "/v1/versions/%s/approve" % digest,
                            body=b'{"reason":"test suite approval"}')


def parse_json(body: bytes) -> dict:
    return json.loads(body.decode("utf-8"))


def package_digest(package: Path) -> str:
    return hashlib.sha256(package.read_bytes()).hexdigest()


class TempDirTestCaseMixin:
    """A temp data dir per test, cleaned up afterwards."""

    def setUp(self) -> None:
        self._tempdir = tempfile.TemporaryDirectory()
        self.data_dir = str(Path(self._tempdir.name) / "data")
        self.pkg_dir = Path(self._tempdir.name) / "pkgs"
        self.pkg_dir.mkdir()

    def tearDown(self) -> None:
        self._tempdir.cleanup()
