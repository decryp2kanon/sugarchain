# Estimating full-verification IBD in less than ten minutes

Build the opt-in **test binary** once (build time is not benchmark time):

```sh
make -C src bench/ibd_estimate -j2
python3 contrib/bench/test_estimate_ibd.py
```

Use a **stopped, unpruned mainnet datadir** with block and undo files, and a
separate synced mainnet RPC node for read-only height-to-hash lookups:

```sh
export IBD_RPC_PASSWORD='your RPC password'
python3 contrib/bench/estimate-ibd.py \
  --source-datadir /path/to/stopped-node \
  --rpc-url http://127.0.0.1:34229 --rpc-user your-user \
  --output /tmp/ibd-estimate.json
```

Alternatively use `--rpc-cookie /path/to/rpc/node/.cookie`. No RPC writes are
performed. The source index is protected by a POSIX record lock; a live source
is rejected. Immutable LevelDB tables are hardlinked into a temporary index,
while manifests/logs are copied. The helper opens only this copy. Source block
and undo files are opened `rb`, and their hashes/checksums are checked. All
validation writes go into separate temporary directories, removed on exit.
`--work-dir` must share the source filesystem to allow these hardlinks.

The default wall budget is 570 seconds, including snapshot preparation, RPC,
fixture reads, all windows and cleanup (a few seconds are reserved for cleanup).
The supervisor kills a timed-out helper. An incomplete run exits nonzero and
writes raw completed observations and an error, **without publishing an ETA**.
OS/filesystem stalls can delay cleanup beyond the deadline. Build time is
excluded. `--height` overrides the target inferred from the source's last logged
`UpdateTip`; every sampled block must exist in that snapshot and RPC chain.

## What is measured

The seeded, reproducible design divides heights 1..target into eight equal
strata. Each has five separated, randomly located 2,000-block windows, randomized
in execution order to reduce confounding between era and machine load. That is
80,000 distinct headers and blocks, covering early, middle and recent history.
More `--repeats` may improve precision but can exceed the budget.
Smaller `--window` values can reduce runtime on slower machines (maximum 2,000). A full synchronized snapshot is required; a short fresh sync cannot
provide representative historical UTXO workloads.

The test translation unit includes the unchanged `validation.cpp` so that the
actual private validation functions can be called without adding production
hooks. It is not linked into the node and is not part of the default build.

For each window:

1. Read real blocks and authenticated undo records, reconstruct external spent
   coins in a temporary coins DB, and load authentic sparse ancestor indexes.
   Ancestor paths include difficulty/median-time context, BIP34, skip pointers
   and time-based sequence-lock dependencies. These are fixture preparation,
   outside processing timings. Source status bits for the measured headers
   are never loaded.
2. Set `-fast-ibd=0`, create no header PoW workers, then time the actual
   `ProcessNewBlockHeaders`: Yespower, contextual checks and index insertion.
   Require every resulting index to have `BLOCK_POW_CHECKED`.
3. Deserialize fresh block objects. Time the actual `ProcessNewBlock`, including
   header/block/contextual checks, block/undo writes, `ActivateBestChain`,
   `ConnectTip`, `ConnectBlock`, tip updates and coins-cache merging. Drain
   validation callbacks and force `FlushStateToDisk` at the end of the window.
   Header PoW evidence from step 2 is reused, exactly as in full-verification IBD.
   No PoW is double-counted, no cached block validation is replayed.
4. Outside the timer, require a header and block with a modified invalid target
   to be rejected despite the previously accepted header. Fail on any valid
   sample rejection, checksum, ancestor, missing-input or disk error.

The node's default `assumevalid` and script-worker count are preserved. After
header verification, the fixture installs authentic best-header/assumevalid
ancestor paths and actual RPC chainwork, so the existing `ConnectBlock` rule
decides which scripts to verify. No assumed-valid evidence is supplied to header
verification. Coinbase-only blocks and actual non-coinbase transactions are
included without selecting by transaction count.

## Reading the report

Each stratum's mean **seconds per block**, multiplied by its full height count,
is summed. Averaging blocks/second would bias the estimate toward fast windows.
Header and block costs are reported separately; `total_seconds` adds them for a
serial processing estimate. `ideal_overlap_seconds` is their maximum, a
hypothetical fully overlapped processing bound, not a second measured ETA.

The 95% intervals resample whole windows within strata, preserving header/block
correlation. These are **sampling-only bootstrap intervals**, not confidence
bounds on end-to-end network IBD. With only five windows per stratum they are
approximate and may underrepresent rare heavy blocks or long stalls. The raw
rows expose era, throughput, transaction/input counts and serialized bytes.

Systematic limits matter more than a narrow bootstrap interval:

- Full-size block-index/UTXO cache misses, long-running LevelDB compaction,
  memory/swap pressure and ongoing chain growth are not reproduced.
- Every short window forces a disk/index flush and starts a small DB; this can
  overstate flush cost while understating large-database cost.
- Network transfer, peer stalls, P2P scheduling and wallet callbacks are
  excluded. The actual block admission, activation, UTXO updates, undo/block
  writes and persistence run with an empty mempool and no wallet, but this is
  not a full daemon/network replay. The height-sized active-chain vector is
  allocated outside the timer; sparse ancestry is sufficient for these IBD
  paths. Non-IBD samples are rejected rather than synthesizing missing history.
- Contending processes can distort either phase's rate. The result applies to
  default assumevalid and script workers, not an `-assumevalid=0` run.

Use the result as a local **processing-time estimate**, not a promised completion
clock. There is no honest finite percentage bound on the unmeasured network and
large-database effects. Compare runs with different seeds or on an idle machine
when tighter operational estimates are needed.

## Recorded local run

The [checked-in report](results/fast-ibd-0-local.json) covers height 44,564,403:
40 windows, 80,000 blocks, 82,530 transactions and 100,941 non-coinbase inputs.
Preparation and measurement took 291.23 seconds. Processing estimates were
36.47 hours for headers and 2.58 hours for blocks, totaling 39.04 hours; the
sampling-only 95% interval for the sum was 38.93–39.16 hours. The source and
temporary output were on the local NVMe filesystem, with other nodes running;
this is not an idle-machine or end-to-end network result.

Validation included six deterministic Python tests, a four-window smoke run,
invalid-target rejection through both native ingress paths in every window,
a live-source lock rejection and a forced 1.5-second timeout (1.43 seconds
observed, no ETA emitted). The existing `fast_ibd_rejects_unproven_headers_and_disk_blocks`
and `processnewblock_checks_pow_during_ibd` regression tests also passed (26
assertions). The production IBD/consensus sources are unchanged.
