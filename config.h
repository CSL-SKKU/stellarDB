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
