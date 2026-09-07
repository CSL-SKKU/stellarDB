// config.h
#ifndef CONFIG_H
#define CONFIG_H

#include "options.h"
#include "workload-common.h"  // bench_t, YCSB, BGWORK, DBBENCH, PRODUCTION, ...

struct runtime_config {
    unsigned long      page_cache_size;
    bench_t            bench;           // enum으로
    struct workload_api *api;           // 포인터로
    int                kv_size;
    unsigned long      max_file_size;
    int                insert_mode;
    double             old_percent;
    unsigned long      epoch;
    /* -r[<x>]: copy hot records after ceil(x * log2(nodes+1)) history hops. */
    int                with_reins;
    double             reins_multiplier;
    int                with_rebal;
    /* Rebalance when depth > log2(nodes) * rebalance_threshold. */
    double             rebalance_threshold;
    /* --util-gate: also require the (distributor busy, I/O idle) gate. Off. */
    int                util_gate;
    /* --latency-series <ms>: per-interval latency lines ("#L"); 0 = off. */
    unsigned long      latency_series_ms;
    /* --churn-mix U/I/D: percentages of updates, inserts of new keys (top of
     * the key space) and deletes of the oldest live key; the rest are reads.
     * Inserts and deletes balance, so the live key count stays constant while
     * the key window slides upward. */
    int                churn_upd, churn_ins, churn_del;
    /* --dump-slabs <s>: heavy monitoring; "#S" line per node every <s> seconds
     * and at the end of the run (seq, kind, history parent, pivot, key range,
     * reserved/valid/stale, tombstones written). 0 = off. */
    unsigned long      dump_slabs_s;
    /* --reins-sample N: copy on one in N qualifying reads (1 = every one). A
     * key read deep N times is copied with high probability; a key read once
     * almost never is. Zero-memory stand-in for a per-record hit counter. */
    unsigned long      reins_sample;
    /* -p <ratio>: enable repeated pruning and set prune_stale_ratio. */
    int                with_prune;
    double             prune_stale_ratio;
    /* -M: shared background maintenance interval, in milliseconds. */
    unsigned long      maintenance_period_ms;
    /*
     * --migrate-th t: move an internal node's valid entries into its history
     * parent when valid(parent) + valid(node)
     * <= t * slab capacity, leaving the node empty. 0 = off.
     */
    double             migrate_th;
    uint64_t           nb_items_in_db;
    uint64_t           nb_requests;
    uint64_t           chunk_for_shuffle;
};

extern struct runtime_config cfg;
void init_default_config(struct runtime_config *cfg);
int validate_runtime_config(const struct runtime_config *cfg);

bench_t parse_bench(const char *s);
struct workload_api *parse_api(const char *s);
int parse_insert_mode(const char *s);

#endif // CONFIG_H
