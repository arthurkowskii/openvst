#!/usr/bin/env python3
"""OpenVST catalogue server (v0).

A small, dependency-free HTTP server that lists OpenVST modules, serves
immutable content-addressed packages with Range support, accepts package
submissions validated with openvst.package_validation, and records
moderation decisions and user reports.

Python 3.11+ standard library only. Single process, threading HTTP server,
SQLite in WAL mode. The server never trusts the client: every size is
bounded, every digest is recomputed from the received bytes, and every
package is validated before it can be listed or downloaded.

Run:
    OPENVST_ADMIN_TOKEN=... python3 server/openvst_catalogue.py --data /var/lib/openvst

All HTTP strings and log output are ASCII.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import hmac
import json
import os
import re
import shutil
import sqlite3
import sys
import threading
import time
import urllib.parse
import uuid
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# Make "openvst" importable when this file is run as a script from the repo.
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from openvst.package_validation import Limits, validate_package  # noqa: E402

CATALOGUE_SCHEMA = "openvst.catalogue/0"

DIGEST_RE = re.compile(r"^[0-9a-f]{64}$")
# Same lowercase reverse-DNS shape the package validator enforces.
MODULE_ID_RE = re.compile(
    r"^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?"
    r"(?:\.[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?)+$"
)
KINDS = ("effect", "instrument")
STATUSES = ("unapproved", "approved", "rejected", "revoked")
PUBLIC_STATUSES = ("approved", "unapproved")
REPORT_CATEGORIES = ("security", "rights", "content", "other")

MAX_PACKAGE_BYTES = 512 * 1024 * 1024
MAX_Q_CHARS = 64
MAX_REASON_CHARS = 1000
MAX_REPORT_MESSAGE_CHARS = 2000
MAX_JSON_BODY_BYTES = 64 * 1024
UPLOAD_CHUNK = 64 * 1024
FILE_CHUNK = 64 * 1024
RATE_LIMIT_EVENTS = 120
RATE_LIMIT_WINDOW = 60.0

_IDEMPOTENCY_RE = re.compile(r"^[ -~]{8,128}$")  # printable ASCII
_SUBMISSION_ID_RE = re.compile(r"^[A-Za-z0-9_-]{3,64}$")
_RANGE_RE = re.compile(r"^bytes=(\d+)-(\d*)$")
_TOKEN_RE = re.compile(r"^[!-~]{8,256}$")

_SQL_SCHEMA = """
CREATE TABLE IF NOT EXISTS versions (
    digest TEXT PRIMARY KEY,
    module_id TEXT NOT NULL,
    version TEXT NOT NULL,
    name TEXT NOT NULL,
    vendor TEXT NOT NULL,
    kind TEXT NOT NULL,
    size INTEGER NOT NULL,
    status TEXT NOT NULL,
    parameters_json TEXT NOT NULL,
    report_json TEXT NOT NULL,
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL
);
CREATE UNIQUE INDEX IF NOT EXISTS versions_module_version
    ON versions(module_id, version);
CREATE INDEX IF NOT EXISTS versions_status ON versions(status);

CREATE TABLE IF NOT EXISTS submissions (
    id TEXT PRIMARY KEY,
    digest TEXT NOT NULL UNIQUE,
    idempotency_key TEXT NOT NULL UNIQUE,
    status TEXT NOT NULL,
    report_json TEXT NOT NULL,
    created_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS audit (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    digest TEXT NOT NULL,
    action TEXT NOT NULL,
    actor TEXT NOT NULL,
    reason TEXT,
    created_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS reports (
    id TEXT PRIMARY KEY,
    digest TEXT NOT NULL,
    category TEXT NOT NULL,
    message TEXT NOT NULL,
    created_at TEXT NOT NULL
);
"""


class ApiError(Exception):
    """An error that maps directly to one HTTP error response."""

    def __init__(self, status: int, code: str, message: str,
                 details: dict | None = None):
        super().__init__(message)
        self.status = status
        self.code = code
        self.message = message
        self.details = details or {}


def now_iso() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def ascii_text(value: str) -> str:
    """Sanitise a value for the ASCII-only access log."""
    return value.encode("ascii", "backslashreplace").decode("ascii")


def sanitize_report(report: dict) -> dict:
    """Remove absolute filesystem paths from a validation report.

    Reports are stored in SQLite and returned over HTTP; invariant 3 forbids
    embedding absolute paths in persisted state. The validator itself records
    the staging path it was given, which is meaningless to clients.
    """
    package = report.get("package")
    if isinstance(package, dict):
        package["path"] = None
    return report


def bounded_report(report: dict, max_errors: int = 50) -> dict:
    """A bounded, error-only view of a validation report for HTTP responses."""
    errors = report.get("errors") or []
    trimmed = []
    for issue in errors[:max_errors]:
        trimmed.append({
            "code": str(issue.get("code", "unknown"))[:64],
            "message": str(issue.get("message", ""))[:500],
            "path": (str(issue["path"])[:240] if issue.get("path") else None),
        })
    return {
        "ok": bool(report.get("ok")),
        "error_count": len(errors),
        "truncated": len(errors) > max_errors,
        "errors": trimmed,
    }


def report_summary(report_json: str) -> dict:
    try:
        report = json.loads(report_json)
    except (ValueError, TypeError):
        return {"ok": False, "error_count": 0}
    if not isinstance(report, dict):
        return {"ok": False, "error_count": 0}
    errors = report.get("errors") or []
    warnings = report.get("warnings") or []
    return {
        "ok": bool(report.get("ok")),
        "error_count": len(errors),
        "warning_count": len(warnings),
        "first_errors": [
            {
                "code": str(issue.get("code", "unknown"))[:64],
                "message": str(issue.get("message", ""))[:500],
                "path": (str(issue["path"])[:240] if issue.get("path") else None),
            }
            for issue in errors[:5]
        ],
    }


def version_sort_key(version: str) -> tuple:
    """SemVer-aware sort key; releases rank above pre-releases."""
    core = version.partition("+")[0].partition("-")[0]
    try:
        major, minor, patch = (int(part) for part in core.split("."))
    except ValueError:
        return (-1, -1, -1, 0, ())
    prerelease = version.partition("+")[0].partition("-")[2]
    if not prerelease:
        return (major, minor, patch, 1, ())
    parts = []
    for part in prerelease.split("."):
        if part.isdigit():
            parts.append((0, int(part), ""))
        else:
            parts.append((1, 0, part))
    return (major, minor, patch, 0, tuple(parts))


class SlidingWindowLimiter:
    """Per-key sliding-window rate limiter, in memory, thread-safe."""

    def __init__(self, limit: int, window_seconds: float):
        self.limit = limit
        self.window = window_seconds
        self.lock = threading.Lock()
        self.events: dict[str, deque] = {}

    def allow(self, key: str) -> bool:
        now = time.monotonic()
        with self.lock:
            bucket = self.events.setdefault(key, deque())
            while bucket and now - bucket[0] > self.window:
                bucket.popleft()
            if len(bucket) >= self.limit:
                return False
            bucket.append(now)
            return True


class Catalogue:
    """SQLite-backed catalogue state plus the content-addressed package store."""

    def __init__(self, data_dir: str, max_package_bytes: int = MAX_PACKAGE_BYTES):
        self.data_dir = os.path.abspath(data_dir)
        self.max_package_bytes = max_package_bytes
        self.db_path = os.path.join(self.data_dir, "catalogue.sqlite3")
        self.packages_dir = os.path.join(self.data_dir, "packages")
        self.tmp_dir = os.path.join(self.data_dir, "tmp")
        os.makedirs(self.data_dir, exist_ok=True)
        os.makedirs(self.packages_dir, exist_ok=True)
        os.makedirs(self.tmp_dir, exist_ok=True)
        self._init_db()
        self._clean_tmp()

    # -- filesystem -----------------------------------------------------

    def package_path(self, digest: str) -> str:
        return os.path.join(
            self.packages_dir, digest[0:2], digest[2:4], digest + ".zip")

    def new_tmp_path(self) -> str:
        return os.path.join(self.tmp_dir, "upload-%s.tmp" % uuid.uuid4().hex)

    def discard_tmp(self, path: str) -> None:
        try:
            os.unlink(path)
        except OSError:
            pass

    def _clean_tmp(self) -> None:
        """Staging is transient: remove whatever survived a previous run."""
        removed = 0
        for name in os.listdir(self.tmp_dir):
            path = os.path.join(self.tmp_dir, name)
            try:
                if os.path.isdir(path) and not os.path.islink(path):
                    shutil.rmtree(path)
                else:
                    os.unlink(path)
                removed += 1
            except OSError:
                pass
        if removed:
            print("openvst-catalogue: removed %d stale staging file(s)"
                  % removed, flush=True)

    def publish(self, tmp_path: str, digest: str) -> str:
        """Move a validated staged package into the immutable store.

        os.replace is atomic on the same filesystem; tmp/ and packages/ share
        the data directory, so a partially stored package cannot appear.
        """
        final = self.package_path(digest)
        os.makedirs(os.path.dirname(final), exist_ok=True)
        os.replace(tmp_path, final)
        return final

    # -- database -------------------------------------------------------

    def connect(self) -> sqlite3.Connection:
        conn = sqlite3.connect(self.db_path, timeout=5.0)
        conn.row_factory = sqlite3.Row
        conn.execute("PRAGMA journal_mode=WAL")
        conn.execute("PRAGMA foreign_keys=ON")
        conn.execute("PRAGMA busy_timeout=5000")
        conn.execute("PRAGMA synchronous=NORMAL")
        return conn

    def _init_db(self) -> None:
        conn = self.connect()
        try:
            conn.executescript(_SQL_SCHEMA)
            conn.commit()
        finally:
            conn.close()

    # -- versions -------------------------------------------------------

    def get_version(self, digest: str) -> sqlite3.Row | None:
        conn = self.connect()
        try:
            return conn.execute(
                "SELECT * FROM versions WHERE digest = ?", (digest,)
            ).fetchone()
        finally:
            conn.close()

    def get_submission(self, submission_id: str) -> sqlite3.Row | None:
        conn = self.connect()
        try:
            return conn.execute(
                "SELECT * FROM submissions WHERE id = ?", (submission_id,)
            ).fetchone()
        finally:
            conn.close()

    def find_submission_by_digest(self, digest: str) -> sqlite3.Row | None:
        conn = self.connect()
        try:
            return conn.execute(
                "SELECT * FROM submissions WHERE digest = ?", (digest,)
            ).fetchone()
        finally:
            conn.close()

    def insert_version(self, digest: str, module: dict, size: int,
                       status: str, parameters: list, report: dict,
                       actor: str) -> sqlite3.Row:
        """Insert an approved version row (used by the seed path)."""
        conn = self.connect()
        try:
            stamp = now_iso()
            conn.execute("BEGIN IMMEDIATE")
            conn.execute(
                "INSERT INTO versions (digest, module_id, version, name,"
                " vendor, kind, size, status, parameters_json, report_json,"
                " created_at, updated_at) VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
                (digest, module["id"], module["version"], module["name"],
                 module["vendor"], module["kind"], size, status,
                 json.dumps(parameters, ensure_ascii=False,
                            separators=(",", ":")),
                 json.dumps(report, ensure_ascii=False,
                            separators=(",", ":")),
                 stamp, stamp))
            conn.execute(
                "INSERT INTO submissions (id, digest, idempotency_key, status,"
                " report_json, created_at) VALUES (?,?,?,?,?,?)",
                ("sub_" + uuid.uuid4().hex, digest, "seed-" + digest[:16],
                 status, json.dumps(report, ensure_ascii=False,
                                    separators=(",", ":")), stamp))
            conn.execute(
                "INSERT INTO audit (digest, action, actor, reason, created_at)"
                " VALUES (?,?,?,?,?)", (digest, "seed", actor, None, stamp))
            conn.commit()
            return self.get_version(digest)
        finally:
            conn.close()

    def transition(self, digest: str, action: str, reason: str | None,
                   actor: str) -> tuple[sqlite3.Row, bool]:
        """Approve or reject a version. Returns (row, was_idempotent)."""
        target = "approved" if action == "approve" else "rejected"
        conn = self.connect()
        try:
            conn.execute("BEGIN IMMEDIATE")
            row = conn.execute(
                "SELECT digest, module_id, version, status FROM versions"
                " WHERE digest = ?", (digest,)).fetchone()
            if row is None:
                conn.commit()
                raise ApiError(404, "not_found", "unknown digest")
            if row["status"] == target:
                conn.execute(
                    "INSERT INTO audit (digest, action, actor, reason,"
                    " created_at) VALUES (?,?,?,?,?)",
                    (digest, action, actor, reason, now_iso()))
                conn.commit()
                return self.get_version(digest), True
            if row["status"] != "unapproved":
                conn.commit()
                raise ApiError(
                    409, "conflict",
                    "version status is %r; only unapproved versions can be %s"
                    % (row["status"], target))
            stamp = now_iso()
            conn.execute(
                "UPDATE versions SET status = ?, updated_at = ?"
                " WHERE digest = ?", (target, stamp, digest))
            conn.execute(
                "UPDATE submissions SET status = ? WHERE digest = ?",
                (target, digest))
            conn.execute(
                "INSERT INTO audit (digest, action, actor, reason, created_at)"
                " VALUES (?,?,?,?,?)", (digest, action, actor, reason, stamp))
            conn.commit()
            return self.get_version(digest), False
        finally:
            conn.close()

    def insert_report(self, digest: str, category: str,
                      message: str) -> str:
        report_id = "rep_" + uuid.uuid4().hex
        conn = self.connect()
        try:
            conn.execute("BEGIN IMMEDIATE")
            conn.execute(
                "INSERT INTO reports (id, digest, category, message,"
                " created_at) VALUES (?,?,?,?,?)",
                (report_id, digest, category, message, now_iso()))
            conn.commit()
        finally:
            conn.close()
        return report_id

    def list_latest_items(self, statuses: tuple[str, ...],
                          query: str | None, kind: str | None,
                          status_filter: str | None) -> list[dict]:
        """Group versions by module id and pick the listable version.

        Default mode considers approved versions only. With unapproved
        included, an approved version still wins over a newer unapproved one
        so the public listing never regresses to a moderated version.
        """
        placeholders = ",".join("?" for _ in statuses)
        sql = ("SELECT digest, module_id, version, name, vendor, kind, size,"
               " status, parameters_json, created_at, updated_at, rowid"
               " FROM versions WHERE status IN (%s)" % placeholders)
        args = list(statuses)
        if query is not None:
            sql += " AND (LOWER(name) LIKE ? OR LOWER(module_id) LIKE ?" \
                   " OR LOWER(vendor) LIKE ?)"
            needle = "%" + query.lower().replace("%", "").replace("_", "") + "%"
            args.extend([needle, needle, needle])
        if kind is not None:
            sql += " AND kind = ?"
            args.append(kind)

        conn = self.connect()
        try:
            rows = conn.execute(sql, args).fetchall()
        finally:
            conn.close()

        best: dict[str, dict] = {}
        for row in rows:
            if status_filter is not None and row["status"] != status_filter:
                continue
            key = (version_sort_key(row["version"]), row["created_at"],
                   row["rowid"])
            current = best.get(row["module_id"])
            current_key = current["_sort"] if current else None
            if current is None or key > current_key:
                item = {
                    "id": row["module_id"],
                    "name": row["name"],
                    "vendor": row["vendor"],
                    "kind": row["kind"],
                    "version": row["version"],
                    "digest": row["digest"],
                    "parameters": json.loads(row["parameters_json"]),
                    "size": row["size"],
                    "status": row["status"],
                    "updated_at": row["updated_at"],
                    "_sort": key,
                }
                if (current is not None
                        and current["status"] == "approved"
                        and row["status"] != "approved"):
                    continue
                best[row["module_id"]] = item
        return list(best.values())

    def list_module_versions(self, module_id: str) -> list[dict]:
        conn = self.connect()
        try:
            rows = conn.execute(
                "SELECT digest, version, status, size, parameters_json,"
                " report_json, updated_at FROM versions"
                " WHERE module_id = ?", (module_id,)).fetchall()
        finally:
            conn.close()
        versions = [
            {
                "version": row["version"],
                "digest": row["digest"],
                "status": row["status"],
                "size": row["size"],
                "updated_at": row["updated_at"],
                "parameters": json.loads(row["parameters_json"]),
                "report_summary": report_summary(row["report_json"]),
            }
            for row in rows
        ]
        versions.sort(key=lambda v: version_sort_key(v["version"]), reverse=True)
        return versions


def _cursor_to_tuple(cursor: str) -> tuple[str, str]:
    try:
        padded = cursor + "=" * (-len(cursor) % 4)
        raw = base64.urlsafe_b64decode(padded.encode("ascii"))
        value = json.loads(raw.decode("utf-8"))
    except (ValueError, UnicodeError):
        raise ApiError(400, "invalid_cursor", "cursor is not a valid page token")
    if (type(value) is not list or len(value) != 2
            or type(value[0]) is not str or type(value[1]) is not str):
        raise ApiError(400, "invalid_cursor", "cursor is not a valid page token")
    return value[0], value[1]


def _tuple_to_cursor(name: str, module_id: str) -> str:
    raw = json.dumps([name, module_id], ensure_ascii=True).encode("utf-8")
    return base64.urlsafe_b64encode(raw).decode("ascii").rstrip("=")


class CatalogueServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, address, catalogue: Catalogue, admin_token: str):
        self.catalogue = catalogue
        self.admin_token = admin_token.encode("utf-8")
        self.limiter = SlidingWindowLimiter(RATE_LIMIT_EVENTS,
                                            RATE_LIMIT_WINDOW)
        super().__init__(address, Handler)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "openvst-catalogue/0"
    request_id = "req"

    # -- plumbing -------------------------------------------------------

    def log_message(self, fmt: str, *args) -> None:
        line = fmt % args
        print("%s - %s" % (self.address_string(), ascii_text(line)), flush=True)

    @property
    def catalogue(self) -> Catalogue:
        return self.server.catalogue  # type: ignore[attr-defined]

    def _read_json_body(self) -> dict:
        length_header = self.headers.get("Content-Length")
        if length_header is None:
            raise ApiError(400, "invalid_request", "Content-Length is required")
        try:
            length = int(length_header)
        except ValueError:
            raise ApiError(400, "invalid_request", "Content-Length is not an integer")
        if length < 0 or length > MAX_JSON_BODY_BYTES:
            raise ApiError(413, "payload_too_large",
                           "JSON body exceeds %d bytes" % MAX_JSON_BODY_BYTES)
        body = self.rfile.read(length)
        if len(body) != length:
            raise ApiError(400, "invalid_request", "truncated request body")
        try:
            value = json.loads(body.decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            raise ApiError(400, "invalid_request", "body is not valid UTF-8 JSON")
        if type(value) is not dict:
            raise ApiError(400, "invalid_request", "body must be a JSON object")
        return value

    def _check_auth(self) -> None:
        header = self.headers.get("Authorization")
        if header is None:
            raise ApiError(401, "unauthorized", "Authorization header is required")
        parts = header.split(" ", 1)
        if len(parts) != 2 or parts[0] != "Bearer":
            raise ApiError(401, "unauthorized",
                           "Authorization must use the Bearer scheme")
        token = parts[1].strip()
        if not token or not _TOKEN_RE.match(token):
            raise ApiError(401, "unauthorized", "invalid token format")
        if not hmac.compare_digest(token.encode("utf-8"),
                                   self.server.admin_token):  # type: ignore[attr-defined]
            raise ApiError(401, "unauthorized", "invalid token")

    def _check_rate_limit(self) -> None:
        limiter = self.server.limiter  # type: ignore[attr-defined]
        if not limiter.allow(self.client_address[0]):
            raise ApiError(429, "rate_limited",
                           "too many requests; retry later")

    def _send_json(self, status: int, value: dict,
                   headers: dict[str, str] | None = None) -> None:
        body = json.dumps(value, ensure_ascii=False,
                          separators=(",", ":")).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Request-Id", self.request_id)
        for key, header_value in (headers or {}).items():
            self.send_header(key, header_value)
        self.end_headers()
        if self.command != "HEAD":
            try:
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError):
                self.close_connection = True

    def _send_error_json(self, status: int, code: str, message: str,
                         details: dict | None = None) -> None:
        # Error responses may leave an unread request body on the socket;
        # those bytes would otherwise be parsed as a new request. Close.
        self.close_connection = True
        self._send_json(status, {
            "error": {
                "code": code,
                "message": message,
                "details": details or {},
                "request_id": self.request_id,
            }
        }, headers={"Connection": "close"})

    # -- dispatch -------------------------------------------------------

    def do_GET(self) -> None:
        self._dispatch("GET")

    def do_HEAD(self) -> None:
        self._dispatch("HEAD")

    def do_POST(self) -> None:
        self._dispatch("POST")

    def _dispatch(self, method: str) -> None:
        self.request_id = uuid.uuid4().hex
        path = self.path
        query = ""
        if "?" in path:
            path, _, query = path.partition("?")
        path = urllib.parse.unquote(path)
        try:
            if method == "GET":
                if path == "/healthz":
                    return self._handle_health()
                if path == "/v1/modules":
                    return self._handle_list_modules(query)
                match = re.match(r"^/v1/modules/([^/]+)$", path)
                if match:
                    return self._handle_module_detail(match.group(1))
                match = re.match(r"^/v1/versions/([^/]+)$", path)
                if match:
                    return self._handle_version_detail(match.group(1))
                match = re.match(r"^/v1/packages/([^/]+)$", path)
                if match:
                    return self._handle_package_get(match.group(1), head=False)
                match = re.match(r"^/v1/submissions/([^/]+)$", path)
                if match:
                    return self._handle_submission_get(match.group(1))
            elif method == "HEAD":
                match = re.match(r"^/v1/packages/([^/]+)$", path)
                if match:
                    return self._handle_package_get(match.group(1), head=True)
            elif method == "POST":
                if path == "/v1/submissions":
                    return self._handle_submission()
                match = re.match(r"^/v1/versions/([^/]+)/(approve|reject)$", path)
                if match:
                    return self._handle_moderation(match.group(1),
                                                   match.group(2))
                if path == "/v1/reports":
                    return self._handle_report()
            raise ApiError(404, "not_found", "unknown route")
        except ApiError as exc:
            self._send_error_json(exc.status, exc.code, exc.message, exc.details)
        except (BrokenPipeError, ConnectionResetError):
            self.close_connection = True
        except Exception as exc:  # defensive: map anything unknown to 500
            self.log_message("internal error: %s: %s", type(exc).__name__, exc)
            self._send_error_json(500, "internal_error",
                                  "unexpected server error")

    # -- GET handlers ---------------------------------------------------

    def _handle_health(self) -> None:
        self._send_json(200, {"ok": True, "schema": CATALOGUE_SCHEMA})

    def _handle_list_modules(self, query: str) -> None:
        params = urllib.parse.parse_qs(query, keep_blank_values=True)

        def single(name: str) -> str | None:
            values = params.get(name)
            if not values:
                return None
            if len(values) != 1:
                raise ApiError(400, "invalid_request",
                               "parameter %r must appear once" % name)
            return values[0]

        include_unapproved = single("include_unapproved")
        if include_unapproved not in (None, "0", "1"):
            raise ApiError(400, "invalid_request",
                           "include_unapproved must be 0 or 1")
        statuses = PUBLIC_STATUSES if include_unapproved == "1" \
            else ("approved",)

        kind = single("kind")
        if kind is not None and kind not in KINDS:
            raise ApiError(400, "invalid_request",
                           "kind must be effect or instrument")

        status_filter = single("status")
        if status_filter is not None and status_filter not in ("approved",
                                                               "unapproved"):
            raise ApiError(400, "invalid_request",
                           "status must be approved or unapproved")

        q = single("q")
        if q is not None:
            if len(q) > MAX_Q_CHARS:
                raise ApiError(400, "invalid_request",
                               "q must be at most %d characters" % MAX_Q_CHARS)
            q = q.lower()

        limit_raw = single("limit") or "50"
        try:
            limit = int(limit_raw)
        except ValueError:
            raise ApiError(400, "invalid_request", "limit must be an integer")
        if not 1 <= limit <= 100:
            raise ApiError(400, "invalid_request", "limit must be 1..100")

        cursor_raw = single("cursor")
        cursor = _cursor_to_tuple(cursor_raw) if cursor_raw else None

        items = self.catalogue.list_latest_items(statuses, q, kind,
                                                 status_filter)
        items.sort(key=lambda item: (item["name"], item["id"]))
        if cursor is not None:
            items = [item for item in items
                     if (item["name"], item["id"]) > cursor]
        page = items[:limit]
        next_cursor = None
        if len(items) > limit and page:
            next_cursor = _tuple_to_cursor(page[-1]["name"], page[-1]["id"])
        for item in page:
            item.pop("_sort", None)
        self._send_json(200, {"items": page, "next_cursor": next_cursor})

    def _validate_module_id(self, module_id: str) -> str:
        if len(module_id) > 128 or not MODULE_ID_RE.match(module_id):
            raise ApiError(400, "invalid_request",
                           "module id must be a lowercase reverse-DNS identifier")
        return module_id

    def _handle_module_detail(self, module_id: str) -> None:
        module_id = self._validate_module_id(module_id)
        versions = self.catalogue.list_module_versions(module_id)
        if not versions:
            raise ApiError(404, "not_found", "unknown module")
        listable = self.catalogue.list_latest_items(
            PUBLIC_STATUSES, None, None, None)
        module_entry = next((item for item in listable
                             if item["id"] == module_id), None)
        self._send_json(200, {"module": module_entry, "versions": versions})

    def _validate_digest(self, digest: str) -> str:
        if not DIGEST_RE.match(digest):
            raise ApiError(400, "invalid_request",
                           "digest must be 64 lowercase hex characters")
        return digest

    def _handle_version_detail(self, digest: str) -> None:
        digest = self._validate_digest(digest)
        row = self.catalogue.get_version(digest)
        if row is None:
            raise ApiError(404, "not_found", "unknown digest")
        package_url = None
        if row["status"] in PUBLIC_STATUSES:
            package_url = "/v1/packages/" + digest
        self._send_json(200, {
            "version": {
                "digest": digest,
                "module_id": row["module_id"],
                "version": row["version"],
                "name": row["name"],
                "vendor": row["vendor"],
                "kind": row["kind"],
                "size": row["size"],
                "status": row["status"],
                "parameters": json.loads(row["parameters_json"]),
                "updated_at": row["updated_at"],
                "package_url": package_url,
            }
        })

    def _handle_submission_get(self, submission_id: str) -> None:
        self._check_auth()
        if not _SUBMISSION_ID_RE.match(submission_id):
            raise ApiError(400, "invalid_request", "invalid submission id")
        row = self.catalogue.get_submission(submission_id)
        if row is None:
            raise ApiError(404, "not_found", "unknown submission")
        self._send_json(200, {
            "submission": {
                "id": row["id"],
                "digest": row["digest"],
                "status": row["status"],
                "created_at": row["created_at"],
            },
            "report_summary": report_summary(row["report_json"]),
        })

    def _handle_package_get(self, digest: str, head: bool) -> None:
        digest = self._validate_digest(digest)
        row = self.catalogue.get_version(digest)
        if row is None:
            raise ApiError(404, "not_found", "unknown digest")
        # Rejected and revoked versions leave public download; copies already
        # installed locally are unaffected.
        if row["status"] not in PUBLIC_STATUSES:
            raise ApiError(403, "forbidden",
                           "version status %r is not downloadable" % row["status"])
        path = self.catalogue.package_path(digest)
        try:
            size = os.path.getsize(path)
        except OSError:
            self.log_message("package row exists but file is missing: %s",
                             digest)
            raise ApiError(500, "internal_error", "package file is missing")

        etag = '"%s"' % digest
        base_headers = {
            "ETag": etag,
            "Cache-Control": "public, max-age=31536000, immutable",
            "Accept-Ranges": "bytes",
        }
        if_none_match = self.headers.get("If-None-Match")
        if if_none_match and digest in if_none_match:
            self.send_response(304)
            for key, value in base_headers.items():
                self.send_header(key, value)
            self.send_header("X-Request-Id", self.request_id)
            self.end_headers()
            return

        range_header = self.headers.get("Range")
        start, end = 0, size - 1
        status = 200
        if range_header:
            parsed = _parse_range(range_header, size)
            if parsed is None:
                self._send_error_json(
                    416, "invalid_range",
                    "requested range is not satisfiable",
                    details={"size": size})
                return
            start, end, status = parsed

        length = end - start + 1
        self.send_response(status)
        self.send_header("Content-Type", "application/zip")
        self.send_header("Content-Length", str(length))
        self.send_header("X-Request-Id", self.request_id)
        for key, value in base_headers.items():
            self.send_header(key, value)
        if status == 206:
            self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, size))
        self.end_headers()
        if head:
            return
        try:
            with open(path, "rb") as handle:
                handle.seek(start)
                remaining = length
                while remaining > 0:
                    chunk = handle.read(min(FILE_CHUNK, remaining))
                    if not chunk:
                        break
                    self.wfile.write(chunk)
                    remaining -= len(chunk)
        except (BrokenPipeError, ConnectionResetError):
            self.close_connection = True

    # -- POST handlers --------------------------------------------------

    def _handle_submission(self) -> None:
        self._check_auth()
        self._check_rate_limit()

        if self.headers.get("Transfer-Encoding"):
            raise ApiError(400, "invalid_request",
                           "chunked transfer encoding is not supported; "
                           "send Content-Length")
        length_header = self.headers.get("Content-Length")
        if length_header is None:
            raise ApiError(400, "invalid_request", "Content-Length is required")
        try:
            length = int(length_header)
        except ValueError:
            raise ApiError(400, "invalid_request", "Content-Length is not an integer")
        if length <= 0:
            raise ApiError(400, "invalid_request", "Content-Length must be positive")

        content_type = (self.headers.get("Content-Type") or "").split(";")[0].strip().lower()
        if content_type != "application/zip":
            raise ApiError(400, "invalid_request",
                           "Content-Type must be application/zip")

        idempotency_key = self.headers.get("Idempotency-Key")
        if idempotency_key is None:
            raise ApiError(400, "invalid_request",
                           "Idempotency-Key header is required")
        if not _IDEMPOTENCY_RE.match(idempotency_key):
            raise ApiError(400, "invalid_request",
                           "Idempotency-Key must be 8..128 printable ASCII characters")

        max_bytes = self.catalogue.max_package_bytes
        if length > max_bytes:
            # Refuse before reading the body; the connection is closed so a
            # client mid-upload cannot keep streaming into a void.
            self.close_connection = True
            raise ApiError(413, "payload_too_large",
                           "package exceeds the %d byte limit" % max_bytes)

        declared_digest = self.headers.get("X-Content-SHA256")
        if declared_digest is not None:
            declared_digest = declared_digest.strip().lower()
            if not DIGEST_RE.match(declared_digest):
                raise ApiError(400, "invalid_request",
                               "X-Content-SHA256 must be 64 lowercase hex characters")

        tmp_path = self.catalogue.new_tmp_path()
        hasher = hashlib.sha256()
        total = 0
        try:
            with open(tmp_path, "wb") as out:
                remaining = length
                while remaining > 0:
                    chunk = self.rfile.read(min(UPLOAD_CHUNK, remaining))
                    if not chunk:
                        raise ApiError(400, "invalid_request",
                                       "client disconnected before sending the full body")
                    remaining -= len(chunk)
                    total += len(chunk)
                    hasher.update(chunk)
                    out.write(chunk)
        except ApiError:
            self.catalogue.discard_tmp(tmp_path)
            raise
        except OSError as exc:
            self.catalogue.discard_tmp(tmp_path)
            self.log_message("staging write failed: %s", exc)
            raise ApiError(500, "internal_error", "staging failed")

        digest = hasher.hexdigest()
        if declared_digest is not None and declared_digest != digest:
            self.catalogue.discard_tmp(tmp_path)
            raise ApiError(400, "digest_mismatch",
                           "X-Content-SHA256 does not match the received bytes")

        limits = Limits(max_package_bytes=max_bytes)
        report = validate_package(tmp_path, limits=limits)
        report_dict = sanitize_report(report.to_dict())
        if not report.ok:
            self.catalogue.discard_tmp(tmp_path)
            raise ApiError(
                422, "package_rejected", "package failed validation",
                details={"report": bounded_report(report_dict)})

        self._register_submission(tmp_path, digest, report.module or {},
                                  length, report.parameters, report_dict,
                                  idempotency_key)

    def _register_submission(self, tmp_path: str, digest: str, module: dict,
                             size: int, parameters: list, report_dict: dict,
                             idempotency_key: str) -> None:
        """Insert version and submission rows, then publish the package.

        The database transaction opens before the file move and commits
        after it: a publish failure rolls back cleanly, and the only crash
        window left behind is an orphaned content-addressed file that no row
        references, which is harmless.
        """
        submission_id = "sub_" + uuid.uuid4().hex
        stamp = now_iso()
        report_json = json.dumps(report_dict, ensure_ascii=False,
                                 separators=(",", ":"))
        parameters_json = json.dumps(parameters, ensure_ascii=False,
                                     separators=(",", ":"))
        conn = self.catalogue.connect()
        published = False
        try:
            conn.execute("BEGIN IMMEDIATE")
            clash = conn.execute(
                "SELECT digest FROM versions WHERE module_id = ? AND version = ?",
                (module["id"], module["version"])).fetchone()
            if clash is not None and clash["digest"] != digest:
                conn.rollback()
                raise ApiError(
                    409, "conflict",
                    "module id and version already exist with different "
                    "content; publish a new version instead")
            existing = conn.execute(
                "SELECT status FROM versions WHERE digest = ?",
                (digest,)).fetchone()
            if existing is not None:
                # Same bytes again: idempotent. A staged duplicate is
                # redundant; the stored package is immutable.
                conn.rollback()
                submission = self.catalogue.find_submission_by_digest(digest)
                self._send_json(200, {
                    "submission_id": submission["id"] if submission else None,
                    "digest": digest,
                    "status": existing["status"],
                })
                return
            duplicate_key = conn.execute(
                "SELECT id FROM submissions WHERE idempotency_key = ?",
                (idempotency_key,)).fetchone()
            if duplicate_key is not None:
                conn.rollback()
                raise ApiError(
                    409, "conflict",
                    "Idempotency-Key was already used for a different package")
            conn.execute(
                "INSERT INTO versions (digest, module_id, version, name,"
                " vendor, kind, size, status, parameters_json,"
                " report_json, created_at, updated_at)"
                " VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
                (digest, module["id"], module["version"], module["name"],
                 module["vendor"], module["kind"], size, "unapproved",
                 parameters_json, report_json, stamp, stamp))
            try:
                conn.execute(
                    "INSERT INTO submissions (id, digest, idempotency_key,"
                    " status, report_json, created_at)"
                    " VALUES (?,?,?,?,?,?)",
                    (submission_id, digest, idempotency_key, "unapproved",
                     report_json, stamp))
            except sqlite3.IntegrityError:
                # Same digest inserted concurrently: idempotent response.
                existing = conn.execute(
                    "SELECT status FROM versions WHERE digest = ?",
                    (digest,)).fetchone()
                conn.rollback()
                if existing is not None:
                    self._send_json(200, {
                        "submission_id": None,
                        "digest": digest,
                        "status": existing["status"],
                    })
                    return
                raise ApiError(409, "conflict",
                               "Idempotency-Key already used")
            self.catalogue.publish(tmp_path, digest)
            published = True
            conn.execute(
                "INSERT INTO audit (digest, action, actor, reason, created_at)"
                " VALUES (?,?,?,?,?)",
                (digest, "submit", "token", None, stamp))
            conn.commit()
        except ApiError:
            if not published:
                self.catalogue.discard_tmp(tmp_path)
            raise
        except Exception:
            conn.rollback()
            self.catalogue.discard_tmp(tmp_path)
            raise
        finally:
            conn.close()
        self._send_json(201, {
            "submission_id": submission_id,
            "digest": digest,
            "status": "unapproved",
        })

    def _handle_moderation(self, digest: str, action: str) -> None:
        self._check_auth()
        self._check_rate_limit()
        digest = self._validate_digest(digest)
        body = self._read_json_body()
        reason = body.get("reason")
        if reason is not None and type(reason) is not str:
            raise ApiError(400, "invalid_request", "reason must be a string")
        if type(reason) is str:
            reason = reason.strip()
            if len(reason) > MAX_REASON_CHARS:
                raise ApiError(400, "invalid_request",
                               "reason must be at most %d characters"
                               % MAX_REASON_CHARS)
        row, idempotent = self.catalogue.transition(
            digest, action, reason, "token")
        self._send_json(200, {
            "version": {
                "digest": digest,
                "module_id": row["module_id"],
                "version": row["version"],
                "status": row["status"],
            },
            "idempotent": idempotent,
        })

    def _handle_report(self) -> None:
        self._check_auth()
        self._check_rate_limit()
        body = self._read_json_body()
        digest = body.get("digest")
        category = body.get("category")
        message = body.get("message")
        if type(digest) is not str or not DIGEST_RE.match(digest):
            raise ApiError(400, "invalid_request",
                           "digest must be 64 lowercase hex characters")
        if type(category) is not str or category not in REPORT_CATEGORIES:
            raise ApiError(400, "invalid_request",
                           "category must be one of security, rights, content, other")
        if type(message) is not str or not 1 <= len(message) <= MAX_REPORT_MESSAGE_CHARS:
            raise ApiError(400, "invalid_request",
                           "message must be 1..%d characters"
                           % MAX_REPORT_MESSAGE_CHARS)
        if self.catalogue.get_version(digest) is None:
            raise ApiError(404, "not_found", "unknown digest")
        report_id = self.catalogue.insert_report(digest, category, message)
        self._send_json(201, {"report_id": report_id})


def _parse_range(header: str, size: int) -> tuple[int, int, int] | None:
    """Parse a single Range header. Returns (start, end, status) or None."""
    value = header.strip()
    if value.startswith("bytes=") and "," in value:
        return None  # multi-range is refused, not repaired
    match = _RANGE_RE.match(value)
    if not match:
        return None
    first = int(match.group(1))
    last_raw = match.group(2)
    if last_raw == "":
        if first >= size:
            return None
        return first, size - 1, 206
    last = int(last_raw)
    if first > last or first >= size:
        return None
    return first, min(last, size - 1), 206


def seed_mode(data_dir: str, seed_path: str, admin_token: str) -> int:
    """Validate one package and insert it as approved; for demos and tests."""
    catalogue = Catalogue(data_dir)
    report = validate_package(seed_path)
    if not report.ok:
        print("seed rejected:", flush=True)
        for issue in report.errors:
            print("  %s: %s" % (issue.code, ascii_text(issue.message)),
                  flush=True)
        return 2
    digest = hashlib.sha256(open(seed_path, "rb").read()).hexdigest()
    final = catalogue.package_path(digest)
    if os.path.exists(final):
        existing = catalogue.get_version(digest)
        if existing is not None:
            print("seed already present: %s (%s)" % (digest,
                                                     existing["status"]),
                  flush=True)
            return 0
    catalogue.publish(seed_path, digest)
    module = report.module or {}
    size = os.path.getsize(catalogue.package_path(digest))
    report_dict = sanitize_report(report.to_dict())
    row = catalogue.insert_version(digest, module, size, "approved",
                                   report.parameters, report_dict,
                                   "seed")
    print("seeded approved version: module=%s version=%s digest=%s status=%s"
          % (module.get("id"), module.get("version"), digest, row["status"]),
          flush=True)
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="OpenVST catalogue server (v0, standard library only)")
    parser.add_argument("--data", required=True,
                        help="data directory (SQLite database, packages/, tmp/)")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--admin-token", default=None,
                        help="bearer token for submissions and moderation "
                             "(falls back to OPENVST_ADMIN_TOKEN)")
    parser.add_argument("--max-package-bytes", type=int,
                        default=MAX_PACKAGE_BYTES,
                        help="maximum accepted package size (default 512 MiB)")
    parser.add_argument("--seed", default=None, metavar="FILE.zip",
                        help="validate FILE.zip, insert it as approved, exit")
    args = parser.parse_args(argv)

    token = args.admin_token or os.environ.get("OPENVST_ADMIN_TOKEN")
    if not token:
        parser.error(
            "no admin token: set OPENVST_ADMIN_TOKEN or pass --admin-token")
    if not _TOKEN_RE.match(token):
        parser.error("admin token must be 8..256 printable ASCII characters")
    if args.max_package_bytes <= 0:
        parser.error("--max-package-bytes must be positive")

    if args.seed:
        return seed_mode(args.data, args.seed, token)

    catalogue = Catalogue(args.data, args.max_package_bytes)
    server = CatalogueServer((args.host, args.port), catalogue, token)
    host, port = server.server_address[:2]
    print("openvst-catalogue v0 listening on %s:%d (data: %s)"
          % (host, port, ascii_text(catalogue.data_dir)), flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
