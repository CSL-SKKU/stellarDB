# StellarDB

StellarDB is a storage engine derived from TNTStore/KVell. This repository
contains the core source code needed to build and run the engine; generated
binaries, raw traces, and machine-specific experiment outputs are intentionally
kept out of version control.

## Compiling

To build StellarDB, install the required dependencies and run `make` in the project directory.

```bash
sudo apt install make clang autoconf libtool
cd stellarDB
make
```

The default application page cache is 8 GiB. Build-time defaults can be
overridden when needed:

```bash
make PAGE_CACHE_SIZE='(PAGE_SIZE * 1048576)'
```

## Running

StellarDB stores its local files under `/scratch0/kvell/` by default. Use `-D`
or `--directory` to select another directory for slab files and recovery metadata:

```bash
./main --directory ./data -n 1000000 -q 1000000 1 4 2
```

Startup creates missing directories, including parent directories, and logs each
creation to stdout. Permissions follow the process's umask. Existing directories
are reused for recovery. Startup exits with an error if the path is not a
directory or the current user lacks read, write, or search permission. Relative
paths are resolved from the working directory; use the same directory to reopen
a database.

CPU pinning is controlled in `options.h`; the affinity implementation is in
`utils.c`. Other runtime defaults are defined in `config.c` and `options.h`.

> **Warning:** Never format or clear a storage device without checking its
> contents and mount point. Machine-specific scripts and raw benchmark outputs
> are intentionally excluded from this repository.

For workloads backed by an external real-key trace, pass the trace path at
build time. Trace files themselves are not stored in Git:

```bash
make REALKEY_FILE_PATH=/path/to/key-trace
```

> **Note**: `nb_disks` is currently not supported and must always be set to `1`.

### Usage

```bash
./main [options] <nb_disks> <nb_workers> <nb_distributors>
```

### Options

```
  -D, --directory <path>         Database directory (default /scratch0/kvell; created if missing)
  -P, --page-cache-size <bytes>   Set page cache size
  -b, --bench <bench_name>        Select workload (e.g., ycsb_c_zipfian)
  -a, --api <api_name>            Select API (ycsb, dbbench, bgwork, locality, latprobe)
  -k, --kv-size <bytes>           Set KV size (used in Section 4.4 experiments)
  -m, --max-file-size <bytes>     Set max file size (used in Section 4.4 experiments)
  -i, --insert-mode <ascend|descend|random>  Set insert mode (used in Section 4.5 experiments)
  -o, --old-percent <float>       Set OLD_PERCENT
  -e, --epoch <number>            Set epoch count
  -r[<x>], --with-reins[=<x>]     Reinsert after >= ceil(x * log2(nodes+1)) history hops (x >= 0, default 1.0)
      --reins-sample <N>          With -r: attempt a copy on one in N qualifying reads (1; 0 means 1)
  -R[<x>], --with-rebal[=<x>]     Rebalance when depth > x * ceil(log2(nodes+1)) (x >= 0, default 5.0)
  -n, --items <number>            Number of items in the database
  -q, --requests <number>         Number of requests
  -c, --chunk <number>            Chunk size for key shuffling (used in Section 4.5 experiments)
      --report-out <file.csv>     Enable reporting (omitted or none: off)
      --config-report <file>      Select metrics; all are enabled by default
      --timeseries <seconds>      Report interval averages; omitted: whole-run aggregate
      --wait-for-pruning-s <s>     Post-request pruning grace period; requires -p (0 = off)
  -h, --help                      Show help message
```

### Examples

```bash
# 100M records, 100M requests, YCSB C (Zipfian), 16 GB app cache, 48 I/O workers, 12 distributors
./main -n 100000000 -q 100000000 -a ycsb -b ycsb_c_zipfian -P 17179869184 1 48 12

# 100M records, 100M requests, Mixgraph benchmark, 4 GB app cache, 48 I/O workers, 12 distributors
./main -n 100000000 -q 100000000 -a dbbench -b dbbench_prefix_dist -P 4294967296 1 48 12
```

> `-k` and `-m` options are used for experiments in Section 4.4.
> `-i`, `-c`, `-r`, and `-R` options are used for experiments in Section 4.5.
For reproducible experiments, record the compiler version, storage device,
NUMA layout, worker mapping, queue depth, and cache size together with results.

## Traversal hop reporting

`upward_hops_avg` and `downward_hops_avg` are optional report fields selected
through [report.config](report.config). Both average over client READ index
lookups, including misses and tombstone hits. Writes, recovery, initial loading,
and maintenance lookups are excluded. Each constituent READ in a scan or RMW
counts separately; the denominator is index lookups, not client requests.

A hop is an edge between nodes. Upward hops follow historical `lu_parent`
links from the routed leaf; downward hops follow routing children from the
current root. A leaf hit has zero upward hops, and a root that is also a leaf
has zero downward hops. Range/filter skips still cross edges and count.
Reaching NULL and retrying a rebuilt slab at the same node do not add hops.
Pruning restarts include edges crossed in every attempt, counted as one lookup.
The reinsertion policy's existing leaf-inclusive `upward_len` is unchanged.

Samples are recorded when the index lookup returns, before disk I/O completes.
With `--timeseries`, each row averages the lookups recorded in that interval;
otherwise the row averages the measured run. Intervals with no lookups have
empty hop cells. The counters are disabled when reporting is off or both
fields are disabled.

## Reinsertion

`-r` / `--with-reins` enables reinsertion at read completion with a default
multiplier of `1.0`. Attach an optional multiplier as `-r0.5` or
`--with-reins=0.5`. The threshold is
`ceil(multiplier * log2(node_count + 1))` historical parent hops above the leaf;
the multiplication happens before rounding. Internally, `upward_len` includes
the leaf, so a read qualifies when `upward_len` is greater than that threshold.
The threshold depends on node count and the multiplier, not routing depth.

The multiplier must be finite and nonnegative. Smaller values consider
shallower reads; larger values require deeper history. `-r0` bypasses only
the distance check. The optional value must be attached: `-r 1 48 12` keeps
the default multiplier and treats the three numbers as disks, workers, and
distributors. `-r0.5 1 48 12` uses the multiplier `0.5` with the same topology.

The source page must already be marked hot by an earlier read since the last
bitmap reset. `--reins-sample N` passes approximately one in N qualifying reads
(default 1; 1 attempts every qualifying read; 0 is treated as 1). Sampling alone
does not enable reinsertion. The bitmap tracks pages, not repeated reads of a
particular key, and is cleared using distributor-loop epochs rather than seconds.

The I/O worker copies the record from the read's page, checks that it is still
authoritative, reserves a leaf slot, and submits an asynchronous shy write. These
preparations run before the client's read callback and can add latency. There is
no automatically started slab-scanning worker. New-key inserts and index misses
do not trigger reinsertion. `-R` independently enables routing rebalancing.

```bash
# YCSB D with reinsertion and rebalancing, sampling one in 16 qualifying reads
./main -n 100000000 -q 100000000 -b ycsb_d_latest -r -R --reins-sample 16 1 48 12

# Half the logarithmic distance threshold, before rounding
./main -n 100000000 -q 100000000 -b ycsb_d_latest -r0.5 -R --reins-sample 16 1 48 12
```

The former `--reins-on-read`, `--reins-depth-ratio`, and
`STELLAR_REINS_ON_READ` controls have been removed; use `-r` instead. The old
`routing_depth / 3` trigger is gone. Check `#R run reins-on-read` for reads seen
and deep/hot reads before sampling; `#R run reinsertion` reports copy attempts,
published copies, and abandoned copies. Slab queue counters stay zero in this
mode.

## Rebalancing

`-R` / `--with-rebal` enables routing rebalancing with a default threshold of
`5.0`. Attach an optional threshold as `-R2.0` or `--with-rebal=2.0`.
Background rebalancing runs when the tree has more than one node and:

```text
depth > threshold * ceil(log2(node_count + 1))
```

Higher values are less sensitive. The threshold must be finite and
nonnegative; `0` requests rebalancing at each maintenance check for a tree
with more than one node. The shared maintenance interval is controlled by
`-M`. With `-R` enabled, `main` also rebalances once after loading, independently
of the background threshold. Rebalancing preserves historical `lu_parent`
links and moves no records.

The optional value must be attached, so `-R 1 48 12` uses the default threshold
and preserves the disk/worker/distributor arguments. Use `-R2.0 1 48 12` for
threshold `2.0`. The last `-R` setting wins; a later bare `-R` restores the
default. Configure the threshold through `-R`; `--rebalance-threshold` is no
longer accepted.

## Pruning

`-p RATIO` / `--with-prune RATIO` enables repeated pruning while the global
stale/reserved slot ratio is at or above `RATIO`. The argument is required and
must be between `0` and `1`; for example, `-p 0.3` uses a 30% threshold. The worker
rechecks the ratio after each successful prune and stops when it falls below
the threshold or no candidate succeeds. `-M` / `--maintenance-period-ms` sets the
periodic wake interval shared by background maintenance (default `500` ms).

The former `-C` / `--pruning` and `--prune-stale-ratio` options have been removed;
use `-p RATIO` instead. Setting the maintenance period alone does not enable pruning.

`--migrate-th T` enables migration when an internal node's valid entries fit
with its history parent's within `T` times the slab capacity (`0` disables it).
The worker attempts one migration per wake before the ILI pruning burst.
For example, `-p 0.3 --migrate-th 0.9 -M 500` enables both. Standalone slab
compaction and all `--compact-*` options have been removed.

### Post-request idle pruning

`--wait-for-pruning-s X` gives pruning a grace period after all benchmark client
requests complete. It requires `-p RATIO`, accepts fractional seconds, and is
disabled by default or with `X = 0`. Busy throughput, latency, traversal, and
maintenance measurements finish at their existing completion boundary.

At idle entry, the optional background reinsertion producer stops, maintenance
parks, and outstanding copies and stale hints drain. Before testing the target,
one complete in-memory sweep marks every ancestor entry shadowed by a descendant,
including copies behind tombstones and copies whose invalidation hints were lost.
It follows the historical tree and updates invalid bits and live-entry counts
directly, with no slab reads or writes. A reusable hash table and undo stack keep
only the current ancestor path: expected O(indexed entries) time and memory
proportional to the largest ancestor path, plus O(nodes) traversal metadata.
The sweep also runs when the initial stale estimate is already below the target.

After the sweep, enabled rebalancing and migration continue alongside pruning;
the utilization gate is bypassed only during idle. Maintenance starts immediately
and unsuccessful pruning bursts retry at the configured `-M` interval. The phase
ends when the stale ratio is **at most** the `-p` target, or when `X` expires.
`X` includes settling and invalidation and is checked between operations. A
started sweep or maintenance operation finishes safely, so elapsed time can exceed
`X`. If settling consumes the budget, invalidation is reported as `not_started`
and the phase times out without claiming the target was reached. A sweep error
ends the phase with `invalidation_failed`, without starting structural maintenance.
An unreachable target is a normal `timeout` result and does not make the
benchmark fail.

Stdout always includes a final `# Idle pruning:` summary with elapsed seconds,
pruning execution seconds, attempts/successes, initial/final stale ratios, target,
status, and last prune return code when attempts are nonzero (`0` success,
`1` no candidate, negative errno).
`# Idle invalidation: status=complete newly_marked=N` records sweep completion.
Elapsed time includes the handoff from background maintenance, pending-work
settlement, and the sweep. Pruning execution time sums complete idle prune calls,
including unsuccessful attempts, and excludes sleeps, invalidation, rebalancing,
migration, and an operation already running at handoff. The initial ratio is measured at
handoff; repairing missed hints can make it rise before pruning reduces it. Both ratios use
the same stale/reserved **estimate** as the pruning trigger, not physical space
amplification.

With `--report-out`, enabling the wait adds a `phase` column (`busy` or `idle`)
and `idle_*` result columns to the CSV. Busy values are unchanged; idle-only cells
are empty on busy rows, and busy-metric cells are empty on idle rows. Filter by
`phase` when analyzing request performance. Without `--timeseries`, there is one
idle summary row. With it, idle rows contain cumulative pruning time and counts
plus the current stale ratio at the requested interval, followed by a final row
even on timeout. The sweep samples between bounded chunks of index entries with
`idle_status=invalidating`; other maintenance operations sample at operation
boundaries and skip missed intervals. Invalidation time is excluded from busy
request throughput/latency and from idle pruning execution time.
Without this option, the existing CSV schema is unchanged.

```bash
./main -D ./db -a ycsb -b ycsb_a_uniform -n 100000 -q 1000000 \
  -p 0.3 --wait-for-pruning-s 60 --report-out run.csv --timeseries 1 1 4 2
```

## Index-Only Testing

For index-only microbenchmarking, build and run the test binary:

```bash
make test
./test/test_main <total_requests> <shuffle_range> <workload_type (A|B|C)> <num_threads>
```

## Latency Probe

Build with detailed timing enabled, then select the `latprobe` API and
benchmark. The workload keeps exactly one request in flight and writes one CSV
row per completed request to standard output.

```bash
make clean
make STELLAR_DEBUG=1
./main -n 1000000 -q 10000 -a latprobe -b latprobe 1 48 12 \
  > latency.csv
```

Without `STELLAR_DEBUG=1`, end-to-end latency is still reported but the individual
stage columns are zero.

## License

This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for details.
