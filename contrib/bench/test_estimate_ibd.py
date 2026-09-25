#!/usr/bin/env python3
# Copyright (c) 2026 The Sugarchain developers
# Distributed under the MIT software license, see the accompanying file COPYING.
"""Deterministic regression checks for sampling, weighting and source safety."""
import importlib.util
import fcntl
import subprocess
import sys
import time
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("estimate_ibd", Path(__file__).with_name("estimate-ibd.py"))
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)


class EstimateTests(unittest.TestCase):
    def test_windows_disjoint_reproducible_and_cover_every_era(self):
        sampled = bench.windows(44564403, 8, 3, 512, 42)
        self.assertEqual(sampled, bench.windows(44564403, 8, 3, 512, 42))
        self.assertEqual(len(sampled), 24)
        intervals = sorted((w["end"] - w["count"] + 1, w["end"]) for w in sampled)
        self.assertTrue(all(b < c for (_, b), (c, _) in zip(intervals, intervals[1:])))
        for s in range(8):
            subset = [w for w in sampled if w["stratum"] == s]
            self.assertEqual(len(subset), 3)
            self.assertTrue(all(44564403 * s // 8 < w["end"] <= 44564403 * (s + 1) // 8 for w in subset))

    def test_weight_seconds_per_block_not_arithmetic_throughput(self):
        rows = [dict(stratum=s, count=10, header_seconds=h, block_seconds=1)
                for s in range(2) for h in (10, 30)]
        result = bench.estimate(rows, 101, 2, 42)
        self.assertAlmostEqual(result["header_seconds"], 202)
        self.assertAlmostEqual(result["block_seconds"], 10.1)
        self.assertAlmostEqual(result["total_seconds"], 212.1)
        self.assertLessEqual(result["sampling_95_percent"]["total"][0], 212.1)
        self.assertGreaterEqual(result["sampling_95_percent"]["total"][1], 212.1)

    def test_incomplete_strata_rejected(self):
        with self.assertRaises(ValueError):
            bench.estimate([dict(stratum=0, count=10, header_seconds=1, block_seconds=1)], 100, 2, 0)
        with self.assertRaises(ValueError):
            bench.windows(10, 8, 3, 512, 0)

    def test_expired_deadline_never_contacts_rpc(self):
        rpc = bench.RPC("http://127.0.0.1:1", "unused:unused", time.monotonic() - 1)
        with self.assertRaises(TimeoutError):
            rpc.batch([("getblockchaininfo", [])])

    def test_live_source_lock_rejected_across_processes(self):
        with tempfile.TemporaryDirectory() as temp:
            with (Path(temp) / "LOCK").open("w") as lock:
                fcntl.lockf(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                script = """
import importlib.util, pathlib, sys
spec = importlib.util.spec_from_file_location('bench', sys.argv[1])
module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
try:
    with module.lock_source_index(pathlib.Path(sys.argv[2])):
        raise SystemExit(1)
except BlockingIOError:
    raise SystemExit(0)
"""
                result = subprocess.run([sys.executable, "-c", script, bench.__file__, temp], timeout=5)
                self.assertEqual(result.returncode, 0)

    def test_snapshot_links_only_immutable_tables(self):
        with tempfile.TemporaryDirectory() as temp:
            source, target = Path(temp) / "source", Path(temp) / "target"
            source.mkdir()
            for name in ("000001.ldb", "CURRENT", "MANIFEST-000001", "000002.log", "LOCK"):
                (source / name).write_text("original")
            bench.snapshot_index(source, target)
            self.assertEqual((source / "000001.ldb").stat().st_ino, (target / "000001.ldb").stat().st_ino)
            for name in ("CURRENT", "MANIFEST-000001", "000002.log"):
                self.assertNotEqual((source / name).stat().st_ino, (target / name).stat().st_ino)
                (target / name).write_text("changed")
                self.assertEqual((source / name).read_text(), "original")
            self.assertFalse((target / "LOCK").exists())


if __name__ == "__main__":
    unittest.main()
