# StellarDB

## Compiling

To build StellarDB, install the required dependencies and run `make` in the project directory.

```bash
sudo apt install make clang autoconf libtool

# Note: the directory name is different due to an internal development nickname.
cd TNTStore
make
```

## Running

Before running, you must manually create the following directory:

```
/scratch0/kvell/
```

You also need to configure CPU core mapping in `util.c`.

Default values can be changed by modifying `config.c` and `options.c`.

> **Note**: `nb_disks` is currently not supported and must always be set to `1`.

> **Tip**: You may mount your target storage device to `/scratch0/kvell/` if needed.

### Usage

```bash
./main [options] <nb_disks> <nb_workers> <nb_distributors>
```

### Options

```
  -P, --page-cache-size <bytes>   Set page cache size
  -b, --bench <bench_name>        Select workload (e.g., ycsb_c_zipfian)
  -a, --api <api_name>            Select API (ycsb, dbbench, bgwork, production)
  -k, --kv-size <bytes>           Set KV size (used in Section 4.4 experiments)
  -m, --max-file-size <bytes>     Set max file size (used in Section 4.4 experiments)
  -i, --insert-mode <ascend|descend|random>  Set insert mode (used in Section 4.5 experiments)
  -o, --old-percent <float>       Set OLD_PERCENT
  -e, --epoch <number>            Set epoch count
  -r, --with-reins                Enable reinsertion logic (used in Section 4.5 experiments)
  -R, --with-rebal                Enable rebalancing logic (used in Section 4.5 experiments)
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
> For consistent results across experiments, it is recommended to run `rm /scratch0/kvell/*` or reformat the target device (`mkfs`) before each run.

## Index-Only Testing

For index-only microbenchmarking, build and run the test binary:

```bash
make test
./test/test_main <total_requests> <shuffle_range> <workload_type (A|B|C)> <num_threads>
```

## License

This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for details.

