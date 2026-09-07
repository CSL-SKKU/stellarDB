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
    int                with_reins;
    int                with_rebal;
    /* Rebalance when depth > log2(nodes) * rebalance_threshold. */
    double             rebalance_threshold;
    /* --util-gate: also require the (distributor busy, I/O idle) gate. Off. */
    int                util_gate;
    /* --latency-series <ms>: per-interval latency lines ("#L"); 0 = off. */
    unsigned long      latency_series_ms;
    /*
     * --reins-on-read <levels>: reinsertion decided at read completion by the
     * I/O worker that owns the page, per record, for records found at least
     * <levels> above the leaf on a page already hot in this epoch. Replaces the
     * background worker's full-slab pass. 0 = off (with -r: the old worker).
     */
    unsigned long      reins_on_read;
    /* --reins-sample N: copy on one in N qualifying reads (1 = every one). A
     * key read deep N times is copied with high probability; a key read once
     * almost never is. Zero-memory stand-in for a per-record hit counter. */
    unsigned long      reins_sample;
    /* --reins-depth-ratio th: instead of a fixed level count, copy when the
     * upward walk exceeded th * ceil(log2(nodes+1)), the depth a balanced tree
     * would need. Nothing qualifies on a well-shaped tree; stranded records
     * on long history chains do, whatever the tree size. 0 = use levels. */
    double             reins_depth_ratio;
    int                with_prune;
    /* Slots kept free in a merged slab. */
    unsigned long      prune_margin;
    /* Skip a triple whose leaf is among the newest N slabs (0: no minimum). */
    unsigned long      prune_min_age;
    /* -C: prune automatically when the stale-slot ratio crosses the threshold. */
    int                prune_auto;
    double             prune_stale_ratio;
    unsigned long      prune_period_ms;
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
