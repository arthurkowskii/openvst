"""Tests for the OpenVST catalogue server (v0).

Covers the full v0 loop: submit -> validate -> approve -> list -> download,
Range support, hostile packages, immutability, pagination and persistence.

Run with:  python3 -m unittest discover -s tests/server -v
"""

from __future__ import annotations

import hashlib
import http.client
import json
import os
import sqlite3
import subprocess
import sys
import threading
import unittest
from pathlib import Path

from _helpers import (ADMIN_TOKEN, ServerHarness, TempDirTestCaseMixin,
                      build_package, package_digest, parse_json)

ROOT = Path(__file__).resolve().parents[2]
SERVER_SCRIPT = ROOT / "server" / "openvst_catalogue.py"


class CatalogueTestCase(TempDirTestCaseMixin, unittest.TestCase):

    def make_harness(self, **kwargs) -> ServerHarness:
        harness = ServerHarness(self.data_dir, **kwargs)
        self.addCleanup(harness.stop)
        return harness


class EndToEndTests(CatalogueTestCase):
    """Test 1: submit, hidden, approve, visible, metadata, download."""

    def test_full_loop(self) -> None:
        harness = self.make_harness()
        package = build_package(
            self.pkg_dir, module_id="io.openvst.fixtures.loop",
            parameters=[{"index": 0, "id": "drive", "name": "Drive",
                         "min": 0, "max": 24, "default": 6, "unit": "dB"}])
        digest = package_digest(package)

        status, _, body = harness.submit_package(package,
                                                 idempotency_key="loop-key-1")
        self.assertEqual(status, 201, body)
        response = parse_json(body)
        self.assertEqual(response["digest"], digest)
        self.assertEqual(response["status"], "unapproved")
        submission_id = response["submission_id"]
        self.assertTrue(submission_id)

        # Hidden by default.
        status, _, body = harness.request("GET", "/v1/modules")
        self.assertEqual(status, 200)
        self.assertEqual(parse_json(body)["items"], [])

        # Visible with include_unapproved=1.
        status, _, body = harness.request(
            "GET", "/v1/modules?include_unapproved=1")
        items = parse_json(body)["items"]
        self.assertEqual(len(items), 1)
        self.assertEqual(items[0]["digest"], digest)
        self.assertEqual(items[0]["status"], "unapproved")
        self.assertEqual(items[0]["parameters"][0]["id"], "drive")

        status, _, body = harness.approve(digest)
        self.assertEqual(status, 200, body)
        self.assertEqual(parse_json(body)["version"]["status"], "approved")

        # Now visible by default.
        status, _, body = harness.request("GET", "/v1/modules")
        items = parse_json(body)["items"]
        self.assertEqual(len(items), 1)
        self.assertEqual(items[0]["status"], "approved")
        self.assertEqual(items[0]["name"], "Fixture Catalogue")
        self.assertEqual(items[0]["vendor"], "OpenVST Fixtures")
        self.assertEqual(items[0]["kind"], "effect")
        self.assertEqual(items[0]["version"], "1.0.0")

        # Module detail.
        status, _, body = harness.request(
            "GET", "/v1/modules/io.openvst.fixtures.loop")
        self.assertEqual(status, 200)
        detail = parse_json(body)
        self.assertEqual(detail["module"]["digest"], digest)
        self.assertEqual(len(detail["versions"]), 1)
        self.assertEqual(detail["versions"][0]["status"], "approved")
        self.assertTrue(detail["versions"][0]["report_summary"]["ok"])

        # Version metadata.
        status, _, body = harness.request("GET", "/v1/versions/" + digest)
        self.assertEqual(status, 200)
        version = parse_json(body)["version"]
        self.assertEqual(version["package_url"], "/v1/packages/" + digest)
        self.assertEqual(version["status"], "approved")

        # Submission status endpoint.
        status, _, body = harness.request("GET",
                                          "/v1/submissions/" + submission_id)
        self.assertEqual(status, 200, body)
        self.assertEqual(parse_json(body)["submission"]["status"], "approved")

        # Download: exact bytes, immutable headers.
        status, headers, body = harness.request("GET", "/v1/packages/" + digest)
        self.assertEqual(status, 200)
        self.assertEqual(hashlib.sha256(body).hexdigest(), digest)
        self.assertEqual(len(body), os.path.getsize(package))
        self.assertEqual(headers.get("ETag"), '"%s"' % digest)
        self.assertEqual(headers["Cache-Control"],
                         "public, max-age=31536000, immutable")
        self.assertEqual(headers["Accept-Ranges"], "bytes")


class RangeTests(CatalogueTestCase):
    """Test 2: 206 slices, headers, 416 on bad range."""

    def setUp(self) -> None:
        super().setUp()
        self.harness = self.make_harness()
        self.package = build_package(self.pkg_dir,
                                     module_id="io.openvst.fixtures.range")
        self.digest = package_digest(self.package)
        status, _, body = self.harness.submit_package(
            self.package, idempotency_key="range-key-1")
        self.assertEqual(status, 201, body)
        status, _, _ = self.harness.approve(self.digest)
        self.assertEqual(status, 200)
        self.data = self.package.read_bytes()

    def get(self, headers: dict | None = None):
        return self.harness.request("GET", "/v1/packages/" + self.digest,
                                    headers=headers)

    def test_range_slice(self) -> None:
        status, headers, body = self.get({"Range": "bytes=10-19"})
        self.assertEqual(status, 206)
        self.assertEqual(body, self.data[10:20])
        self.assertEqual(headers["Content-Range"],
                         "bytes 10-19/%d" % len(self.data))
        self.assertEqual(headers["Content-Length"], "10")
        self.assertEqual(headers["ETag"], '"%s"' % self.digest)
        self.assertEqual(headers["Cache-Control"],
                         "public, max-age=31536000, immutable")
        self.assertEqual(headers["Accept-Ranges"], "bytes")

    def test_range_end_clamped(self) -> None:
        status, _, body = self.get({"Range": "bytes=%d-%d"
                                    % (len(self.data) - 5, len(self.data) + 500)})
        self.assertEqual(status, 206)
        self.assertEqual(body, self.data[-5:])

    def test_no_range_is_full_200(self) -> None:
        status, headers, body = self.get()
        self.assertEqual(status, 200)
        self.assertEqual(body, self.data)
        self.assertNotIn("Content-Range", headers)

    def test_if_none_match_304(self) -> None:
        status, _, body = self.get({"If-None-Match": '"%s"' % self.digest})
        self.assertEqual(status, 304)
        self.assertEqual(body, b"")

    def test_invalid_ranges_416(self) -> None:
        for header in ("bytes=99999999-", "bytes=5-2", "bytes=abc",
                       "bytes=0-1,5-6"):
            status, _, body = self.get({"Range": header})
            self.assertEqual(status, 416, header)
            self.assertEqual(parse_json(body)["error"]["code"], "invalid_range")


class AuthTests(CatalogueTestCase):
    """Test 3: unauthorized submit, missing Idempotency-Key."""

    def setUp(self) -> None:
        super().setUp()
        self.harness = self.make_harness()
        self.package = build_package(self.pkg_dir,
                                     module_id="io.openvst.fixtures.auth")

    def test_missing_token_401(self) -> None:
        status, _, body = self.harness.submit_package(
            self.package, token=None, idempotency_key="auth-key-1")
        self.assertEqual(status, 401)
        self.assertEqual(parse_json(body)["error"]["code"], "unauthorized")

    def test_wrong_token_401(self) -> None:
        status, _, body = self.harness.submit_package(
            self.package, token="wrong-token-value", idempotency_key="auth-key-2")
        self.assertEqual(status, 401)

    def test_missing_idempotency_key_400(self) -> None:
        status, _, body = self.harness.request(
            "POST", "/v1/submissions", body=self.package.read_bytes(),
            headers={"Content-Type": "application/zip"})
        self.assertEqual(status, 400)
        self.assertIn("Idempotency-Key", parse_json(body)["error"]["message"])

    def test_short_idempotency_key_400(self) -> None:
        status, _, _ = self.harness.submit_package(
            self.package, idempotency_key="short")
        self.assertEqual(status, 400)

    def test_wrong_content_type_400(self) -> None:
        status, _, body = self.harness.request(
            "POST", "/v1/submissions", body=self.package.read_bytes(),
            headers={"Content-Type": "application/octet-stream"},
            idempotency_key="auth-key-3")
        self.assertEqual(status, 400)

    def test_chunked_encoding_400(self) -> None:
        conn = http.client.HTTPConnection("127.0.0.1", self.harness.port,
                                          timeout=10)
        try:
            conn.putrequest("POST", "/v1/submissions")
            conn.putheader("Authorization", "Bearer " + ADMIN_TOKEN)
            conn.putheader("Content-Type", "application/zip")
            conn.putheader("Idempotency-Key", "auth-key-4")
            conn.putheader("Transfer-Encoding", "chunked")
            conn.endheaders()
            response = conn.getresponse()
            self.assertEqual(response.status, 400)
        finally:
            conn.close()

    def test_moderation_requires_token(self) -> None:
        package = build_package(self.pkg_dir,
                                module_id="io.openvst.fixtures.auth2")
        digest = package_digest(package)
        status, _, _ = self.harness.submit_package(
            package, idempotency_key="auth-key-5")
        self.assertEqual(status, 201)
        status, _, body = self.harness.request(
            "POST", "/v1/versions/%s/approve" % digest,
            body=b'{"reason":"no token"}', token=None)
        self.assertEqual(status, 401)
        status, _, body = self.harness.request(
            "POST", "/v1/reports",
            body=json.dumps({"digest": digest, "category": "other",
                             "message": "x"}).encode(), token=None)
        self.assertEqual(status, 401)


class IdempotencyTests(CatalogueTestCase):
    """Test 4: re-submit same bytes -> 200, one row. Plus (id,version) clash."""

    def test_resubmit_idempotent(self) -> None:
        harness = self.make_harness()
        package = build_package(self.pkg_dir,
                                module_id="io.openvst.fixtures.idem")
        digest = package_digest(package)

        status, _, body = harness.submit_package(package,
                                                 idempotency_key="idem-key-1")
        self.assertEqual(status, 201)
        submission_id = parse_json(body)["submission_id"]

        status, _, body = harness.submit_package(package,
                                                 idempotency_key="idem-key-2")
        self.assertEqual(status, 200, body)
        response = parse_json(body)
        self.assertEqual(response["submission_id"], submission_id)
        self.assertEqual(response["status"], "unapproved")

        status, _, body = harness.submit_package(package,
                                                 idempotency_key="idem-key-1")
        self.assertEqual(status, 200)

        conn = sqlite3.connect(os.path.join(self.data_dir,
                                            "catalogue.sqlite3"))
        try:
            versions = conn.execute("SELECT COUNT(*) FROM versions").fetchone()[0]
            submissions = conn.execute(
                "SELECT COUNT(*) FROM submissions").fetchone()[0]
        finally:
            conn.close()
        self.assertEqual(versions, 1)
        self.assertEqual(submissions, 1)

    def test_same_module_version_different_bytes_409(self) -> None:
        harness = self.make_harness()
        package_a = build_package(self.pkg_dir,
                                  module_id="io.openvst.fixtures.immutable",
                                  version="2.0.0", filename="a.zip")
        package_b = build_package(
            self.pkg_dir, module_id="io.openvst.fixtures.immutable",
            version="2.0.0", name="Fixture Catalogue B", filename="b.zip")
        status, _, body = harness.submit_package(package_a,
                                                 idempotency_key="imm-key-1")
        self.assertEqual(status, 201, body)
        status, _, body = harness.submit_package(package_b,
                                                 idempotency_key="imm-key-2")
        self.assertEqual(status, 409, body)
        self.assertEqual(parse_json(body)["error"]["code"], "conflict")

    def test_reused_key_different_package_409(self) -> None:
        harness = self.make_harness()
        package_a = build_package(self.pkg_dir,
                                  module_id="io.openvst.fixtures.keyone",
                                  filename="ka.zip")
        package_b = build_package(self.pkg_dir,
                                  module_id="io.openvst.fixtures.keytwo",
                                  filename="kb.zip")
        status, _, _ = harness.submit_package(package_a,
                                              idempotency_key="shared-key-1")
        self.assertEqual(status, 201)
        status, _, body = harness.submit_package(package_b,
                                                 idempotency_key="shared-key-1")
        self.assertEqual(status, 409, body)


class HostilePackageTests(CatalogueTestCase):
    """Test 5: traversal, absolute path, backslash -> 422, nothing published."""

    def assert_rejected_and_clean(self, extra_entries: list[tuple[str, bytes]]) -> None:
        harness = self.make_harness()
        package = build_package(self.pkg_dir, extra=extra_entries,
                                filename="hostile.zip", expect_valid=False)
        digest = package_digest(package)
        status, _, body = harness.submit_package(
            package, idempotency_key="hostile-" + digest[:8])
        self.assertEqual(status, 422, body)
        error = parse_json(body)["error"]
        self.assertEqual(error["code"], "package_rejected")
        report = error["details"]["report"]
        self.assertFalse(report["ok"])
        self.assertGreater(report["error_count"], 0)
        # Nothing published: no version row, no package file, no tmp leftovers.
        conn = sqlite3.connect(os.path.join(self.data_dir,
                                            "catalogue.sqlite3"))
        try:
            self.assertEqual(conn.execute(
                "SELECT COUNT(*) FROM versions").fetchone()[0], 0)
        finally:
            conn.close()
        self.assertFalse(
            (Path(self.data_dir) / "packages" / digest[0:2] / digest[2:4]
             / (digest + ".zip")).exists())
        tmp_dir = Path(self.data_dir) / "tmp"
        self.assertEqual(list(tmp_dir.iterdir()), [])
        # The rejected digest itself must not be downloadable.
        status, _, _ = harness.request("GET", "/v1/versions/" + digest)
        self.assertEqual(status, 404)

    def test_path_traversal_entry(self) -> None:
        self.assert_rejected_and_clean([("../escape.txt", b"x")])

    def test_absolute_path_entry(self) -> None:
        self.assert_rejected_and_clean([("/etc/passwd.txt", b"x")])

    def test_backslash_entry(self) -> None:
        self.assert_rejected_and_clean([("patch\\evil.cmajor", b"x")])

    def test_garbage_bytes_422(self) -> None:
        harness = self.make_harness()
        status, _, body = harness.request(
            "POST", "/v1/submissions", body=b"this is not a zip at all",
            headers={"Content-Type": "application/zip"},
            idempotency_key="garbage-key-1")
        self.assertEqual(status, 422, body)
        self.assertEqual(parse_json(body)["error"]["code"], "package_rejected")


class DigestHeaderTests(CatalogueTestCase):
    """Test 6: X-Content-SHA256 mismatch -> 400, match -> accepted."""

    def test_mismatch_400(self) -> None:
        harness = self.make_harness()
        package = build_package(self.pkg_dir,
                                module_id="io.openvst.fixtures.mismatch")
        real = package_digest(package)
        wrong = "0" * 64
        status, _, body = harness.submit_package(
            package, idempotency_key="digest-key-1",
            headers={"X-Content-SHA256": wrong})
        self.assertEqual(status, 400, body)
        self.assertEqual(parse_json(body)["error"]["code"], "digest_mismatch")
        status, _, _ = harness.request("GET", "/v1/versions/" + real)
        self.assertEqual(status, 404)

    def test_match_accepted(self) -> None:
        harness = self.make_harness()
        package = build_package(self.pkg_dir,
                                module_id="io.openvst.fixtures.match")
        real = package_digest(package)
        status, _, body = harness.submit_package(
            package, idempotency_key="digest-key-2",
            headers={"X-Content-SHA256": real})
        self.assertEqual(status, 201, body)

    def test_malformed_header_400(self) -> None:
        harness = self.make_harness()
        package = build_package(self.pkg_dir,
                                module_id="io.openvst.fixtures.badsha")
        status, _, _ = harness.submit_package(
            package, idempotency_key="digest-key-3",
            headers={"X-Content-SHA256": "NOT-A-DIGEST"})
        self.assertEqual(status, 400)


class RejectTests(CatalogueTestCase):
    """Test 7: rejected version never listed, not downloadable."""

    def test_reject_then_hidden(self) -> None:
        harness = self.make_harness()
        package = build_package(self.pkg_dir,
                                module_id="io.openvst.fixtures.reject")
        digest = package_digest(package)
        status, _, _ = harness.submit_package(package,
                                              idempotency_key="reject-key-1")
        self.assertEqual(status, 201)

        status, _, body = harness.request(
            "POST", "/v1/versions/%s/reject" % digest,
            body=b'{"reason":"hostile DSP"}')
        self.assertEqual(status, 200)
        self.assertEqual(parse_json(body)["version"]["status"], "rejected")

        for query in ("", "?include_unapproved=1"):
            status, _, body = harness.request("GET", "/v1/modules" + query)
            self.assertEqual(status, 200)
            self.assertEqual(parse_json(body)["items"], [], query)

        status, _, _ = harness.request("GET", "/v1/packages/" + digest)
        self.assertEqual(status, 403)

        # Idempotent re-reject is fine; approve after reject is a conflict.
        status, _, body = harness.request(
            "POST", "/v1/versions/%s/reject" % digest,
            body=b'{"reason":"again"}')
        self.assertEqual(status, 200)
        status, _, body = harness.request(
            "POST", "/v1/versions/%s/approve" % digest,
            body=b'{"reason":"too late"}')
        self.assertEqual(status, 409)

    def test_reapprove_idempotent(self) -> None:
        harness = self.make_harness()
        package = build_package(self.pkg_dir,
                                module_id="io.openvst.fixtures.reapprove")
        digest = package_digest(package)
        harness.submit_package(package, idempotency_key="reap-key-1")
        harness.approve(digest)
        status, _, body = harness.approve(digest)
        self.assertEqual(status, 200)
        self.assertTrue(parse_json(body)["idempotent"])


class ModuleDetailTests(CatalogueTestCase):
    """Test 8: module detail returns all versions with statuses."""

    def test_all_versions_with_statuses(self) -> None:
        harness = self.make_harness()
        module_id = "io.openvst.fixtures.multiver"
        package_v1 = build_package(self.pkg_dir, module_id=module_id,
                                   version="1.0.0", filename="v1.zip")
        package_v2 = build_package(self.pkg_dir, module_id=module_id,
                                   version="2.0.0", filename="v2.zip")
        package_other = build_package(self.pkg_dir,
                                      module_id="io.openvst.fixtures.other",
                                      filename="other.zip")
        digest_v1 = package_digest(package_v1)
        digest_v2 = package_digest(package_v2)
        digest_other = package_digest(package_other)
        harness.submit_package(package_v1, idempotency_key="multi-key-1")
        harness.submit_package(package_v2, idempotency_key="multi-key-2")
        harness.submit_package(package_other, idempotency_key="multi-key-3")
        harness.approve(digest_v2)
        harness.request("POST", "/v1/versions/%s/reject" % digest_v1,
                        body=b'{"reason":"broken"}')

        status, _, body = harness.request("GET", "/v1/modules/" + module_id)
        self.assertEqual(status, 200)
        detail = parse_json(body)
        statuses = {v["digest"]: v["status"] for v in detail["versions"]}
        self.assertEqual(statuses,
                         {digest_v1: "rejected", digest_v2: "approved"})
        # Latest approved version first.
        self.assertEqual(detail["versions"][0]["version"], "2.0.0")
        # The default listing shows only the approved latest; the unapproved
        # module appears with the opt-in flag.
        status, _, body = harness.request("GET", "/v1/modules")
        items = parse_json(body)["items"]
        by_id = {item["id"]: item for item in items}
        self.assertEqual(by_id[module_id]["digest"], digest_v2)
        self.assertNotIn("io.openvst.fixtures.other", by_id)
        status, _, body = harness.request(
            "GET", "/v1/modules?include_unapproved=1")
        items = parse_json(body)["items"]
        by_id = {item["id"]: item for item in items}
        self.assertEqual(by_id["io.openvst.fixtures.other"]["digest"],
                         digest_other)
        self.assertEqual(by_id["io.openvst.fixtures.other"]["status"],
                         "unapproved")

    def test_unknown_module_404(self) -> None:
        harness = self.make_harness()
        status, _, _ = harness.request("GET", "/v1/modules/io.openvst.nope")
        self.assertEqual(status, 404)

    def test_malformed_module_id_400(self) -> None:
        harness = self.make_harness()
        status, _, _ = harness.request("GET", "/v1/modules/Not_A_Valid_Id")
        self.assertEqual(status, 400)


class ConcurrentDownloadTests(CatalogueTestCase):
    """Test 9: 8 threads download identical bytes."""

    def test_eight_threads_identical(self) -> None:
        harness = self.make_harness()
        package = build_package(self.pkg_dir,
                                module_id="io.openvst.fixtures.concurrent")
        digest = package_digest(package)
        harness.submit_package(package, idempotency_key="conc-key-1")
        harness.approve(digest)
        expected = package.read_bytes()
        results: list[bytes] = []
        errors: list[str] = []

        def worker() -> None:
            try:
                status, _, body = harness.request("GET",
                                                  "/v1/packages/" + digest)
                if status != 200:
                    errors.append("status %d" % status)
                results.append(body)
            except Exception as exc:  # pragma: no cover - surfaced below
                errors.append(repr(exc))

        threads = [threading.Thread(target=worker) for _ in range(8)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join(timeout=30)
        self.assertEqual(errors, [])
        self.assertEqual(len(results), 8)
        for body in results:
            self.assertEqual(body, expected)
            self.assertEqual(len(body), len(expected))


class OversizeTests(CatalogueTestCase):
    """Test 10: oversized Content-Length -> 413 without reading the body."""

    def test_oversized_413(self) -> None:
        harness = self.make_harness(max_package_bytes=1024)
        conn = http.client.HTTPConnection("127.0.0.1", harness.port, timeout=10)
        try:
            conn.putrequest("POST", "/v1/submissions")
            conn.putheader("Authorization", "Bearer " + ADMIN_TOKEN)
            conn.putheader("Content-Type", "application/zip")
            conn.putheader("Idempotency-Key", "oversize-key-1")
            conn.putheader("Content-Length", "4096")
            conn.endheaders()
            response = conn.getresponse()
            body = response.read()
            self.assertEqual(response.status, 413)
            self.assertEqual(parse_json(body)["error"]["code"],
                             "payload_too_large")
        finally:
            conn.close()
        # Nothing staged.
        tmp_dir = Path(self.data_dir) / "tmp"
        self.assertEqual(list(tmp_dir.iterdir()), [])


class DigestErrorTests(CatalogueTestCase):
    """Test 11: unknown digest -> 404, malformed digest -> 400."""

    def setUp(self) -> None:
        super().setUp()
        self.harness = self.make_harness()

    def test_package_endpoints(self) -> None:
        unknown = "b" * 64
        status, _, _ = self.harness.request("GET", "/v1/packages/" + unknown)
        self.assertEqual(status, 404)
        for bad in ("ABCDEF", "b" * 63, "b" * 65, "z" * 64):
            status, _, _ = self.harness.request("GET", "/v1/packages/" + bad)
            self.assertEqual(status, 400, repr(bad))

    def test_version_endpoints(self) -> None:
        unknown = "c" * 64
        status, _, _ = self.harness.request("GET", "/v1/versions/" + unknown)
        self.assertEqual(status, 404)
        status, _, _ = self.harness.request(
            "POST", "/v1/versions/%s/approve" % ("b" * 64),
            body=b'{"reason":"x"}')
        self.assertEqual(status, 404)
        status, _, _ = self.harness.request("GET", "/v1/versions/short")
        self.assertEqual(status, 400)

    def test_moderation_malformed(self) -> None:
        status, _, _ = self.harness.request(
            "POST", "/v1/versions/zzz/approve", body=b'{"reason":"x"}')
        self.assertEqual(status, 400)

    def test_report_on_unknown_digest_404(self) -> None:
        status, _, body = self.harness.request(
            "POST", "/v1/reports",
            body=json.dumps({"digest": "d" * 64, "category": "security",
                             "message": "suspicious"}).encode())
        self.assertEqual(status, 404)

    def test_report_validation(self) -> None:
        harness = self.make_harness()
        package = build_package(self.pkg_dir,
                                module_id="io.openvst.fixtures.report")
        digest = package_digest(package)
        harness.submit_package(package, idempotency_key="report-key-1")
        bad_bodies = [
            {"digest": "nope", "category": "security", "message": "x"},
            {"digest": digest, "category": "wat", "message": "x"},
            {"digest": digest, "category": "security", "message": ""},
            {"digest": digest, "category": "security"},
        ]
        for payload in bad_bodies:
            status, _, _ = harness.request(
                "POST", "/v1/reports", body=json.dumps(payload).encode())
            self.assertEqual(status, 400, payload)
        status, _, body = harness.request(
            "POST", "/v1/reports",
            body=json.dumps({"digest": digest, "category": "security",
                             "message": "suspicious behaviour"}).encode())
        self.assertEqual(status, 201, body)
        self.assertTrue(parse_json(body)["report_id"].startswith("rep_"))


class PersistenceTests(CatalogueTestCase):
    """Test 12: restart on the same data dir keeps state."""

    def test_restart_persists(self) -> None:
        first = self.make_harness()
        package = build_package(self.pkg_dir,
                                module_id="io.openvst.fixtures.persist")
        digest = package_digest(package)
        status, _, _ = first.submit_package(package,
                                            idempotency_key="persist-key-1")
        self.assertEqual(status, 201)
        status, _, _ = first.approve(digest)
        self.assertEqual(status, 200)
        first.stop()

        second = self.make_harness()
        status, _, body = second.request("GET", "/v1/modules")
        items = parse_json(body)["items"]
        self.assertEqual([item["digest"] for item in items], [digest])
        status, _, body = second.request("GET", "/v1/packages/" + digest)
        self.assertEqual(status, 200)
        self.assertEqual(hashlib.sha256(body).hexdigest(), digest)
        second.stop()


class SeedTests(CatalogueTestCase):
    """Test 13: --seed validates and inserts an approved package."""

    def test_seed_inserts_approved(self) -> None:
        package = build_package(self.pkg_dir,
                                module_id="io.openvst.fixtures.seeded")
        digest = package_digest(package)
        result = subprocess.run(
            [sys.executable, str(SERVER_SCRIPT), "--data", self.data_dir,
             "--seed", str(package), "--admin-token", ADMIN_TOKEN],
            capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(digest, result.stdout)

        harness = self.make_harness()
        status, _, body = harness.request("GET", "/v1/modules")
        items = parse_json(body)["items"]
        self.assertEqual([item["digest"] for item in items], [digest])
        self.assertEqual(items[0]["status"], "approved")
        status, _, _ = harness.request("GET", "/v1/packages/" + digest)
        self.assertEqual(status, 200)

    def test_seed_rejects_hostile_package(self) -> None:
        package = build_package(self.pkg_dir, extra=[("../evil.txt", b"x")],
                                filename="hostile-seed.zip", expect_valid=False)
        result = subprocess.run(
            [sys.executable, str(SERVER_SCRIPT), "--data", self.data_dir,
             "--seed", str(package), "--admin-token", ADMIN_TOKEN],
            capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 2, result.stderr)
        harness = self.make_harness()
        status, _, body = harness.request("GET", "/v1/modules")
        self.assertEqual(parse_json(body)["items"], [])

    def test_missing_token_refuses_to_start(self) -> None:
        env = dict(os.environ)
        env.pop("OPENVST_ADMIN_TOKEN", None)
        result = subprocess.run(
            [sys.executable, str(SERVER_SCRIPT), "--data", self.data_dir],
            capture_output=True, text=True, timeout=60, env=env)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("admin token", result.stderr.lower())


class PaginationTests(CatalogueTestCase):
    """Test 14: q, kind, limit and cursor pagination without duplicates."""

    def _prepare(self, harness: ServerHarness) -> list[str]:
        names = ["Alpha Boost", "Beta Cloud", "Gamma Drive", "Delta Echo",
                 "Epsilon Flange"]
        keys = ["pag-key-aaaa", "pag-key-bbbb", "pag-key-cccc",
                "pag-key-dddd", "pag-key-eeee"]
        digests = []
        for index, (name, key) in enumerate(zip(names, keys)):
            module_id = "io.openvst.fixtures.page%d" % index
            kind = "effect" if index % 2 == 0 else "instrument"
            package = build_package(self.pkg_dir, module_id=module_id,
                                    name=name, kind=kind,
                                    filename="page%d.zip" % index)
            status, _, body = harness.submit_package(package,
                                                     idempotency_key=key)
            self.assertEqual(status, 201, body)
            digest = package_digest(package)
            status, _, body = harness.approve(digest)
            self.assertEqual(status, 200, body)
            digests.append(digest)
        return digests

    def test_two_pages_no_duplicates(self) -> None:
        harness = self.make_harness()
        digests = self._prepare(harness)
        seen: list[str] = []
        cursor = ""
        pages = 0
        while True:
            query = "/v1/modules?limit=2" + ("&cursor=" + cursor if cursor else "")
            status, _, body = harness.request("GET", query)
            self.assertEqual(status, 200, body)
            response = parse_json(body)
            seen.extend(item["digest"] for item in response["items"])
            pages += 1
            if not response["next_cursor"]:
                break
            cursor = response["next_cursor"]
            self.assertLess(pages, 10)
        self.assertEqual(sorted(seen), sorted(digests))
        self.assertEqual(len(seen), len(set(seen)))
        self.assertEqual(pages, 3)

    def test_sorted_by_name(self) -> None:
        harness = self.make_harness()
        self._prepare(harness)
        status, _, body = harness.request("GET", "/v1/modules?limit=100")
        names = [item["name"] for item in parse_json(body)["items"]]
        self.assertEqual(names, sorted(names))

    def test_query_filter(self) -> None:
        harness = self.make_harness()
        self._prepare(harness)
        status, _, body = harness.request("GET", "/v1/modules?q=drive")
        items = parse_json(body)["items"]
        self.assertEqual([item["name"] for item in items], ["Gamma Drive"])
        # Case-insensitive, matches vendor and id too.
        status, _, body = harness.request("GET", "/v1/modules?q=FIXTURES")
        self.assertEqual(len(parse_json(body)["items"]), 5)

    def test_kind_filter(self) -> None:
        harness = self.make_harness()
        self._prepare(harness)
        status, _, body = harness.request("GET", "/v1/modules?kind=instrument")
        items = parse_json(body)["items"]
        self.assertEqual(len(items), 2)
        self.assertTrue(all(item["kind"] == "instrument" for item in items))

    def test_invalid_parameters(self) -> None:
        harness = self.make_harness()
        for query in ("limit=0", "limit=101", "limit=abc", "kind=synth",
                      "include_unapproved=maybe", "status=rejected"):
            status, _, _ = harness.request("GET", "/v1/modules?" + query)
            self.assertEqual(status, 400, query)
        status, _, _ = harness.request(
            "GET", "/v1/modules?cursor=!!!not-base64!!!")
        self.assertEqual(status, 400)

    def test_q_too_long_400(self) -> None:
        harness = self.make_harness()
        status, _, _ = harness.request("GET", "/v1/modules?q=" + "x" * 65)
        self.assertEqual(status, 400)


if __name__ == "__main__":
    unittest.main(verbosity=2)
