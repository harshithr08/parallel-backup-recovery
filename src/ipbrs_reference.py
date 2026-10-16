#!/usr/bin/env python3
"""Sequential IPBRS reference implementation: backup, verify, damage, recover."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import random
import shutil
import sqlite3
import sys
import tempfile
import time
import uuid
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path, PurePosixPath
from typing import Any

SCHEMA_VERSION = 1
BUFFER_BYTES = 1024 * 1024


@dataclass
class FileResult:
    relative_path: str
    status: str
    bytes_processed: int = 0
    sha256: str | None = None
    error_code: str | None = None
    error: str | None = None


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def canonical(path: Path) -> Path:
    return path.expanduser().resolve(strict=False)


def is_within(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def validate_disjoint(source: Path, backup: Path, catalog: Path) -> None:
    src, dst, cat = canonical(source), canonical(backup), canonical(catalog)
    if not src.exists() or not src.is_dir():
        raise ValueError(f"source root must be an existing directory: {src}")
    if is_within(dst, src) or is_within(src, dst):
        raise ValueError("source and backup roots must be separate, non-overlapping directories")
    if is_within(cat, src) or is_within(cat, dst):
        raise ValueError("catalog file must be outside both source and backup roots")
    if cat.exists() and cat.is_dir():
        raise ValueError("catalog path must be a file")


def safe_relative(raw: str) -> PurePosixPath:
    rel = PurePosixPath(raw.replace("\\", "/"))
    if rel.is_absolute() or not rel.parts or any(part in {"", ".", ".."} for part in rel.parts):
        raise ValueError(f"unsafe relative path: {raw!r}")
    if ":" in rel.parts[0]:
        raise ValueError(f"unsafe drive-qualified path: {raw!r}")
    return rel


def safe_join(root: Path, relative: str) -> Path:
    root = canonical(root)
    rel = safe_relative(relative)
    target = root.joinpath(*rel.parts)
    # Resolve existing symlinks in parents; a catalog path must not escape the root.
    resolved = target.resolve(strict=False)
    if not is_within(resolved, root):
        raise ValueError(f"path escapes root: {relative!r}")
    return target


def sha256_file(path: Path) -> tuple[str, int]:
    digest = hashlib.sha256()
    total = 0
    with path.open("rb") as stream:
        while True:
            block = stream.read(BUFFER_BYTES)
            if not block:
                break
            digest.update(block)
            total += len(block)
    return digest.hexdigest(), total


def discover(root: Path) -> list[dict[str, Any]]:
    root = canonical(root)
    records: list[dict[str, Any]] = []
    def raise_walk_error(error: OSError) -> None:
        raise error

    for current, dirs, files in os.walk(root, followlinks=False, onerror=raise_walk_error):
        current_path = Path(current)
        # Do not traverse directory symlinks. os.walk lists them in dirs.
        dirs[:] = sorted(name for name in dirs if not (current_path / name).is_symlink())
        for name in sorted(files):
            path = current_path / name
            if path.is_symlink() or not path.is_file():
                continue
            rel = path.relative_to(root).as_posix()
            info = path.stat()
            records.append({
                "relative_path": rel,
                "size_bytes": info.st_size,
                "modified_time_ns": info.st_mtime_ns,
                "file_type": path.suffix.lower(),
            })
    records.sort(key=lambda item: item["relative_path"])
    return records


def connect_catalog(path: Path) -> sqlite3.Connection:
    path = canonical(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    db = sqlite3.connect(path)
    db.execute("PRAGMA foreign_keys = ON")
    db.execute("PRAGMA journal_mode = WAL")
    has_backups = db.execute(
        "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'backups'"
    ).fetchone() is not None
    if has_backups:
        try:
            version = db.execute(
                "SELECT value FROM metadata WHERE key = 'schema_version'"
            ).fetchone()
        except sqlite3.Error as exc:
            db.close()
            raise ValueError("existing catalog has no supported schema metadata") from exc
        try:
            supported = version is not None and int(version[0]) == SCHEMA_VERSION
        except (TypeError, ValueError):
            supported = False
        if not supported:
            db.close()
            raise ValueError("existing catalog schema version is missing or unsupported")
    db.executescript("""
        CREATE TABLE IF NOT EXISTS metadata (
            key TEXT PRIMARY KEY,
            value TEXT NOT NULL
        );
        CREATE TABLE IF NOT EXISTS backups (
            backup_id TEXT PRIMARY KEY,
            created_utc TEXT NOT NULL,
            source_root TEXT NOT NULL,
            backup_root TEXT NOT NULL,
            status TEXT NOT NULL,
            schema_version INTEGER NOT NULL,
            error_summary TEXT
        );
        CREATE TABLE IF NOT EXISTS files (
            backup_id TEXT NOT NULL,
            relative_path TEXT NOT NULL,
            backup_relative_path TEXT NOT NULL,
            size_bytes INTEGER NOT NULL,
            source_mtime_ns INTEGER NOT NULL,
            file_type TEXT NOT NULL,
            sha256 TEXT NOT NULL,
            status TEXT NOT NULL,
            PRIMARY KEY (backup_id, relative_path),
            FOREIGN KEY (backup_id) REFERENCES backups(backup_id)
        );
    """)
    db.execute("INSERT OR IGNORE INTO metadata(key, value) VALUES('schema_version', ?)", (str(SCHEMA_VERSION),))
    db.commit()
    return db


def open_catalog_readonly(path: Path) -> sqlite3.Connection:
    path = canonical(path)
    if not path.is_file():
        raise FileNotFoundError(f"catalog does not exist: {path}")
    db = sqlite3.connect(path.as_uri() + "?mode=ro", uri=True)
    db.execute("PRAGMA foreign_keys = ON")
    try:
        version = db.execute(
            "SELECT value FROM metadata WHERE key = 'schema_version'"
        ).fetchone()
        if version is None or int(version[0]) != SCHEMA_VERSION:
            raise ValueError("catalog schema version is missing or unsupported")
    except Exception:
        db.close()
        raise
    return db


def copy_and_hash(source: Path, destination_temp: Path) -> tuple[str, int]:
    digest = hashlib.sha256()
    size = 0
    destination_temp.parent.mkdir(parents=True, exist_ok=True)
    with source.open("rb") as src, destination_temp.open("xb") as dst:
        while True:
            block = src.read(BUFFER_BYTES)
            if not block:
                break
            dst.write(block)
            digest.update(block)
            size += len(block)
        dst.flush()
        os.fsync(dst.fileno())
    return digest.hexdigest(), size


def backup(source_root: Path, backup_root: Path, catalog_path: Path) -> dict[str, Any]:
    validate_disjoint(source_root, backup_root, catalog_path)
    source_root, backup_root, catalog_path = canonical(source_root), canonical(backup_root), canonical(catalog_path)
    backup_root.mkdir(parents=True, exist_ok=True)
    records = discover(source_root)
    backup_id = str(uuid.uuid4())
    results: list[FileResult] = []
    candidates: list[dict[str, Any]] = []
    db = connect_catalog(catalog_path)
    db.execute("INSERT INTO backups VALUES (?, ?, ?, ?, 'IN_PROGRESS', ?, NULL)",
               (backup_id, utc_now(), str(source_root), str(backup_root), SCHEMA_VERSION))
    db.commit()
    started = time.perf_counter()
    try:
        for record in records:
            rel = record["relative_path"]
            source = safe_join(source_root, rel)
            backup_rel = f"{backup_id}/{rel}"
            destination = safe_join(backup_root, backup_rel)
            temp = destination.with_name(destination.name + f".ipbrs-{backup_id}.tmp")
            try:
                before = source.stat()
                if before.st_size != record["size_bytes"] or before.st_mtime_ns != record["modified_time_ns"]:
                    raise RuntimeError("source changed after discovery and before backup")
                source_digest, copied_size = copy_and_hash(source, temp)
                after = source.stat()
                if before.st_size != after.st_size or before.st_mtime_ns != after.st_mtime_ns:
                    raise RuntimeError("source changed while being backed up")
                if copied_size != before.st_size:
                    raise RuntimeError("copied byte count differs from source size")
                os.utime(temp, ns=(temp.stat().st_atime_ns, before.st_mtime_ns))
                if temp.stat().st_mtime_ns != before.st_mtime_ns:
                    raise RuntimeError("backup copy could not preserve source modification time")
                backup_digest, backup_size = sha256_file(temp)
                if backup_size != copied_size or backup_digest != source_digest:
                    raise RuntimeError("backup verification failed")
                destination.parent.mkdir(parents=True, exist_ok=True)
                os.replace(temp, destination)
                # Independently verify the final named output before cataloging it.
                final_digest, final_size = sha256_file(destination)
                if final_size != before.st_size or final_digest != source_digest:
                    raise RuntimeError("final backup verification failed")
                candidates.append({
                    **record,
                    "backup_relative_path": backup_rel,
                    "sha256": source_digest,
                    "status": "COMPLETE",
                })
                results.append(FileResult(rel, "SUCCESS", copied_size, source_digest))
            except Exception as exc:
                try:
                    if temp.exists():
                        temp.unlink()
                except OSError:
                    pass
                results.append(FileResult(rel, "FAILED", error_code="BACKUP_IO_OR_VERIFY", error=str(exc)))

        failed = [r for r in results if r.status != "SUCCESS"]
        with db:
            for item in candidates:
                db.execute("""INSERT INTO files VALUES (?, ?, ?, ?, ?, ?, ?, ?)""",
                           (backup_id, item["relative_path"], item["backup_relative_path"], item["size_bytes"],
                            item["modified_time_ns"], item["file_type"], item["sha256"], item["status"]))
            db.execute("UPDATE backups SET status = ?, error_summary = ? WHERE backup_id = ?",
                       ("FAILED" if failed else "COMPLETE",
                        f"{len(failed)} file(s) failed" if failed else None, backup_id))
        elapsed = time.perf_counter() - started
        return {
            "operation": "backup", "backup_id": backup_id,
            "status": "FAILED" if failed else "COMPLETE",
            "files_discovered": len(records), "files_succeeded": len(candidates),
            "files_failed": len(failed), "bytes_backed_up": sum(x["size_bytes"] for x in candidates),
            "elapsed_seconds": elapsed, "results": [asdict(r) for r in results],
        }
    finally:
        db.close()


def load_backup(db: sqlite3.Connection, backup_id: str) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    row = db.execute("SELECT backup_id, source_root, backup_root, status, schema_version FROM backups WHERE backup_id = ?",
                     (backup_id,)).fetchone()
    if row is None:
        raise ValueError(f"backup ID not found: {backup_id}")
    item = {"backup_id": row[0], "source_root": row[1], "backup_root": row[2], "status": row[3], "schema_version": row[4]}
    if item["schema_version"] != SCHEMA_VERSION:
        raise ValueError(f"unsupported catalog schema version: {item['schema_version']}")
    if item["status"] != "COMPLETE":
        raise ValueError(f"backup is not complete (status={item['status']})")
    rows = db.execute("""SELECT relative_path, backup_relative_path, size_bytes, source_mtime_ns,
                               file_type, sha256, status FROM files WHERE backup_id = ? ORDER BY relative_path""",
                      (backup_id,)).fetchall()
    files = [{"relative_path": r[0], "backup_relative_path": r[1], "size_bytes": r[2],
              "source_mtime_ns": r[3], "file_type": r[4], "sha256": r[5], "status": r[6]} for r in rows]
    return item, files


def verify_backup(backup_root: Path, catalog_path: Path, backup_id: str) -> dict[str, Any]:
    backup_root = canonical(backup_root)
    db = open_catalog_readonly(catalog_path)
    try:
        backup_record, entries = load_backup(db, backup_id)
        if canonical(Path(backup_record["backup_root"])) != backup_root:
            raise ValueError("backup root does not match the root recorded in the catalog")
        results = []
        for item in entries:
            path = safe_join(backup_root, item["backup_relative_path"])
            try:
                digest, size = sha256_file(path)
                if size != item["size_bytes"] or digest != item["sha256"]:
                    results.append(FileResult(item["relative_path"], "FAILED", size, digest,
                                              "DIGEST_OR_SIZE_MISMATCH"))
                elif path.stat().st_mtime_ns != item["source_mtime_ns"]:
                    results.append(FileResult(item["relative_path"], "FAILED", size, digest,
                                              "TIMESTAMP_MISMATCH",
                                              "backup file modification time differs from catalog"))
                else:
                    results.append(FileResult(item["relative_path"], "SUCCESS", size, digest))
            except OSError as exc:
                results.append(FileResult(item["relative_path"], "FAILED", error_code="VERIFY_IO", error=str(exc)))
        failures = [r for r in results if r.status != "SUCCESS"]
        return {"operation": "verify", "backup_id": backup_id,
                "status": "FAILED" if failures else "COMPLETE", "files_verified": len(results),
                "files_failed": len(failures), "results": [asdict(r) for r in results]}
    finally:
        db.close()


def recover(source_root: Path, backup_root: Path, catalog_path: Path, backup_id: str,
            full: bool = False) -> dict[str, Any]:
    source_root, backup_root, catalog_path = canonical(source_root), canonical(backup_root), canonical(catalog_path)
    if not source_root.exists() or not source_root.is_dir():
        raise ValueError(f"source root must exist: {source_root}")
    if is_within(source_root, backup_root) or is_within(backup_root, source_root):
        raise ValueError("source and backup roots must be separate, non-overlapping directories")
    if is_within(catalog_path, source_root) or is_within(catalog_path, backup_root):
        raise ValueError("catalog must be outside source and backup roots")
    db = open_catalog_readonly(catalog_path)
    results: list[FileResult] = []
    try:
        backup_record, entries = load_backup(db, backup_id)
        if canonical(Path(backup_record["source_root"])) != source_root:
            raise ValueError("source root does not match the root recorded in the catalog")
        if canonical(Path(backup_record["backup_root"])) != backup_root:
            raise ValueError("backup root does not match the root recorded in the catalog")
        to_restore: list[dict[str, Any]] = []
        unchanged = 0
        for item in entries:
            current = safe_join(source_root, item["relative_path"])
            if full:
                to_restore.append(item)
                continue
            try:
                if not current.exists():
                    to_restore.append(item)
                    continue
                if current.stat().st_mtime_ns != item["source_mtime_ns"]:
                    to_restore.append(item)
                    continue
                digest, size = sha256_file(current)
                if size != item["size_bytes"] or digest != item["sha256"]:
                    to_restore.append(item)
                else:
                    unchanged += 1
                    results.append(FileResult(item["relative_path"], "SKIPPED", size, digest))
            except OSError:
                to_restore.append(item)

        for item in to_restore:
            rel = item["relative_path"]
            source = safe_join(backup_root, item["backup_relative_path"])
            destination = safe_join(source_root, rel)
            temp = destination.with_name(destination.name + f".ipbrs-{uuid.uuid4().hex}.restore-tmp")
            try:
                src_digest, src_size = sha256_file(source)
                if src_size != item["size_bytes"] or src_digest != item["sha256"]:
                    raise RuntimeError("backup copy failed pre-restore verification")
                digest, size = copy_and_hash(source, temp)
                if size != item["size_bytes"] or digest != item["sha256"]:
                    raise RuntimeError("temporary restore failed verification")
                os.utime(temp, ns=(temp.stat().st_atime_ns, item["source_mtime_ns"]))
                if temp.stat().st_mtime_ns != item["source_mtime_ns"]:
                    raise RuntimeError("temporary restore could not preserve catalog modification time")
                destination.parent.mkdir(parents=True, exist_ok=True)
                os.replace(temp, destination)
                final_digest, final_size = sha256_file(destination)
                if final_size != item["size_bytes"] or final_digest != item["sha256"]:
                    raise RuntimeError("final restored file failed verification")
                if destination.stat().st_mtime_ns != item["source_mtime_ns"]:
                    raise RuntimeError("final restored file modification time differs from catalog")
                results.append(FileResult(rel, "SUCCESS", final_size, final_digest))
            except Exception as exc:
                try:
                    if temp.exists():
                        temp.unlink()
                except OSError:
                    pass
                results.append(FileResult(rel, "FAILED", error_code="RESTORE_IO_OR_VERIFY", error=str(exc)))
        failures = [r for r in results if r.status == "FAILED"]
        successful = [r for r in results if r.status == "SUCCESS"]
        bytes_written = sum(r.bytes_processed for r in successful)
        bytes_total = sum(i["size_bytes"] for i in entries)
        bytes_avoided = sum(i["size_bytes"] for i in entries if i not in to_restore)
        return {
            "operation": "recover", "backup_id": backup_id,
            "status": "COMPLETE_WITH_FAILURES" if failures else "COMPLETE",
            "mode": "full" if full else "selective",
            "catalog_files": len(entries), "unchanged_skipped": unchanged,
            "restore_tasks": len(to_restore), "files_restored": len(successful),
            "files_failed": len(failures), "bytes_restored": bytes_written,
            "files_avoided": len(entries) - len(to_restore), "bytes_avoided": bytes_avoided,
            "catalog_bytes": bytes_total, "results": [asdict(r) for r in results],
        }
    finally:
        db.close()


def inject_damage(manifest_path: Path, ratio: float, damage_types: list[str], seed: int) -> dict[str, Any]:
    manifest_path = canonical(manifest_path)
    manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
    root = canonical(manifest_path.parent / manifest["dataset_root_name"])
    if not root.is_dir():
        raise ValueError(f"dataset root from manifest does not exist: {root}")
    files = manifest.get("files")
    if not isinstance(files, list) or not files:
        raise ValueError("manifest contains no files")
    allowed = {"missing", "modified", "corrupted", "truncated"}
    if not damage_types or any(kind not in allowed for kind in damage_types):
        raise ValueError(f"damage types must be selected from {sorted(allowed)}")
    if not 0 <= ratio <= 1:
        raise ValueError("ratio must be between 0 and 1")
    count = int(len(files) * ratio)
    rng = random.Random(seed)
    chosen = rng.sample(files, count) if count else []
    log_path = manifest_path.with_name(manifest_path.stem + f".damage-{seed}.json")
    assignments = [(item, damage_types[index % len(damage_types)]) for index, item in enumerate(chosen)]
    # Validate the whole plan before modifying anything. A zero-byte file cannot
    # meaningfully be truncated, so reject that plan without partial damage.
    for item, kind in assignments:
        rel = safe_relative(item["relative_path"]).as_posix()
        path = safe_join(root, rel)
        if kind in {"truncated", "corrupted"} and path.stat().st_size == 0:
            raise ValueError(f"cannot apply {kind} damage to a zero-byte file: {rel}")

    result = {"schema_version": 1, "seed": seed, "requested_ratio": ratio,
              "eligible_files": len(files), "damaged_files": 0,
              "status": "IN_PROGRESS", "changes": []}

    def save_log(initial: bool = False) -> None:
        if initial:
            with log_path.open("x", encoding="utf-8") as stream:
                json.dump(result, stream, indent=2)
            return
        temp_log = log_path.with_name(log_path.name + f".{os.getpid()}.tmp")
        temp_log.write_text(json.dumps(result, indent=2), encoding="utf-8")
        os.replace(temp_log, log_path)

    save_log(initial=True)
    try:
        for item, kind in assignments:
            rel = safe_relative(item["relative_path"]).as_posix()
            path = safe_join(root, rel)
            before_size = path.stat().st_size
            before_digest, _ = sha256_file(path)
            if kind == "missing":
                path.unlink()
            elif kind == "truncated":
                with path.open("r+b") as stream:
                    stream.truncate(before_size // 2)
            elif kind == "modified":
                with path.open("ab") as stream:
                    stream.write(b"IPBRS-MODIFIED")
            else:
                if before_size == 0:
                    raise ValueError(f"cannot corrupt zero-byte file without changing its size: {rel}")
                offset = rng.randrange(before_size)
                with path.open("r+b") as stream:
                    stream.seek(offset)
                    original = stream.read(1)
                    stream.seek(offset)
                    stream.write(bytes([original[0] ^ 0xFF]))
            result["changes"].append({
                "relative_path": rel,
                "damage_type": kind,
                "original_size_bytes": before_size,
                "original_sha256": before_digest,
            })
            result["damaged_files"] = len(result["changes"])
            save_log()
        result["status"] = "COMPLETE"
        save_log()
    except Exception as exc:
        result["status"] = "FAILED"
        result["error"] = str(exc)
        save_log()
        raise
    return {"status": result["status"], "damage_log": str(log_path),
            "damaged_files": result["damaged_files"], "eligible_files": len(files)}


def emit(result: dict[str, Any], output: Path | None) -> None:
    encoded = json.dumps(result, indent=2)
    if output:
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(encoded, encoding="utf-8")
    print(encoded)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    p_backup = commands.add_parser("backup")
    p_backup.add_argument("--source", required=True, type=Path)
    p_backup.add_argument("--backup", required=True, type=Path)
    p_backup.add_argument("--catalog", required=True, type=Path)
    p_backup.add_argument("--result", type=Path)
    p_verify = commands.add_parser("verify")
    p_verify.add_argument("--backup", required=True, type=Path)
    p_verify.add_argument("--catalog", required=True, type=Path)
    p_verify.add_argument("--backup-id", required=True)
    p_verify.add_argument("--result", type=Path)
    p_recover = commands.add_parser("recover")
    p_recover.add_argument("--source", required=True, type=Path)
    p_recover.add_argument("--backup", required=True, type=Path)
    p_recover.add_argument("--catalog", required=True, type=Path)
    p_recover.add_argument("--backup-id", required=True)
    p_recover.add_argument("--full", action="store_true", help="restore every catalog entry")
    p_recover.add_argument("--result", type=Path)
    p_damage = commands.add_parser("damage")
    p_damage.add_argument("--manifest", required=True, type=Path)
    p_damage.add_argument("--ratio", required=True, type=float)
    p_damage.add_argument("--types", default="missing,modified,corrupted,truncated")
    p_damage.add_argument("--seed", required=True, type=int)

    args = parser.parse_args()
    try:
        if args.command == "backup":
            result = backup(args.source, args.backup, args.catalog)
            emit(result, args.result)
            return 0 if result["status"] == "COMPLETE" else 1
        if args.command == "verify":
            result = verify_backup(args.backup, args.catalog, args.backup_id)
            emit(result, args.result)
            return 0 if result["status"] == "COMPLETE" else 1
        if args.command == "recover":
            result = recover(args.source, args.backup, args.catalog, args.backup_id, args.full)
            emit(result, args.result)
            return 0 if result["status"] == "COMPLETE" else 1
        if args.command == "damage":
            result = inject_damage(args.manifest, args.ratio, [x.strip() for x in args.types.split(",") if x.strip()], args.seed)
            emit(result, None)
            return 0
    except (ValueError, OSError, sqlite3.Error, json.JSONDecodeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    return 2


if __name__ == "__main__":
    raise SystemExit(main())




