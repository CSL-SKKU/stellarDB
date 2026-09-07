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

StellarDB stores its local files under `/scratch0/kvell/` by default. Create the
directory and ensure that the current user can write to it before running:

```bash
sudo mkdir -p /scratch0/kvell
sudo chown "$(id -un):$(id -gn)" /scratch0/kvell
```

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
  -P, --page-cache-size <bytes>   Set page cache size
  -b, --bench <bench_name>        Select workload (e.g., ycsb_c_zipfian)
  -a, --api <api_name>            Select API (ycsb, dbbench, bgwork, locality, latprobe)
  -k, --kv-size <bytes>           Set KV size (used in Section 4.4 experiments)
  -m, --max-file-size <bytes>     Set max file size (used in Section 4.4 experiments)
  -i, --insert-mode <ascend|descend|random>  Set insert mode (used in Section 4.5 experiments)
  -o, --old-percent <float>       Set OLD_PERCENT
  -e, --epoch <number>            Set epoch count
  -r[<x>], --with-reins[=<x>]     Reinsert after >= ceil(x * log2(nodes+1)) history hops (x >= 0, default 1.0)
      --reins-sample <N>          With -r: attempt a copy on one in N qualifying reads (16; 0 means 1)
  -R[<x>], --with-rebal[=<x>]     Rebalance when depth > x * ceil(log2(nodes+1)) (x >= 0, default 5.0)
  -n, --items <number>            Number of items in the database
  -q, --requests <number>         Number of requests
  -c, --chunk <number>            Chunk size for key shuffling (used in Section 4.5 experiments)
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
(default 16; 1 attempts every qualifying read; 0 is treated as 1). Sampling alone
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
