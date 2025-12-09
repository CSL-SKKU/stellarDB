// config.c
#include <string.h>
#include <stdio.h>
#include "config.h"

// -- bench 문자열 매핑 테이블
static const struct { const char *name; bench_t b; } bench_table[] = {
    {"ycsb_a_uniform",      ycsb_a_uniform},
    {"ycsb_b_uniform",      ycsb_b_uniform},
    {"ycsb_c_uniform",      ycsb_c_uniform},
    {"ycsb_e_uniform",      ycsb_e_uniform},
    {"ycsb_a_zipfian",      ycsb_a_zipfian},
    {"ycsb_b_zipfian",      ycsb_b_zipfian},
    {"ycsb_c_zipfian",      ycsb_c_zipfian},
    {"ycsb_e_zipfian",      ycsb_e_zipfian},
    {"prod1",               prod1},
    {"prod2",               prod2},
    {"dbbench_all_random",  dbbench_all_random},
    {"dbbench_all_dist",    dbbench_all_dist},
    {"dbbench_prefix_random", dbbench_prefix_random},
    {"dbbench_prefix_dist", dbbench_prefix_dist},
    {"locality_random", locality_random},
    {"locality_temporal", locality_temporal},
    {"locality_keyspace", locality_keyspace},
    {"locality_both", locality_both},
    {"bgwork_reinsertion",  bgwork_reinsertion},
    {NULL, 0}
};

int parse_insert_mode(const char *s) {
    if (strcasecmp(s, "ascend") == 0)   return ASCEND;
    if (strcasecmp(s, "descend") == 0)  return DESCEND;
    if (strcasecmp(s, "random") == 0)   return RANDOM;
    // 혹은 숫자로도 받으려면:
    if (strcmp(s, "1") == 0) return ASCEND;
    if (strcmp(s, "2") == 0) return DESCEND;
    if (strcmp(s, "3") == 0) return RANDOM;

    fprintf(stderr, "Unknown insert_mode '%s', using default (%s)\n",
            s,
            INSERT_MODE == ASCEND ? "ascend" :
            INSERT_MODE == DESCEND ? "descend" :
            "random");
    return INSERT_MODE;
}

bench_t parse_bench(const char *s) {
    for (int i = 0; bench_table[i].name; i++) {
        if (strcmp(s, bench_table[i].name) == 0)
            return bench_table[i].b;
    }
    fprintf(stderr, "Unknown bench '%s', using default\n", s);
    return SELECTED_BENCH;
}

// -- API 문자열 매핑
struct workload_api *parse_api(const char *s) {
    if (strcasecmp(s, "ycsb") == 0)      return &YCSB;
    if (strcasecmp(s, "dbbench") == 0)   return &DBBENCH;
    if (strcasecmp(s, "bgwork") == 0)    return &BGWORK;
    if (strcasecmp(s, "locality") == 0)    return &LOCALITY;
    fprintf(stderr, "Unknown api '%s', using default YCSB\n", s);
    return &YCSB;
}

struct runtime_config cfg;

void init_default_config(struct runtime_config *cfg) {
    cfg->page_cache_size = PAGE_CACHE_SIZE;
    cfg->bench           = SELECTED_BENCH;
    cfg->api             = &YCSB;  // 기본
    cfg->kv_size         = KV_SIZE;
    cfg->max_file_size   = MAX_FILE_SIZE;
    cfg->insert_mode     = INSERT_MODE;
    cfg->old_percent     = OLD_PERCENT;
    cfg->epoch           = EPOCH;
    cfg->with_reins      = 0;
    cfg->with_rebal      = 0;
    cfg->nb_items_in_db  = 100000000LU;  // 기존 기본값
    cfg->nb_requests     = 0;            // 런타임 결정
    cfg->chunk_for_shuffle = 1;
}
