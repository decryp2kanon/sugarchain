#!/usr/bin/env python3
# Copyright (c) 2026 The Sugarchain developers
# Distributed under the MIT software license, see the accompanying file COPYING.
"""Bounded, stratified historical IBD processing benchmark (standard library only)."""
import argparse
import base64
import contextlib
import fcntl
import hashlib
import json
import os
from pathlib import Path
import random
import re
import shutil
import statistics
import subprocess
import tempfile
import time
import urllib.request


def windows(height, strata, repeats, count, seed):
    rng = random.Random(seed)
    result = []
    for group in range(strata):
        low, high = 1 + height * group // strata, height * (group + 1) // strata
        if high - low + 1 < count * repeats:
            raise ValueError("height too small for disjoint sampling windows")
        # Independently randomized windows in equal sub-strata spread each era's
        # observations in height, rather than repeatedly timing the same blocks.
        for repeat in range(repeats):
            a = low + (high - low + 1) * repeat // repeats
            b = low + (high - low + 1) * (repeat + 1) // repeats - 1
            start = rng.randint(a, b - count + 1)
            result.append(dict(stratum=group, end=start + count - 1, count=count))
    rng.shuffle(result)  # separate chain era from thermal/load/time-of-run effects
    return result


def estimate(rows, height, strata, seed):
    groups = [[r for r in rows if r["stratum"] == s] for s in range(strata)]
    if any(len(g) < 2 for g in groups):
        raise ValueError("at least two complete windows per stratum required")
    weights = [height * (s + 1) // strata - height * s // strata for s in range(strata)]
    def total(sample, key):
        return sum(w * statistics.mean(r[key] / r["count"] for r in g)
                   for w, g in zip(weights, sample))
    header = total(groups, "header_seconds")
    block = total(groups, "block_seconds")
    rng = random.Random(seed)
    draws = []
    for _ in range(4000):
        sample = [[rng.choice(g) for _ in g] for g in groups]
        h, b = total(sample, "header_seconds"), total(sample, "block_seconds")
        draws.append((h, b, h + b))
    def interval(column):
        values = sorted(d[column] for d in draws)
        return [values[int(len(values) * .025)], values[int(len(values) * .975)]]
    return dict(header_seconds=header, block_seconds=block, total_seconds=header + block,
                sampling_95_percent=dict(header=interval(0), block=interval(1), total=interval(2)),
                ideal_overlap_seconds=max(header, block))


class RPC:
    def __init__(self, url, credentials, deadline):
        self.url, self.deadline = url, deadline
        self.auth = "Basic " + base64.b64encode(credentials.encode()).decode()

    def batch(self, calls):
        result = []
        for offset in range(0, len(calls), 128):
            chunk = calls[offset:offset + 128]
            data = [{"jsonrpc": "1.0", "id": i, "method": method, "params": params}
                    for i, (method, params) in enumerate(chunk)]
            request = urllib.request.Request(self.url, json.dumps(data).encode(),
                                             {"Authorization": self.auth, "Content-Type": "application/json"})
            remaining = self.deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("benchmark deadline exceeded during RPC preparation")
            with urllib.request.urlopen(request, timeout=min(30, remaining)) as response:
                replies = json.load(response)
            by_id = {r["id"]: r for r in replies}
            for i in range(len(chunk)):
                if by_id[i]["error"]:
                    raise RuntimeError(str(by_id[i]["error"]))
                result.append(by_id[i]["result"])
        return result


@contextlib.contextmanager
def lock_source_index(source):
    # Match LevelDB's POSIX record lock, not flock. Holding this lock also
    # prevents the source node starting during fixture preparation/measurement.
    with (source / "LOCK").open("rb") as lock:
        fcntl.lockf(lock, fcntl.LOCK_SH | fcntl.LOCK_NB)
        yield


def snapshot_index(source, target):
    """Immutable table hardlinks; mutable metadata copies, under source DB lock."""
    target.mkdir()
    for path in source.iterdir():
        if path.name == "LOCK" or not path.is_file():
            continue
        if path.suffix in (".ldb", ".sst"):
            # Do not silently copy many GB and exceed the runtime budget. The
            # work directory must be on the same filesystem as the source.
            os.link(path, target / path.name)
        else:
            shutil.copy2(path, target / path.name)


def source_tip(datadir):
    with (datadir / "debug.log").open("rb") as log:
        log.seek(0, 2)
        log.seek(max(0, log.tell() - 2 * 1024 * 1024))
        matches = re.findall(rb"UpdateTip: new best=([0-9a-f]{64}) height=(\d+)", log.read())
    if not matches:
        raise ValueError("cannot discover snapshot tip; supply --height")
    return int(matches[-1][1])


def main():
    root = Path(__file__).resolve().parents[2]
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--source-datadir", required=True, type=Path, help="stopped, unpruned mainnet node with undo")
    p.add_argument("--rpc-url", default="http://127.0.0.1:34229", help="read-only height/hash lookup on a synced mainnet node")
    p.add_argument("--rpc-cookie", type=Path)
    p.add_argument("--rpc-user")
    p.add_argument("--rpc-password-env", default="IBD_RPC_PASSWORD")
    p.add_argument("--binary", type=Path, default=root / "src/bench/ibd_estimate")
    p.add_argument("--work-dir", type=Path, help="same filesystem as source; defaults to source parent")
    p.add_argument("--height", type=int, help="fixed target height present in the snapshot (default: last logged tip)")
    p.add_argument("--seconds", type=float, default=570, help="total wall budget including preparation (default 570, max 600)")
    p.add_argument("--strata", type=int, default=8)
    p.add_argument("--repeats", type=int, default=5)
    p.add_argument("--window", type=int, default=2000)
    p.add_argument("--seed", type=int, default=20260926)
    p.add_argument("--output", required=True, type=Path)
    args = p.parse_args()
    if not (0 < args.seconds <= 600 and args.strata >= 2 and args.repeats >= 2 and 1 <= args.window <= 2000):
        p.error("require seconds in (0,600], strata/repeats >= 2, window in [1,2000]")
    started = time.monotonic()
    deadline = started + args.seconds - min(5, args.seconds / 10)  # reserve report/cleanup time
    args.source_datadir = args.source_datadir.resolve()
    args.binary = args.binary.resolve()
    report = {"status": "incomplete", "fast_ibd": 0, "seed": args.seed, "samples": []}
    try:
        height = args.height if args.height is not None else source_tip(args.source_datadir)
        if height <= 0:
            raise ValueError("height must be positive")
        report.update(target_height=height, strata=args.strata, repeats=args.repeats,
                      window=args.window, binary_sha256=hashlib.sha256(args.binary.read_bytes()).hexdigest())
        if args.rpc_cookie:
            credentials = args.rpc_cookie.read_text().strip()
        else:
            if not args.rpc_user or args.rpc_password_env not in os.environ:
                raise ValueError("supply --rpc-cookie or --rpc-user and the password environment variable")
            credentials = args.rpc_user + ":" + os.environ[args.rpc_password_env]
        rpc = RPC(args.rpc_url, credentials, deadline)
        info = rpc.batch([("getblockchaininfo", [])])[0]
        if info["chain"] != "main" or info["blocks"] < height:
            raise ValueError("RPC must have mainnet blocks through target height")
        sampled = windows(height, args.strata, args.repeats, args.window, args.seed)
        hashes = rpc.batch([("getblockhash", [w["end"]]) for w in sampled])
        for w, hash_ in zip(sampled, hashes):
            w["end_hash"] = hash_
        source_index = args.source_datadir / "blocks/index"
        with contextlib.ExitStack() as stack:
            stack.enter_context(lock_source_index(source_index))
            work = Path(stack.enter_context(tempfile.TemporaryDirectory(prefix="ibd-estimate-", dir=args.work_dir or args.source_datadir.parent)))
            snapshot_index(source_index, work / "source-index")
            def helper(payload, plan=False, number=0):
                infile = work / "input.json"
                infile.write_text(json.dumps(payload))
                datadir = work / ("run-%d" % number)
                datadir.mkdir(exist_ok=True)
                command = [str(args.binary.resolve()), "-input=" + str(infile),
                           "-source=" + str(args.source_datadir), "-source-index=" + str(work / "source-index"),
                           "-datadir=" + str(datadir)]
                if plan:
                    command.append("-plan=1")
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError("benchmark wall budget exhausted")
                proc = subprocess.run(command, capture_output=True, text=True, timeout=remaining)
                if proc.returncode:
                    raise RuntimeError("helper failed: " + proc.stderr[-3000:])
                return json.loads(proc.stdout)
            tip_hash = rpc.batch([("getblockhash", [height])])[0]
            tip = rpc.batch([("getblockheader", [tip_hash])])[0]
            report["target_hash"] = tip_hash
            plans = helper({"windows": sampled, "tip_height": height, "tip_hash": tip_hash,
                            "tip_chainwork": tip["chainwork"]}, plan=True)
            known = {}
            for w in plans:
                known.update(w["known"])
            missing = sorted({h for w in plans for h in w["needed"] if str(h) not in known})
            values = rpc.batch([("getblockhash", [h]) for h in missing])
            known.update({str(h): v for h, v in zip(missing, values)})
            parent_hashes = [w["known"][str(w["end"] - w["count"])] for w in plans]
            parents = rpc.batch([("getblockheader", [h]) for h in parent_hashes])
            txstats = rpc.batch([("getchaintxstats", [0, h]) for h in parent_hashes])
            for w, parent, txstat in zip(plans, parents, txstats):
                w["parent_chainwork"] = parent["chainwork"]
                w["parent_txcount"] = txstat["txcount"]
            report["preparation_seconds"] = time.monotonic() - started
            for number, w in enumerate(plans, 1):
                w["context"] = {str(h): known[str(h)] for h in w["needed"]}
                result = helper(w, number=number)
                report["samples"].append(result)
                print("window %d/%d height=%d headers=%.1f/s blocks=%.1f/s" %
                      (number, len(plans), result["start"], result["count"] / result["header_seconds"],
                       result["count"] / result["block_seconds"]), flush=True)
            report.update(estimate(report["samples"], height, args.strata, args.seed))
            report["status"] = "complete"
    except (Exception, KeyboardInterrupt) as error:
        report["error"] = str(error)
    report["elapsed_seconds"] = time.monotonic() - started
    report["limitations"] = [
        "Processing estimate, not a measured full network IBD; assumes a local data supply without peer stalls or bandwidth bottlenecks.",
        "Sparse authentic ancestor context and restored spent coins; small test databases do not reproduce full-index/UTXO cache pressure or long-term LevelDB compaction.",
        "Block timing includes deserialize, ProcessNewBlock, ActivateBestChain/ConnectTip, block/undo writes, coins flush and synchronous index flush; excludes P2P scheduling and wallet callbacks.",
        "Default assumevalid and script-worker settings are preserved using authentic best-header/assumevalid ancestry and chainwork; no header PoW workers run.",
        "Header timing uses ProcessNewBlockHeaders with fresh PoW; block stage reuses only that verified evidence, so Yespower is counted once.",
        "95% stratified bootstrap interval describes sampled window variability only, not systematic error; serial total is header+block, ideal overlap is max(header,block).",
        "Concurrent workloads and small windows can bias rates; no finite end-to-end error guarantee. Fixed target excludes chain growth during the predicted sync."
    ]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({k: v for k, v in report.items() if k not in ("samples", "limitations")}, indent=2))
    return 0 if report["status"] == "complete" else 1


if __name__ == "__main__":
    raise SystemExit(main())
