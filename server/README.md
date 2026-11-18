# OpenVST catalogue server (v0)

A small, dependency-free catalogue server for OpenVST modules: listing,
metadata, immutable package downloads with Range support, package
submissions validated with `openvst/package_validation.py`, moderation and
user reports.

Python 3.11+ standard library only. No Flask, no external packages, no
Docker requirement. Single process, `http.server.ThreadingHTTPServer`,
SQLite in WAL mode, content-addressed package storage.

## Run

    OPENVST_ADMIN_TOKEN='<some long random token>' \
        python3 server/openvst_catalogue.py --data /var/lib/openvst-catalogue

Flags:

    --data DIR              required; holds catalogue.sqlite3, packages/, tmp/
    --host HOST             default 127.0.0.1
    --port PORT             default 8080
    --admin-token TOKEN     falls back to OPENVST_ADMIN_TOKEN; refuses to
                            start without one
    --max-package-bytes N   default 536870912 (512 MiB)
    --seed FILE.zip         validate FILE.zip, insert it as approved, exit

Seed a demo catalogue:

    python3 server/openvst_catalogue.py --data /var/lib/openvst-catalogue \
        --seed my_module.zip

The server never starts without an admin token. All HTTP strings and logs
are ASCII.

## Data layout

    DIR/catalogue.sqlite3        SQLite, WAL mode; versions, submissions, audit, reports
    DIR/packages/ab/cd/<sha256>.zip   immutable, content-addressed, written once
    DIR/tmp/                     bounded staging, wiped on start

Package bytes are never rewritten after publish. `tmp/` and `packages/`
share the data directory so publishing is an atomic `os.replace`.

## API

Reading is anonymous. Writing (submissions, moderation, reports) requires
`Authorization: Bearer <admin-token>`. There is one token in v0; there are
no accounts yet.

    GET  /healthz
    GET  /v1/modules?q=&kind=&status=&include_unapproved=1&limit=50&cursor=
    GET  /v1/modules/{id}
    GET  /v1/versions/{digest}
    GET  /v1/packages/{digest}          exact ZIP bytes, ETag, immutable, Range
    POST /v1/submissions                raw ZIP body (not multipart)
    GET  /v1/submissions/{id}           (auth)
    POST /v1/versions/{digest}/approve  (auth) {"reason": "..."}
    POST /v1/versions/{digest}/reject   (auth) {"reason": "..."}
    POST /v1/reports                    (auth) {"digest","category","message"}

Listing defaults to approved versions only, one row per module id (latest
approved version), stable sort by `name,id`, opaque base64url cursor.
`include_unapproved=1` also shows unapproved versions (never rejected or
revoked); an approved version still wins over a newer unapproved one.
`kind` is `effect|instrument`; `q` is a case-insensitive substring over
name/id/vendor, max 64 characters; `limit` is 1..100.

Errors are always:

    {"error":{"code":"...","message":"...","details":{},"request_id":"..."}}

### curl examples

    curl -s http://127.0.0.1:8080/healthz

    # List approved modules, two per page
    curl -s 'http://127.0.0.1:8080/v1/modules?limit=2'

    # Search instruments
    curl -s 'http://127.0.0.1:8080/v1/modules?kind=instrument&q=saturator'

    # Module detail (all versions and their statuses)
    curl -s http://127.0.0.1:8080/v1/modules/io.openvst.klang.tapesat

    # Download, resume at byte 1000
    curl -s -H 'Range: bytes=1000-' \
        -o module.zip http://127.0.0.1:8080/v1/packages/<64-hex-digest>

    # Submit a package (raw ZIP bytes; Idempotency-Key makes retries safe)
    curl -s -X POST http://127.0.0.1:8080/v1/submissions \
        -H "Authorization: Bearer $OPENVST_ADMIN_TOKEN" \
        -H 'Content-Type: application/zip' \
        -H 'Idempotency-Key: my-retry-safe-key-1' \
        --data-binary @my_module.zip

    # Moderate (idempotent on the same transition)
    curl -s -X POST http://127.0.0.1:8080/v1/versions/<digest>/approve \
        -H "Authorization: Bearer $OPENVST_ADMIN_TOKEN" \
        -H 'Content-Type: application/json' \
        -d '{"reason":"reviewed, sounds good"}'

    # Report a version
    curl -s -X POST http://127.0.0.1:8080/v1/reports \
        -H "Authorization: Bearer $OPENVST_ADMIN_TOKEN" \
        -H 'Content-Type: application/json' \
        -d '{"digest":"<digest>","category":"security","message":"why"}'

Submission rules: `Content-Type: application/zip`, a required
`Idempotency-Key` (8..128 printable ASCII), a required `Content-Length`
(chunked transfer encoding is refused). Optionally send
`X-Content-SHA256: <64 lowercase hex>`; a mismatch is a 400. The server
hashes the received bytes itself and never trusts the declared digest.

Outcomes: valid package -> 201, status `unapproved`; same bytes again ->
200 (idempotent); invalid package -> 422 with a bounded error report
(archive-internal paths only, never filesystem paths); oversized body ->
413 with the connection closed before the body is read; a package whose
`(module_id, version)` already exists with different bytes -> 409, because
(id, version) is immutable.

Download rules: `ETag` is the digest, `Cache-Control` is
`public, max-age=31536000, immutable`, `Accept-Ranges: bytes`. A single
`Range: bytes=start-end` (suffix `-N` also works) returns 206 with
`Content-Range`; multi-range and unsatisfiable ranges return 416. Metadata
for any known digest is visible, but rejected and revoked versions answer
403 on the package endpoint: rejection stops public download while the
bytes are retained.

## Security model (honest)

- The server never trusts the client: sizes are bounded, the digest is
  recomputed from received bytes, every package goes through
  `openvst.package_validation.validate_package` before it can be listed or
  downloaded, and unsafe paths are rejected, never repaired.
- Digests must match `^[0-9a-f]{64}$`; module ids must be lowercase
  reverse-DNS. No path from user input is ever joined without validation.
- Validation checks structure and hygiene only. It does NOT prove DSP
  safety, licensing rights or runtime compatibility. Unapproved versions
  are opt-in in listings; do not auto-install them without a warning.
- The report stored per version has filesystem paths stripped before it is
  persisted (no absolute paths in state).

## Deliberately not implemented (v0)

- Accounts, per-author identity, email verification, 2FA: one shared
  admin bearer token instead.
- Multipart or resumable uploads: raw ZIP body with Idempotency-Key.
- TLS: terminate at a reverse proxy.
- Quotas, per-account rate limits: a simple per-IP sliding window
  (120 writes per minute) returns 429; nothing finer.
- Revocation endpoint and Unapproved UI flows: schema and status handling
  are ready (`revoked` answers 403 on downloads), the endpoint is not.
- Signatures / attestations: live outside the ZIP; not served here yet.

## Deployment sketch

    # Caddy in front: TLS + no need to expose the Python process directly
    catalogue.example.com {
        reverse_proxy 127.0.0.1:8080
        request_body max_size 537MB
    }

    # systemd unit
    [Unit]
    Description=OpenVST catalogue
    After=network.target

    [Service]
    User=openvst
    Environment=OPENVST_ADMIN_TOKEN=<from a credentials file or systemd secret>
    ExecStart=/usr/bin/python3 /opt/openvst/server/openvst_catalogue.py \
        --data /var/lib/openvst-catalogue --port 8080
    Restart=on-failure
    NoNewPrivileges=true
    ProtectSystem=strict
    ReadWritePaths=/var/lib/openvst-catalogue

    [Install]
    WantedBy=multi-user.target

Keep the package store read-only for the service user after publish if you
can (chmod via a helper or a separate user); the server only writes there
through `os.replace` at publish time.

## Tests

    python3 -m unittest discover -s tests/server -v
