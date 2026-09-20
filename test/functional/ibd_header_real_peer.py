#!/usr/bin/env python3
# Copyright (c) 2026 The Sugarchain developers
# Distributed under the MIT software license.

"""Real-peer IBD header download benchmark.

This is intentionally NOT part of test_sugarchain/make check.  It launches a
fresh benchmark node using the supplied --datadir and connects it only to the
supplied fully-synced --peer.  The benchmark observes real P2P header progress
through RPC and reports header throughput.

The datadir must be dedicated to this benchmark. Existing chain data is refused
unless --allow-existing-datadir is explicitly supplied.
"""

import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import urllib.request


# Small JSON-RPC helper used to read benchmark-node state and stop it cleanly.\n# This talks only to the temporary benchmark node, not to the full-sync peer.\ndef rpc(url, user, password, method, params=None):\n    payload = json.dumps({"jsonrpc": "1.0", "id": "ibd-header-bench",
                          "method": method, "params": params or []}).encode()
    req = urllib.request.Request(url, data=payload,
                                 headers={"Content-Type": "application/json"})
    import base64
    token = base64.b64encode((user + ":" + password).encode()).decode()
    req.add_header("Authorization", "Basic " + token)
    with urllib.request.urlopen(req, timeout=5) as response:
        result = json.loads(response.read().decode())
    if result.get("error"):
        raise RuntimeError(result["error"])
    return result["result"]


def main():
    p = argparse.ArgumentParser(description="Benchmark real IBD header download from one full-sync peer")\n    # Required inputs:\n    #   --peer    = P2P address of an already fully-synced SugarChain node.\n    #               Example: 127.0.0.1:34230\n    #   --datadir = EMPTY/dedicated directory for the new benchmark IBD node.\n    #               Do NOT point this at the full-sync peer's existing datadir.\n    p.add_argument("--peer", required=True, help="full-sync P2P peer as HOST:PORT")
    p.add_argument("--datadir", required=True, help="dedicated datadir for the benchmark IBD node")
    p.add_argument("--daemon", default="./src/sugarchaind", help="path to sugarchaind")
    p.add_argument("--rpcport", type=int, default=18443, help="benchmark node RPC port")
    p.add_argument("--p2pport", type=int, default=18444, help="benchmark node local P2P port")
    p.add_argument("--rpcuser", default="ibdbench")
    p.add_argument("--rpcpassword", default="ibdbench")
    p.add_argument("--duration", type=int, default=60, help="measurement duration in seconds; 0 means until peer tip")
    p.add_argument("--sample", type=float, default=0.25, help="RPC sampling interval in seconds")
    p.add_argument("--startup-timeout", type=int, default=30)
    p.add_argument("--allow-existing-datadir", action="store_true",
                   help="allow a non-empty datadir; never deletes it")
    args = p.parse_args()

    # Safety guard: by default we refuse a non-empty datadir.  The benchmark\n    # starts a fresh node and must never accidentally reuse/delete valuable\n    # chain data.  --allow-existing-datadir is an explicit opt-in only.\n    datadir = os.path.abspath(os.path.expanduser(args.datadir))\n    if os.path.exists(datadir) and os.listdir(datadir) and not args.allow_existing_datadir:
        p.error("--datadir is not empty; use a dedicated empty directory or explicitly pass --allow-existing-datadir")
    os.makedirs(datadir, exist_ok=True)

    # Launch a dedicated benchmark daemon and force its outbound connection to\n    # --peer.  DNS discovery is disabled so measured header progress comes from\n    # the peer supplied by the user rather than random public peers.\n    cmd = [\n        args.daemon,
        "-datadir=" + datadir,
        "-server=1",
        "-listen=1",
        "-port=" + str(args.p2pport),
        "-rpcport=" + str(args.rpcport),
        "-rpcuser=" + args.rpcuser,
        "-rpcpassword=" + args.rpcpassword,
        "-connect=" + args.peer,
        "-dnsseed=0",
        "-listenonion=0",
        "-discover=0",
        "-printtoconsole=0",
    ]

    print("Launching benchmark node")
    print("Peer:", args.peer)
    print("Datadir:", datadir)
    proc = subprocess.Popen(cmd)
    url = "http://127.0.0.1:%d/" % args.rpcport

    try:
        # Phase 1: wait for the benchmark daemon's RPC server to become ready.\n        deadline = time.time() + args.startup_timeout\n        while True:
            if proc.poll() is not None:
                raise RuntimeError("benchmark node exited with status %d" % proc.returncode)
            try:
                rpc(url, args.rpcuser, args.rpcpassword, "getblockchaininfo")
                break
            except Exception:
                if time.time() >= deadline:
                    raise RuntimeError("RPC did not become ready within startup timeout")
                time.sleep(0.2)

        # Phase 2: wait until the configured full-sync P2P peer is connected.\n        # Measurement does not start before this succeeds.\n        deadline = time.time() + args.startup_timeout
        peer_info = []
        while time.time() < deadline:
            peer_info = rpc(url, args.rpcuser, args.rpcpassword, "getpeerinfo")
            if peer_info:
                break
            time.sleep(0.2)
        if not peer_info:
            raise RuntimeError("full-sync peer did not connect")

        # Capture the starting point.  'headers' is the best accepted header\n        # height; 'blocks' is the downloaded/processed block height.\n        info = rpc(url, args.rpcuser, args.rpcpassword, "getblockchaininfo")\n        start_headers = int(info["headers"])
        start_blocks = int(info["blocks"])
        max_headers = start_headers
        samples = [(time.monotonic(), start_headers, start_blocks)]

        print("Connected. start headers=%d blocks=%d" % (start_headers, start_blocks))
        start = time.monotonic()

        # Main measurement loop.  Poll RPC at --sample intervals and measure\n        # actual header-height growth while the daemon performs real P2P sync.\n        while True:\n            now = time.monotonic()\n            info = rpc(url, args.rpcuser, args.rpcpassword, "getblockchaininfo")
            headers = int(info["headers"])
            blocks = int(info["blocks"])
            max_headers = max(max_headers, headers)
            samples.append((now, headers, blocks))

            elapsed = now - start
            gained = headers - start_headers
            rate = gained / elapsed if elapsed > 0 else 0.0
            print("\rheaders=%d blocks=%d lead=%d gained=%d rate=%.1f headers/s" %
                  (headers, blocks, headers - blocks, gained, rate), end="", flush=True)

            # --duration=N: stop after N seconds.\n            # --duration=0: keep going until our header height reaches the\n            # connected peer's reported synced_headers value.\n            if args.duration > 0 and elapsed >= args.duration:\n                break
            if args.duration == 0:
                peers = rpc(url, args.rpcuser, args.rpcpassword, "getpeerinfo")
                peer_tip = max([int(x.get("synced_headers", -1)) for x in peers] + [-1])
                if peer_tip >= 0 and headers >= peer_tip:
                    break
            time.sleep(args.sample)

        end = time.monotonic()
        print()
        elapsed = end - start
        gained = max_headers - start_headers
        rate = gained / elapsed if elapsed > 0 else 0.0
        # Convert total header gain into equivalent full 2000-header batches.\n        # This makes the result directly comparable with the IBD target of\n        # multiple complete HEADERS responses per second.\n        batches = gained / 2000.0\n        batch_rate = batches / elapsed if elapsed > 0 else 0.0

        final_info = rpc(url, args.rpcuser, args.rpcpassword, "getblockchaininfo")
        print("IBD REAL-PEER HEADER BENCHMARK")
        print("  elapsed: %.3f s" % elapsed)
        print("  headers: %d -> %d (+%d)" % (start_headers, max_headers, gained))
        print("  blocks:  %d -> %d" % (start_blocks, int(final_info["blocks"])))
        print("  header lead: %d" % (int(final_info["headers"]) - int(final_info["blocks"])))
        print("  headers/s: %.1f" % rate)
        print("  equivalent 2000-header batches/s: %.3f" % batch_rate)
        return 0 if gained > 0 else 2
    finally:\n        # Always stop the benchmark daemon.  Try RPC first, then SIGTERM, and\n        # finally SIGKILL only if the process refuses to exit.\n        try:
            rpc(url, args.rpcuser, args.rpcpassword, "stop")
        except Exception:
            if proc.poll() is None:
                proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


if __name__ == "__main__":
    sys.exit(main())
