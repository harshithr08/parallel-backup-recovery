"""Regression tests for the Python IPBRS reference and experiment tooling.

Run with: python -m unittest discover -s tests -v
"""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import tempfile
import unittest
import uuid
from contextlib import contextmanager
from pathlib import Path

from src import benchmark, dataset_generator, experiment_matrix, ipbrs_reference


@contextmanager
def temporary_workspace():
    """Create a disposable directory with the workspace's normal Windows ACLs."""
    parent = Path(__file__).resolve().parent
    for _ in range(100):
        root = parent / f".test-work-{uuid.uuid4().hex}"
        try:
            root.mkdir()
            break
        except FileExistsError:
            continue
    else:
        raise RuntimeError("could not allocate a test workspace")
    try:
        yield root
    finally:
        shutil.rmtree(root)


def small_dataset_config(seed: int = 7, file_count: int = 8) -> dict:
    return {
        "seed": seed,
        "file_count": file_count,
        "files_per_directory": 4,
        "duplicate_ratio": 0.0,
        "heterogeneity": "custom",
        "file_types": [{"name": "binary", "extension": ".bin", "weight": 1.0}],
        "size_profiles": [{"name": "small", "min_bytes": 16, "max_bytes": 32, "weight": 1.0}],
    }


class DatasetGeneratorTests(unittest.TestCase):
    def test_same_seed_reproduces_file_paths_sizes_and_digests(self) -> None:
        with temporary_workspace() as root:
            first_manifest = dataset_generator.generate(
                small_dataset_config(), root / "first")
            second_manifest = dataset_generator.generate(
                small_dataset_config(), root / "second")

            first = json.loads(first_manifest.read_text(encoding="utf-8"))
            second = json.loads(second_manifest.read_text(encoding="utf-8"))
            stable_fields = lambda document: [
                (item["relative_path"], item["size_bytes"], item["sha256"], item["duplicate_of"])
                for item in document["files"]
            ]
            self.assertEqual(stable_fields(first), stable_fields(second))
            self.assertEqual(first["file_count"], 8)


class ReferencePipelineTests(unittest.TestCase):
    def test_backup_verify_selective_recovery_and_extra_file_behavior(self) -> None:
        with temporary_workspace() as root:
            source = root / "source"
            source.mkdir()
            (source / "keep.bin").write_bytes(b"healthy")
            (source / "change.bin").write_bytes(b"original bytes")
            (source / "remove.bin").write_bytes(b"restore me")
            backup_root = root / "backups"
            catalog = root / "catalog.sqlite"

            created = ipbrs_reference.backup(source, backup_root, catalog)
            self.assertEqual(created["status"], "COMPLETE")
            verified = ipbrs_reference.verify_backup(
                backup_root, catalog, created["backup_id"])
            self.assertEqual(verified["status"], "COMPLETE")

            (source / "change.bin").write_bytes(b"changed")
            (source / "remove.bin").unlink()
            (source / "extra.bin").write_bytes(b"leave this alone")
            recovered = ipbrs_reference.recover(
                source, backup_root, catalog, created["backup_id"])

            self.assertEqual(recovered["status"], "COMPLETE")
            self.assertEqual(recovered["files_restored"], 2)
            self.assertEqual(recovered["unchanged_skipped"], 1)
            self.assertEqual((source / "change.bin").read_bytes(), b"original bytes")
            self.assertEqual((source / "remove.bin").read_bytes(), b"restore me")
            self.assertEqual((source / "extra.bin").read_bytes(), b"leave this alone")

    def test_python_verifier_detects_timestamp_only_mismatch(self) -> None:
        with temporary_workspace() as root:
            source = root / "source"
            source.mkdir()
            (source / "file.bin").write_bytes(b"unchanged content")
            backup_root = root / "backups"
            catalog = root / "catalog.sqlite"
            created = ipbrs_reference.backup(source, backup_root, catalog)
            backed_up_file = backup_root / created["backup_id"] / "file.bin"
            current = backed_up_file.stat()
            os.utime(backed_up_file,
                     ns=(current.st_atime_ns, current.st_mtime_ns + 2_000_000_000))

            verified = ipbrs_reference.verify_backup(
                backup_root, catalog, created["backup_id"])

            self.assertEqual(verified["status"], "FAILED")
            self.assertEqual(verified["files_failed"], 1)
            self.assertEqual(verified["results"][0]["error_code"], "TIMESTAMP_MISMATCH")

    def test_damage_injection_records_each_requested_damage_kind(self) -> None:
        with temporary_workspace() as root:
            manifest = dataset_generator.generate(
                small_dataset_config(file_count=8), root / "dataset")
            damage_types = ["missing", "modified", "corrupted", "truncated"]

            result = ipbrs_reference.inject_damage(
                manifest, 0.5, damage_types, seed=19)

            log = json.loads(Path(result["damage_log"]).read_text(encoding="utf-8"))
            self.assertEqual(result["status"], "COMPLETE")
            self.assertEqual(log["damaged_files"], 4)
            self.assertEqual(
                [change["damage_type"] for change in log["changes"]], damage_types)

    def test_zero_byte_truncation_is_rejected_without_mutating_source(self) -> None:
        with temporary_workspace() as root:
            config = {
                "seed": 1,
                "file_count": 1,
                "files_per_directory": 1,
                "duplicate_ratio": 0,
                "heterogeneity": "custom",
                "file_types": [{"name": "empty", "extension": ".bin", "weight": 1.0}],
                "size_profiles": [{"name": "empty", "min_bytes": 0, "max_bytes": 0, "weight": 1.0}],
            }
            manifest = dataset_generator.generate(config, root / "empty-dataset")
            target = root / "empty-dataset" / "dir_00000" / "file_00000000.bin"

            with self.assertRaisesRegex(ValueError, "zero-byte file"):
                ipbrs_reference.inject_damage(manifest, 1.0, ["truncated"], seed=5)

            self.assertEqual(target.read_bytes(), b"")
            self.assertFalse((root / "empty-dataset.damage-5.json").exists())

    def test_unsafe_catalog_relative_paths_are_rejected(self) -> None:
        with self.assertRaisesRegex(ValueError, "unsafe relative path"):
            ipbrs_reference.safe_relative("../outside.bin")


class ExperimentToolTests(unittest.TestCase):
    def test_benchmark_config_accepts_valid_backup_and_normalizes_defaults(self) -> None:
        with temporary_workspace() as root:
            source = root / "source"
            source.mkdir()
            engine = root / "engine.exe"
            engine.write_bytes(b"placeholder for config validation")
            config_path = root / "benchmark.json"
            config_path.write_text(json.dumps({
                "schema_version": 1,
                "engine": str(engine),
                "runs": [{
                    "id": "backup-1",
                    "operation": "backup",
                    "source": str(source),
                    "backup": str(root / "backup"),
                }],
            }), encoding="utf-8")

            normalized = benchmark.validate_config(config_path)

            self.assertEqual(normalized["runs"][0]["policy"], "sequential")
            self.assertEqual(normalized["runs"][0]["workers"], 1)
            self.assertEqual(normalized["runs"][0]["operation"], "backup")

    def test_benchmark_config_rejects_duplicate_run_ids(self) -> None:
        with temporary_workspace() as root:
            source = root / "source"
            source.mkdir()
            engine = root / "engine.exe"
            engine.write_bytes(b"placeholder")
            run = {"id": "same", "operation": "backup", "source": str(source),
                   "backup": str(root / "backup")}
            config_path = root / "benchmark.json"
            config_path.write_text(json.dumps({
                "schema_version": 1,
                "engine": str(engine),
                "runs": [run, {**run, "backup": str(root / "backup-2")}],
            }), encoding="utf-8")

            with self.assertRaisesRegex(ValueError, "unique and non-empty"):
                benchmark.validate_config(config_path)

    def test_scheduler_matrix_has_expected_policy_and_chunk_coverage(self) -> None:
        protocol = {
            "workers": [1, 2, 4, 8],
            "fixed_chunk_sizes": [1, 4, 16, 64],
            "adaptive_start_chunks": [1, 4, 16, 64],
            "adaptive_min_chunk": 1,
            "adaptive_max_chunk": 64,
        }
        settings = experiment_matrix._matrix_settings(protocol)
        counts = {policy: sum(setting[0] == policy for setting in settings)
                  for policy in experiment_matrix.POLICIES}

        self.assertEqual(len(settings), 53)
        self.assertEqual(counts, {
            "sequential": 1,
            "static": 4,
            "dynamic": 16,
            "stealing": 16,
            "adaptive": 16,
        })

    @unittest.skipUnless(os.name == "nt", "C++ backup executable is Windows-only")
    def test_adaptive_scheduler_honors_configured_start_chunk(self) -> None:
        engine = Path(os.environ.get(
            "IPBRS_TEST_ENGINE",
            Path(__file__).resolve().parents[1] / "build" / "ipbrs-backup.exe"))
        if not engine.is_file():
            self.skipTest("build the C++ engine to check adaptive chunk behavior")
        with temporary_workspace() as root:
            source = root / "source"
            source.mkdir()
            for index in range(12):
                (source / f"file-{index:02}.bin").write_bytes(bytes([index]) * (index + 1))
            completed = subprocess.run(
                [str(engine), "backup", "--source", str(source), "--backup", str(root / "backups"),
                 "--policy", "adaptive", "--threads", "2", "--chunk", "8",
                 "--min-chunk", "1", "--max-chunk", "8"],
                capture_output=True, text=True, check=False)

            self.assertEqual(completed.returncode, 0, completed.stderr)
            result = json.loads(completed.stdout)
            self.assertEqual(result["initial_adaptive_chunk"], 8)


@unittest.skipUnless(os.name == "nt", "C++ backup executable is Windows-only")
class CppVerifierTests(unittest.TestCase):
    def test_cpp_verifier_detects_timestamp_only_mismatch(self) -> None:
        engine = Path(os.environ.get(
            "IPBRS_TEST_ENGINE",
            Path(__file__).resolve().parents[1] / "build" / "ipbrs-backup.exe"))
        if not engine.is_file():
            self.skipTest("build the C++ engine to run its verifier integration test")
        with temporary_workspace() as root:
            source = root / "source"
            source.mkdir()
            (source / "file.bin").write_bytes(b"unchanged content")
            backup_root = root / "backups"
            backup_root.mkdir()
            backed_up = subprocess.run(
                [str(engine), "backup", "--source", str(source), "--backup", str(backup_root),
                 "--policy", "sequential", "--threads", "1"],
                capture_output=True, text=True, check=False)
            self.assertEqual(backed_up.returncode, 0, backed_up.stderr)
            backup_result = json.loads(backed_up.stdout)
            self.assertTrue(backup_result["success"])
            # Schema 3 keeps payloads below files/ to separate them from catalog metadata.
            backup_file = Path(backup_result["snapshot_path"]) / "files" / "file.bin"
            current = backup_file.stat()
            os.utime(backup_file,
                     ns=(current.st_atime_ns, current.st_mtime_ns + 2_000_000_000))

            checked = subprocess.run(
                [str(engine), "verify", "--backup", str(backup_root),
                 "--catalog", backup_result["catalog_path"],
                 "--policy", "sequential", "--threads", "1"],
                capture_output=True, text=True, check=False)
            verification = json.loads(checked.stdout)

            self.assertNotEqual(checked.returncode, 0)
            self.assertFalse(verification["success"])
            self.assertEqual(verification["files_timestamp_mismatch"], 1)


if __name__ == "__main__":
    unittest.main()
