"""Read-only validation of OpenVST package archives (contract v0).

The validator never extracts, executes or compiles archive content. It opens
the ZIP, reads entries in bounded streaming chunks, parses two JSON documents
strictly and returns a structured report. Nothing is written next to the
package.

Out of scope for this brick: signatures, author identity, actual licensing
rights, binary runtime compatibility, DAW behaviour and DSP safety.
"""

from __future__ import annotations

import hashlib
import json
import math
import os
import posixpath
import re
import stat
import unicodedata
import zipfile
import zlib
from dataclasses import asdict, dataclass, field
from typing import Any, BinaryIO

SCHEMA = "openvst.package/0"
MANIFEST_NAME = "openvst.json"
ENGINE = "cmajor"
PATCH_FORMAT = 1
HOST_API = "openvst-host/0"

_RATIO_MIN_BYTES = 1024
_TOTAL_RATIO_MIN_BYTES = 64 * 1024
_MAX_ABS_NUMBER = 10**12
_MAX_PATCH_SOURCES = 64
_MAX_TEXT = {
    "module.id": 128,
    "module.version": 64,
    "module.name": 128,
    "module.vendor": 128,
    "module.license": 64,
    "parameter.id": 64,
    "parameter.name": 128,
    "parameter.unit": 16,
    "resource.path": 240,
    "resource.license": 64,
}

_ASSET_SUFFIXES = frozenset({
    ".wav", ".flac", ".aif", ".aiff", ".ogg", ".opus", ".mp3",
    ".png", ".jpg", ".jpeg", ".webp",
})
_DSP_SUFFIXES = frozenset({".cmajor", ".cmajorpatch"})
_ALLOWED_COMPRESSION = frozenset({zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED})
_WINDOWS_RESERVED = frozenset(
    {"con", "prn", "aux", "nul", "conin$", "conout$"}
    | {f"com{i}" for i in range(1, 10)}
    | {f"lpt{i}" for i in range(1, 10)}
    | {f"{prefix}{superdigits}" for prefix in ("com", "lpt")
       for superdigits in ("\u00b9", "\u00b2", "\u00b3")}
)
_WINDOWS_FORBIDDEN_CHARS = frozenset('<>"|?*')
_ID_RE = re.compile(
    r"^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?"
    r"(?:\.[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?)+$"
)
_SEMVER_CORE_RE = re.compile(
    r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\Z")
_SEMVER_IDENT_RE = re.compile(r"^[0-9A-Za-z-]+\Z")
_SHA256_RE = re.compile(r"^[0-9a-f]{64}$")

# Contract v0 forbids patch workers, scripted views and external references.
# Only these plain Cmajor patch properties are accepted; anything else (worker,
# view/views, sourceTransformer, resources, externals, custom fields) is
# rejected so no JS or out-of-bundle reference can hide in the patch manifest.
_PATCH_REQUIRED = frozenset({"CmajorVersion", "ID", "version", "name", "source"})
_PATCH_ALLOWED = frozenset({
    "CmajorVersion", "ID", "version", "name", "description", "manufacturer",
    "category", "mainProcessor", "isInstrument", "source",
})
_PATCH_TEXT_LIMITS = {
    "ID": 128,
    "version": 64,
    "name": 128,
    "description": 512,
    "manufacturer": 128,
    "category": 64,
    "mainProcessor": 128,
}

NOT_VALIDATED = (
    "package signature and publisher authenticity (detached, outside the ZIP)",
    "author identity or account ownership",
    "actual licensing rights and SPDX expression correctness",
    "binary runtime, host API or DAW compatibility",
    "DSP code safety, correctness or sandboxing",
    "installation, extraction and execution behaviour",
    "concurrent replacement or in-place mutation after validation; an installer "
    "must consume the verified snapshot and re-check its sha256",
)


class _DuplicateKey(ValueError):
    pass


class _NonFiniteNumber(ValueError):
    pass


def _reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise _DuplicateKey(f"duplicate object key {key!r}")
        result[key] = value
    return result


def _reject_constant(token: str) -> Any:
    raise _NonFiniteNumber(f"non-finite JSON number {token!r}")


def parse_json_strict(data: bytes) -> Any:
    """Parse UTF-8 JSON, rejecting duplicate keys and NaN/Infinity."""
    text = data.decode("utf-8")
    return json.loads(
        text,
        object_pairs_hook=_reject_duplicate_keys,
        parse_constant=_reject_constant,
    )


@dataclass(frozen=True)
class Limits:
    """Hard caps applied to a package archive.

    ``chunk_size`` is the streaming read size; every other field is a ceiling.
    """

    max_package_bytes: int = 512 * 1024 * 1024
    max_entries: int = 20_000
    max_total_uncompressed: int = 2 * 1024 * 1024 * 1024
    max_entry_uncompressed: int = 512 * 1024 * 1024
    max_compression_ratio: int = 100
    max_path_chars: int = 240
    max_manifest_bytes: int = 256 * 1024
    max_patch_bytes: int = 1024 * 1024
    max_resource_files: int = 4096
    max_parameters: int = 128
    chunk_size: int = 64 * 1024

    def __post_init__(self) -> None:
        for name in (
            "max_package_bytes", "max_entries", "max_total_uncompressed",
            "max_entry_uncompressed", "max_compression_ratio", "max_path_chars",
            "max_manifest_bytes", "max_patch_bytes", "max_resource_files",
            "max_parameters", "chunk_size",
        ):
            value = getattr(self, name)
            if type(value) is not int or value <= 0:
                raise ValueError(f"Limits.{name} must be a positive integer")


@dataclass(frozen=True)
class Issue:
    code: str
    message: str
    path: str | None = None

    def to_dict(self) -> dict[str, Any]:
        return {"code": self.code, "message": self.message, "path": self.path}


@dataclass(frozen=True)
class InventoryEntry:
    path: str
    size: int
    sha256: str

    def to_dict(self) -> dict[str, Any]:
        return {"path": self.path, "size": self.size, "sha256": self.sha256}


@dataclass
class PackageReport:
    package_path: str
    package_size: int | None = None
    package_sha256: str | None = None
    module: dict[str, Any] | None = None
    parameters: list[dict[str, Any]] = field(default_factory=list)
    resources: list[dict[str, Any]] = field(default_factory=list)
    inventory: list[InventoryEntry] = field(default_factory=list)
    errors: list[Issue] = field(default_factory=list)
    warnings: list[Issue] = field(default_factory=list)
    limits: dict[str, int] = field(default_factory=dict)

    @property
    def ok(self) -> bool:
        return not self.errors

    def error(self, code: str, message: str, path: str | None = None) -> None:
        self.errors.append(Issue(code, message, path))

    def to_dict(self) -> dict[str, Any]:
        return {
            "ok": self.ok,
            "package": {
                "path": self.package_path,
                "size": self.package_size,
                "sha256": self.package_sha256,
            },
            "module": self.module,
            "parameters": self.parameters,
            "resources": self.resources,
            "inventory": [entry.to_dict() for entry in self.inventory],
            "errors": [issue.to_dict() for issue in self.errors],
            "warnings": [issue.to_dict() for issue in self.warnings],
            "limits": self.limits,
            "not_validated": list(NOT_VALIDATED),
        }


def _entry_path_problem(raw: str, limits: Limits) -> str | None:
    """Return a rejection reason, or None when the entry path is acceptable."""
    if not raw:
        return "empty entry name"
    if len(raw) > limits.max_path_chars:
        return f"longer than {limits.max_path_chars} characters"
    name = raw[:-1] if raw.endswith("/") else raw
    if not name:
        return "empty entry name"
    if name != unicodedata.normalize("NFC", name):
        return "not Unicode NFC"
    if "\\" in name:
        return "backslash separator"
    if name.startswith("/"):
        return "absolute path"
    if re.match(r"^[A-Za-z]:", name):
        return "Windows drive prefix"
    for component in name.split("/"):
        if component in ("", ".", ".."):
            return "empty, dot or traversal component"
        if ":" in component:
            return "colon in component (ADS or drive)"
        if any(char in _WINDOWS_FORBIDDEN_CHARS for char in component):
            return "character forbidden on Windows"
        if any(unicodedata.category(char) == "Cc" for char in component):
            return "control character"
        if component[-1] in (".", " "):
            return "trailing dot or space"
        if component.split(".", 1)[0].casefold() in _WINDOWS_RESERVED:
            return "reserved Windows device name"
    return None


def _text(report: PackageReport, value: Any, where: str, max_len: int,
          path: str | None = MANIFEST_NAME) -> str | None:
    if type(value) is not str:
        report.error("manifest_type", f"{where} must be a string", path)
        return None
    if not 1 <= len(value) <= max_len:
        report.error("manifest_text", f"{where} must be 1..{max_len} characters", path)
        return None
    if any(unicodedata.category(char) == "Cc" for char in value):
        report.error("manifest_text", f"{where} must not contain control characters", path)
        return None
    return value


def _number(report: PackageReport, value: Any, where: str) -> int | float | None:
    if type(value) not in (int, float):
        report.error("manifest_type", f"{where} must be a number", MANIFEST_NAME)
        return None
    if type(value) is float and not math.isfinite(value):
        report.error("manifest_value", f"{where} must be finite", MANIFEST_NAME)
        return None
    if abs(value) > _MAX_ABS_NUMBER:
        report.error("manifest_value", f"{where} magnitude is too large", MANIFEST_NAME)
        return None
    return value


def _is_semver(value: str) -> bool:
    """Validate SemVer 2.0.0 shape (core, prerelease, build metadata)."""
    if len(value) > _MAX_TEXT["module.version"]:
        return False
    core, plus, build = value.partition("+")
    if plus:
        if not build or any(not _SEMVER_IDENT_RE.match(part) for part in build.split(".")):
            return False
    base, dash, prerelease = core.partition("-")
    if not _SEMVER_CORE_RE.match(base):
        return False
    if dash:
        if not prerelease:
            return False
        for part in prerelease.split("."):
            if not _SEMVER_IDENT_RE.match(part):
                return False
            if part.isascii() and part.isdigit() and len(part) > 1 and part[0] == "0":
                return False
    return True


def _require_keys(report: PackageReport, obj: dict[str, Any], where: str,
                  required: set[str], allowed: set[str]) -> bool:
    ok = True
    for key in sorted(required - obj.keys()):
        report.error("manifest_field_missing", f"{where}.{key} is required", MANIFEST_NAME)
        ok = False
    for key in sorted(obj.keys() - allowed):
        report.error("manifest_field_unknown",
                     f"{where}.{key} is not allowed by {SCHEMA}", MANIFEST_NAME)
        ok = False
    return ok


def _validate_module(report: PackageReport, module: Any) -> dict[str, Any] | None:
    if type(module) is not dict:
        report.error("manifest_type", "module must be an object", MANIFEST_NAME)
        return None
    if not _require_keys(
        report, module, "module",
        {"id", "version", "name", "kind", "vendor", "license"},
        {"id", "version", "name", "kind", "vendor", "license"},
    ):
        return None

    module_id = _text(report, module["id"], "module.id", _MAX_TEXT["module.id"])
    version = _text(report, module["version"], "module.version", _MAX_TEXT["module.version"])
    name = _text(report, module["name"], "module.name", _MAX_TEXT["module.name"])
    vendor = _text(report, module["vendor"], "module.vendor", _MAX_TEXT["module.vendor"])
    license_name = _text(report, module["license"], "module.license",
                         _MAX_TEXT["module.license"])
    kind = module["kind"]
    if type(kind) is not str or kind not in ("effect", "instrument"):
        report.error("manifest_value", "module.kind must be 'effect' or 'instrument'",
                     MANIFEST_NAME)
        kind = None

    failed = False
    if module_id is not None and not _ID_RE.match(module_id):
        report.error("manifest_value",
                     "module.id must be a lowercase reverse-DNS identifier", MANIFEST_NAME)
        failed = True
    if version is not None and not _is_semver(version):
        report.error("manifest_value", "module.version must be SemVer MAJOR.MINOR.PATCH",
                     MANIFEST_NAME)
        failed = True
    if failed or None in (module_id, version, name, vendor, license_name, kind):
        return None
    return {
        "id": module_id,
        "version": version,
        "name": name,
        "kind": kind,
        "vendor": vendor,
        "license": license_name,
    }


def _validate_runtime(report: PackageReport, runtime: Any,
                      limits: Limits) -> dict[str, Any] | None:
    if type(runtime) is not dict:
        report.error("manifest_type", "runtime must be an object", MANIFEST_NAME)
        return None
    if not _require_keys(
        report, runtime, "runtime",
        {"engine", "patch_format", "host_api", "patch"},
        {"engine", "patch_format", "host_api", "patch"},
    ):
        return None
    failed = False
    if type(runtime["engine"]) is not str:
        report.error("manifest_type", "runtime.engine must be a string", MANIFEST_NAME)
        failed = True
    elif runtime["engine"] != ENGINE:
        report.error("manifest_value", f"runtime.engine must be {ENGINE!r}", MANIFEST_NAME)
        failed = True
    if type(runtime["patch_format"]) is not int:
        report.error("manifest_type", "runtime.patch_format must be an integer",
                     MANIFEST_NAME)
        failed = True
    elif runtime["patch_format"] != PATCH_FORMAT:
        report.error("manifest_value", f"runtime.patch_format must be {PATCH_FORMAT}",
                     MANIFEST_NAME)
        failed = True
    if type(runtime["host_api"]) is not str:
        report.error("manifest_type", "runtime.host_api must be a string", MANIFEST_NAME)
        failed = True
    elif runtime["host_api"] != HOST_API:
        report.error("manifest_value", f"runtime.host_api must be {HOST_API!r}",
                     MANIFEST_NAME)
        failed = True

    patch_path = _text(report, runtime["patch"], "runtime.patch",
                       min(limits.max_path_chars, _MAX_TEXT["resource.path"]))
    if patch_path is None:
        return None
    problem = _entry_path_problem(patch_path, limits)
    if problem:
        report.error("manifest_value", f"runtime.patch is unsafe: {problem}", MANIFEST_NAME)
        failed = True
    elif not patch_path.startswith("patch/") or not patch_path.lower().endswith(".cmajorpatch"):
        report.error("manifest_value",
                     "runtime.patch must be a .cmajorpatch file under patch/", MANIFEST_NAME)
        failed = True
    if failed:
        return None
    return {"engine": ENGINE, "patch_format": PATCH_FORMAT,
            "host_api": HOST_API, "patch": patch_path}


def _validate_parameters(report: PackageReport, parameters: Any,
                         limits: Limits) -> list[dict[str, Any]] | None:
    if type(parameters) is not list:
        report.error("manifest_type", "parameters must be an array", MANIFEST_NAME)
        return None
    if len(parameters) > limits.max_parameters:
        report.error("manifest_limit",
                     f"parameters exceeds {limits.max_parameters} entries", MANIFEST_NAME)
        return None

    parsed: list[dict[str, Any]] = []
    ids: set[str] = set()
    indexes: set[int] = set()
    failed = False
    allowed = {"id", "name", "min", "max", "default", "unit", "index"}
    for position, item in enumerate(parameters):
        where = f"parameters[{position}]"
        if type(item) is not dict:
            report.error("manifest_type", f"{where} must be an object", MANIFEST_NAME)
            failed = True
            continue
        before = len(report.errors)
        if not _require_keys(report, item, where,
                             {"id", "name", "min", "max", "default"}, allowed):
            failed = True
            continue
        parameter_id = _text(report, item["id"], f"{where}.id",
                             _MAX_TEXT["parameter.id"])
        name = _text(report, item["name"], f"{where}.name", _MAX_TEXT["parameter.name"])
        minimum = _number(report, item["min"], f"{where}.min")
        maximum = _number(report, item["max"], f"{where}.max")
        default = _number(report, item["default"], f"{where}.default")
        unit = None
        if "unit" in item:
            unit = _text(report, item["unit"], f"{where}.unit", _MAX_TEXT["parameter.unit"])
        index = None
        if "index" in item:
            index = item["index"]
            if type(index) is not int:
                report.error("manifest_type", f"{where}.index must be an integer",
                             MANIFEST_NAME)
                failed = True
                index = None
            elif not 0 <= index < limits.max_parameters:
                report.error("parameter_range",
                             f"{where}.index must be in 0..{limits.max_parameters - 1}",
                             MANIFEST_NAME)
                failed = True
                index = None
        if len(report.errors) != before:
            failed = True
            continue
        if parameter_id in ids:
            report.error("parameter_duplicate_id",
                         f"{where}.id duplicates {parameter_id!r}", MANIFEST_NAME)
            failed = True
            continue
        if minimum > maximum:
            report.error("parameter_range", f"{where}.min must not exceed max", MANIFEST_NAME)
            failed = True
            continue
        if default < minimum or default > maximum:
            report.error("parameter_range",
                         f"{where}.default must be within [min, max]", MANIFEST_NAME)
            failed = True
            continue
        if index is not None:
            if index in indexes:
                report.error("parameter_range", f"{where}.index duplicates {index}", MANIFEST_NAME)
                failed = True
                continue
            indexes.add(index)
        ids.add(parameter_id)
        entry = {"id": parameter_id, "name": name, "min": minimum,
                 "max": maximum, "default": default}
        if unit is not None:
            entry["unit"] = unit
        if index is not None:
            entry["index"] = index
        parsed.append(entry)
    if failed:
        return None
    return parsed


def _validate_resources(report: PackageReport, resources: Any,
                        limits: Limits) -> list[dict[str, Any]] | None:
    if type(resources) is not list:
        report.error("manifest_type", "resources must be an array", MANIFEST_NAME)
        return None
    if len(resources) > limits.max_resource_files:
        report.error("manifest_limit",
                     f"resources exceeds {limits.max_resource_files} entries", MANIFEST_NAME)
        return None

    parsed: list[dict[str, Any]] = []
    seen: dict[str, str] = {}
    failed = False
    for position, item in enumerate(resources):
        where = f"resources[{position}]"
        if type(item) is not dict:
            report.error("manifest_type", f"{where} must be an object", MANIFEST_NAME)
            failed = True
            continue
        if not _require_keys(report, item, where,
                             {"path", "sha256", "license"},
                             {"path", "sha256", "license"}):
            failed = True
            continue
        path = _text(report, item["path"], f"{where}.path", _MAX_TEXT["resource.path"])
        digest = _text(report, item["sha256"], f"{where}.sha256", 64)
        license_name = _text(report, item["license"], f"{where}.license",
                             _MAX_TEXT["resource.license"])
        if None in (path, digest, license_name):
            failed = True
            continue
        problem = _entry_path_problem(path, limits)
        if problem:
            report.error("resource_invalid_path", f"{where}.path is unsafe: {problem}",
                         MANIFEST_NAME)
            failed = True
            continue
        if not path.startswith("assets/") or posixpath.splitext(path)[1].lower() not in _ASSET_SUFFIXES:
            report.error("resource_invalid_path",
                         f"{where}.path must be an asset under assets/", MANIFEST_NAME)
            failed = True
            continue
        if not _SHA256_RE.match(digest):
            report.error("resource_hash_format",
                         f"{where}.sha256 must be 64 lowercase hex characters", MANIFEST_NAME)
            failed = True
            continue
        key = path.casefold()
        if key in seen:
            report.error("resource_duplicate_path",
                         f"{where}.path duplicates {seen[key]!r}", MANIFEST_NAME)
            failed = True
            continue
        seen[key] = path
        parsed.append({"path": path, "sha256": digest, "license": license_name})
    if failed:
        return None
    return parsed


def _validate_manifest(report: PackageReport, manifest: Any,
                       limits: Limits) -> dict[str, Any] | None:
    if type(manifest) is not dict:
        report.error("manifest_type", "manifest root must be an object", MANIFEST_NAME)
        return None
    if not _require_keys(
        report, manifest, "manifest",
        {"schema", "module", "runtime", "parameters", "resources"},
        {"schema", "module", "runtime", "parameters", "resources"},
    ):
        return None
    if manifest["schema"] != SCHEMA:
        report.error("manifest_schema_unsupported",
                     f"schema must be {SCHEMA!r}", MANIFEST_NAME)
        return None
    module = _validate_module(report, manifest["module"])
    if module is None:
        return None
    runtime = _validate_runtime(report, manifest["runtime"], limits)
    if runtime is None:
        return None
    parameters = _validate_parameters(report, manifest["parameters"], limits)
    if parameters is None:
        return None
    resources = _validate_resources(report, manifest["resources"], limits)
    if resources is None:
        return None
    return {"module": module, "runtime": runtime,
            "parameters": parameters, "resources": resources}


def _open_package(path: str) -> tuple[BinaryIO | None, os.stat_result]:
    """Open the package once, read-only, and return its handle and fstat.

    ``O_NONBLOCK`` prevents an accidental FIFO from blocking the validator; a
    non-regular file is rejected before any buffered stream is created.
    """
    flags = (os.O_RDONLY | getattr(os, "O_CLOEXEC", 0)
             | getattr(os, "O_BINARY", 0) | getattr(os, "O_NONBLOCK", 0))
    descriptor = os.open(path, flags)
    try:
        status = os.fstat(descriptor)
    except BaseException:
        os.close(descriptor)
        raise
    if not stat.S_ISREG(status.st_mode):
        os.close(descriptor)
        return None, status
    try:
        return os.fdopen(descriptor, "rb"), status
    except BaseException:
        os.close(descriptor)
        raise


def _hash_package(handle: BinaryIO, limits: Limits, report: PackageReport,
                  declared_size: int) -> bool:
    digest = hashlib.sha256()
    total = 0
    try:
        handle.seek(0)
        while True:
            chunk = handle.read(limits.chunk_size)
            if not chunk:
                break
            total += len(chunk)
            if total > limits.max_package_bytes:
                report.error("package_too_large",
                             f"package exceeds {limits.max_package_bytes} bytes")
                return False
            digest.update(chunk)
    except OSError as exc:
        report.error("package_unreadable", f"{type(exc).__name__}: {exc}")
        return False
    if total != declared_size:
        report.error("package_changed_during_validation",
                     f"file size changed from {declared_size} to {total} bytes "
                     "while it was being read")
        return False
    report.package_size = total
    report.package_sha256 = digest.hexdigest()
    return True


def _stream_entry(zf: zipfile.ZipFile, info: zipfile.ZipInfo, limits: Limits,
                  already_read: int) -> tuple[str | None, int, Issue | None]:
    digest = hashlib.sha256()
    size = 0
    budget = limits.max_total_uncompressed - already_read
    try:
        with zf.open(info, "r") as handle:
            while True:
                want = min(limits.chunk_size, limits.max_entry_uncompressed - size + 1)
                chunk = handle.read(want)
                if not chunk:
                    break
                size += len(chunk)
                if size > limits.max_entry_uncompressed:
                    return None, 0, Issue(
                        "archive_entry_too_large",
                        f"decompressed data exceeds {limits.max_entry_uncompressed} bytes",
                        info.filename)
                if size > budget:
                    return None, 0, Issue(
                        "archive_extracted_too_large",
                        f"total decompressed data exceeds {limits.max_total_uncompressed} bytes",
                        info.filename)
                digest.update(chunk)
    except (zipfile.BadZipFile, zipfile.LargeZipFile, EOFError, OSError,
            RuntimeError, NotImplementedError, zlib.error) as exc:
        return None, 0, Issue("archive_entry_corrupt",
                              f"{type(exc).__name__}: {exc}", info.filename)
    if size != info.file_size:
        return None, 0, Issue(
            "archive_size_mismatch",
            f"declared {info.file_size} bytes, decompressed {size}", info.filename)
    return digest.hexdigest(), size, None


def _read_entry(zf: zipfile.ZipFile, info: zipfile.ZipInfo, max_bytes: int,
                too_large_code: str) -> tuple[bytes | None, Issue | None]:
    try:
        with zf.open(info, "r") as handle:
            data = handle.read(max_bytes + 1)
    except (zipfile.BadZipFile, zipfile.LargeZipFile, EOFError, OSError,
            RuntimeError, NotImplementedError, zlib.error) as exc:
        return None, Issue("archive_entry_corrupt", f"{type(exc).__name__}: {exc}",
                           info.filename)
    if len(data) > max_bytes:
        return None, Issue(too_large_code,
                           f"entry exceeds {max_bytes} bytes", info.filename)
    return data, None


def _check_archive(zf: zipfile.ZipFile, report: PackageReport,
                   limits: Limits) -> tuple[dict[str, zipfile.ZipInfo],
                                            dict[str, str], dict[str, int]] | None:
    infos = zf.infolist()
    if not infos:
        report.error("manifest_missing", "archive contains no entries")
        return None
    if len(infos) > limits.max_entries:
        report.error("archive_entry_count",
                     f"archive exceeds {limits.max_entries} entries")
        return None

    files: dict[str, zipfile.ZipInfo] = {}
    directories: set[str] = set()
    seen: dict[str, str] = {}
    failed = False
    for info in infos:
        raw = info.filename
        is_directory = raw.endswith("/")
        problem = _entry_path_problem(raw, limits)
        if problem:
            report.error("archive_path_unsafe", f"{raw!r}: {problem}", raw)
            failed = True
            continue
        name = raw[:-1] if is_directory else raw
        key = name.casefold()
        previous = seen.get(key)
        if previous is not None and previous != name:
            report.error("archive_path_collision",
                         f"{name!r} collides with {previous!r} after case folding", raw)
            failed = True
            continue
        seen[key] = name
        if info.flag_bits & 0x01:
            report.error("archive_encrypted", "encrypted entries are not supported", raw)
            failed = True
            continue
        if info.compress_type not in _ALLOWED_COMPRESSION:
            report.error("archive_compression",
                         f"compression method {info.compress_type} is not allowed", raw)
            failed = True
            continue
        mode = (info.external_attr >> 16) & 0o170000
        if is_directory:
            if info.file_size != 0 or mode not in (0, stat.S_IFDIR):
                report.error("archive_entry_type",
                             "directory entry has data or a non-directory mode", raw)
                failed = True
                continue
            if name in directories:
                report.error("archive_path_collision", f"duplicate directory {name!r}", raw)
                failed = True
                continue
            directories.add(name)
            continue
        if mode not in (0, stat.S_IFREG):
            report.error("archive_entry_type",
                         f"non-regular entry (mode {oct(mode)})", raw)
            failed = True
            continue
        if name != MANIFEST_NAME:
            suffix = posixpath.splitext(name)[1].lower()
            if suffix not in _ASSET_SUFFIXES and suffix not in _DSP_SUFFIXES:
                report.error("file_forbidden_suffix",
                             f"suffix {suffix or '(none)'} is not allowed in contract v0",
                             raw)
                failed = True
                continue
        if name in files:
            report.error("archive_path_collision", f"duplicate entry {name!r}", raw)
            failed = True
            continue
        if info.file_size > limits.max_entry_uncompressed:
            report.error("archive_entry_too_large",
                         f"declared size exceeds {limits.max_entry_uncompressed} bytes", raw)
            failed = True
            continue
        ratio = info.file_size / max(info.compress_size, 1)
        if info.file_size > _RATIO_MIN_BYTES and ratio > limits.max_compression_ratio:
            report.error("archive_ratio_exceeded",
                         f"compression ratio {ratio:.0f}:1 exceeds "
                         f"{limits.max_compression_ratio}:1", raw)
            failed = True
            continue
        files[name] = info

    for name in list(files):
        parts = name.split("/")
        for position in range(1, len(parts)):
            ancestor = "/".join(parts[:position])
            if ancestor in files:
                report.error("archive_file_directory_conflict",
                             f"{name!r} is nested under file {ancestor!r}", name)
                failed = True
                break
    for name in directories:
        if name in files:
            report.error("archive_file_directory_conflict",
                         f"{name!r} is both a file and a directory", name)
            failed = True
    if failed:
        return None

    total_declared = sum(info.file_size for info in files.values())
    if total_declared > limits.max_total_uncompressed:
        report.error("archive_extracted_too_large",
                     f"declared total exceeds {limits.max_total_uncompressed} bytes")
        return None
    total_compressed = sum(max(info.compress_size, 1) for info in files.values())
    if (total_declared > _TOTAL_RATIO_MIN_BYTES
            and total_declared / total_compressed > limits.max_compression_ratio):
        report.error("archive_ratio_exceeded",
                     f"overall compression ratio exceeds {limits.max_compression_ratio}:1")
        return None

    manifest_info = files.get(MANIFEST_NAME)
    if manifest_info is not None and manifest_info.file_size > limits.max_manifest_bytes:
        report.error("manifest_too_large",
                     f"manifest exceeds {limits.max_manifest_bytes} bytes", MANIFEST_NAME)
        return None
    if manifest_info is None:
        report.error("manifest_missing", f"{MANIFEST_NAME!r} is missing from the root")
        return None

    digests: dict[str, str] = {}
    sizes: dict[str, int] = {}
    consumed = 0
    for name, info in files.items():
        digest, size, issue = _stream_entry(zf, info, limits, consumed)
        if issue is not None:
            report.errors.append(issue)
            return None
        consumed += size
        digests[name] = digest or ""
        sizes[name] = size
        report.inventory.append(InventoryEntry(name, size, digest or ""))
    return files, digests, sizes


def _validate_patch_fields(report: PackageReport, patch: dict[str, Any],
                           patch_path: str, module: dict[str, Any]) -> bool:
    """Allowlist the .cmajorpatch fields permitted by contract v0."""
    failed = False
    for key in sorted(patch.keys() - _PATCH_ALLOWED):
        report.error(
            "patch_field_unknown",
            f"patch field {key!r} is not allowed in contract v0 "
            "(no workers, scripted views or external references)", patch_path)
        failed = True
    for key in sorted(_PATCH_REQUIRED - patch.keys()):
        report.error("patch_field_missing", f"patch.{key} is required", patch_path)
        failed = True
    if failed:
        return False

    if type(patch["CmajorVersion"]) is not int or patch["CmajorVersion"] != 1:
        report.error("patch_format_unsupported",
                     "CmajorVersion must be the integer 1", patch_path)
        return False

    for key, limit in _PATCH_TEXT_LIMITS.items():
        if key not in patch:
            continue
        if _text(report, patch[key], f"patch.{key}", limit, patch_path) is None:
            failed = True
    if failed:
        return False

    is_instrument = patch.get("isInstrument", False)
    if type(is_instrument) is not bool:
        report.error("patch_field_invalid", "patch.isInstrument must be a boolean",
                     patch_path)
        return False
    if is_instrument != (module["kind"] == "instrument"):
        report.error(
            "patch_kind_mismatch",
            f"patch.isInstrument={is_instrument} is inconsistent with "
            f"module.kind={module['kind']!r}", patch_path)
        return False
    return True


def _validate_links(zf: zipfile.ZipFile, files: dict[str, zipfile.ZipInfo],
                    digests: dict[str, str], report: PackageReport,
                    limits: Limits) -> None:
    manifest_bytes, issue = _read_entry(
        zf, files[MANIFEST_NAME], limits.max_manifest_bytes, "manifest_too_large")
    if issue is not None:
        report.errors.append(issue)
        return
    try:
        manifest = parse_json_strict(manifest_bytes or b"")
    except _DuplicateKey:
        report.error("manifest_duplicate_key", "duplicate JSON object key", MANIFEST_NAME)
        return
    except (_NonFiniteNumber, ValueError, UnicodeDecodeError, RecursionError):
        report.error("manifest_json_invalid", "manifest is not strict UTF-8 JSON",
                     MANIFEST_NAME)
        return
    parsed = _validate_manifest(report, manifest, limits)
    if parsed is None:
        return
    report.module = parsed["module"]
    report.parameters = parsed["parameters"]
    report.resources = parsed["resources"]

    patch_path = parsed["runtime"]["patch"]
    patch_info = files.get(patch_path)
    if patch_info is None:
        report.error("patch_missing", "runtime.patch does not exist in the archive",
                     patch_path)
        return
    if patch_info.file_size > limits.max_patch_bytes:
        report.error("patch_too_large",
                     f"patch exceeds {limits.max_patch_bytes} bytes", patch_path)
        return
    patch_bytes, issue = _read_entry(zf, patch_info, limits.max_patch_bytes, "patch_too_large")
    if issue is not None:
        report.errors.append(issue)
        return
    try:
        patch = parse_json_strict(patch_bytes or b"")
    except _DuplicateKey:
        report.error("patch_json_invalid", "duplicate JSON object key", patch_path)
        return
    except (_NonFiniteNumber, ValueError, UnicodeDecodeError, RecursionError):
        report.error("patch_json_invalid", "patch is not strict UTF-8 JSON", patch_path)
        return
    if type(patch) is not dict:
        report.error("patch_json_invalid", "patch root must be an object", patch_path)
        return

    if not _validate_patch_fields(report, patch, patch_path, parsed["module"]):
        return

    internal_id = patch.get("ID")
    internal_version = patch.get("version")
    if type(internal_id) is not str or type(internal_version) is not str:
        report.error("patch_identity_missing", "patch ID and version must be strings",
                     patch_path)
        return
    if internal_id != parsed["module"]["id"]:
        report.error("patch_identity_mismatch",
                     f"patch ID {internal_id!r} differs from module.id "
                     f"{parsed['module']['id']!r}", patch_path)
    if internal_version != parsed["module"]["version"]:
        report.error("patch_identity_mismatch",
                     f"patch version {internal_version!r} differs from module.version "
                     f"{parsed['module']['version']!r}", patch_path)

    sources = patch.get("source")
    if type(sources) is str:
        sources = [sources]
    if (type(sources) is not list or not sources
            or len(sources) > _MAX_PATCH_SOURCES
            or any(type(source) is not str for source in sources)):
        report.error("patch_source_invalid",
                     "source must be a non-empty string or array of strings", patch_path)
        return
    for source in sources:
        problem = _entry_path_problem(source, limits)
        if problem or not source.lower().endswith(".cmajor"):
            report.error("patch_source_invalid",
                         f"source {source!r} is not a safe .cmajor path", patch_path)
            continue
        joined = posixpath.normpath(posixpath.join(posixpath.dirname(patch_path), source))
        if not joined.startswith("patch/"):
            report.error("patch_source_invalid",
                         f"source {source!r} escapes patch/", patch_path)
            continue
        if joined not in files:
            report.error("patch_source_missing",
                         f"source {source!r} does not exist in the archive", joined)

    declared = {resource["path"] for resource in parsed["resources"]}
    for resource in parsed["resources"]:
        path = resource["path"]
        if path not in files:
            report.error("resource_missing",
                         "declared resource is missing from the archive", path)
            continue
        if digests.get(path) != resource["sha256"]:
            report.error("resource_hash_mismatch",
                         "resource bytes do not match the declared sha256", path)

    allowed = {MANIFEST_NAME, patch_path}
    allowed.update(name for name in files
                   if name.startswith("patch/") and name.lower().endswith(".cmajor"))
    allowed.update(declared)
    for name in files:
        if name in allowed:
            continue
        suffix = posixpath.splitext(name)[1].lower()
        if suffix in _ASSET_SUFFIXES:
            if not name.startswith("assets/"):
                report.error("file_unexpected_location",
                             "assets must live under assets/", name)
            else:
                report.error("resource_undeclared",
                             "asset is not declared in resources[]", name)
        elif suffix in _DSP_SUFFIXES:
            report.error("file_unexpected_location",
                         "DSP files must live under patch/ and be linked from runtime.patch",
                         name)
        else:
            report.error("file_forbidden_suffix",
                         f"suffix {suffix or '(none)'} is not allowed in contract v0", name)


def validate_package(path: str | os.PathLike[str], *,
                     limits: Limits | None = None) -> PackageReport:
    """Validate one OpenVST package archive without extracting or executing it.

    The package is opened once, read-only, and that single handle is used for
    the size check, the whole-file sha256, the ZIP signature and the archive
    itself. Replacing the directory entry does not redirect this open handle.
    Device/inode/size/mtime are compared before and after validation to detect
    ordinary in-place mutation instead of accepting a mismatched report.

    This closes the path-reopen race, not hostile mutation by a process with the
    same rights: an installer must consume the verified snapshot and re-check
    its sha256. The returned report is JSON-serialisable via
    ``PackageReport.to_dict()``; ``report.ok`` is True only when no error was
    recorded. Unsafe paths are rejected, never repaired.
    """
    limits = limits or Limits()
    filesystem_path = os.fspath(path)
    report = PackageReport(package_path=os.path.abspath(filesystem_path),
                           limits=asdict(limits))
    try:
        handle, status = _open_package(filesystem_path)
    except FileNotFoundError:
        report.error("package_not_found", "package path does not exist")
        return report
    except OSError as exc:
        report.error("package_unreadable", f"{type(exc).__name__}: {exc}")
        return report
    if handle is None:
        report.error("package_not_file", "package path is not a regular file")
        return report

    with handle:
        if status.st_size > limits.max_package_bytes:
            report.package_size = status.st_size
            report.error("package_too_large",
                         f"package exceeds {limits.max_package_bytes} bytes")
            return report
        snapshot = (status.st_dev, status.st_ino, status.st_size,
                    status.st_mtime_ns)
        if not _hash_package(handle, limits, report, status.st_size):
            return report

        try:
            handle.seek(0)
            magic = handle.read(4)
        except OSError as exc:
            report.error("package_unreadable", f"{type(exc).__name__}: {exc}")
            return report
        if magic not in (b"PK\x03\x04", b"PK\x05\x06"):
            report.error("archive_invalid",
                         "file does not start with a plain ZIP signature")
            return report

        try:
            handle.seek(0)
            archive = zipfile.ZipFile(handle, "r")
        except (zipfile.BadZipFile, zipfile.LargeZipFile, OSError, ValueError) as exc:
            report.error("archive_invalid", f"{type(exc).__name__}: {exc}")
            return report
        try:
            with archive:
                checked = _check_archive(archive, report, limits)
                if checked is None:
                    return report
                files, digests, _sizes = checked
                _validate_links(archive, files, digests, report, limits)
        except Exception as exc:  # defensive: never raise on hostile input
            report.error("validation_error",
                         f"unexpected {type(exc).__name__}: {str(exc)[:200]}")
            return report

        try:
            final = os.fstat(handle.fileno())
        except OSError as exc:
            report.error("package_unreadable", f"{type(exc).__name__}: {exc}")
            return report
        if (final.st_dev, final.st_ino, final.st_size,
                final.st_mtime_ns) != snapshot:
            report.error("package_changed_during_validation",
                         "package file changed while it was being validated")
    return report
