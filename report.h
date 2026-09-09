#ifndef STELLAR_REPORT_H
#define STELLAR_REPORT_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum report_metric {
  REPORT_THROUGHPUT, REPORT_LATENCY_AVG, REPORT_LATENCY_P99,
  REPORT_ENTRIES_TOTAL, REPORT_ENTRIES_STALE, REPORT_ENTRIES_TOMBSTONES,
  REPORT_ENTRIES_NORMAL, REPORT_NODES, REPORT_DEPTH,
  REPORT_UPWARD_HOPS_AVG, REPORT_DOWNWARD_HOPS_AVG,
  REPORT_PAGE_CACHE_HITS, REPORT_PAGE_CACHE_MISSES,
  REPORT_PAGE_CACHE_COALESCED, REPORT_PAGE_CACHE_HIT_RATIO,
  REPORT_REBALANCE, REPORT_REINSERTION, REPORT_PRUNING, REPORT_MIGRATION,
  REPORT_METRICS
};
/* Immutable after initialization, before recovery and worker startup. */
extern uint64_t report_mask;
static inline int report_enabled(enum report_metric metric) {
  return (report_mask & (UINT64_C(1) << metric)) != 0;
}
static inline int report_entry_classes(void) {
  return report_enabled(REPORT_ENTRIES_TOMBSTONES) ||
         report_enabled(REPORT_ENTRIES_NORMAL);
}
static inline int report_marked_entries(void) {
  return report_enabled(REPORT_ENTRIES_STALE) || report_entry_classes();
}
static inline int report_read_hops_enabled(void) {
  return report_enabled(REPORT_UPWARD_HOPS_AVG) ||
         report_enabled(REPORT_DOWNWARD_HOPS_AVG);
}
/* One completed client index lookup, including misses and retry edges. */
void report_read_hops(uint64_t upward, uint64_t downward);
/* Outcomes of a page-data lookup, before callbacks or disk-read completion.
 * A coalesced access is a miss that joins an already pending read. */
enum report_cache_result {
  REPORT_CACHE_HIT, REPORT_CACHE_FETCH, REPORT_CACHE_COALESCED,
  REPORT_CACHE_RESULTS
};
static inline int report_page_cache_enabled(void) {
  return (report_mask & (UINT64_C(15) << REPORT_PAGE_CACHE_HITS)) != 0;
}
void report_page_cache(enum report_cache_result result);
uint64_t report_now_ns(void);
uint64_t report_request_start(void);
void report_request_complete(uint64_t start_ns);
void report_event(enum report_metric metric);
int report_init(const char *output, const char *config, double interval_s);
void report_begin(void);
void report_finish(void);
void report_close(void);
struct idle_pruning_result {
  uint64_t elapsed_ns, pruning_ns, attempts, successes;
  double initial_ratio, stale_ratio, target; /* target=0: no ratio target */
  const char *status;
  int last_prune_status;
};
/* Separate post-request rows; never touches the busy collectors or clock. */
void report_idle_pruning(const struct idle_pruning_result *result);
/* O(nodes), no disk I/O or entry scans. Locally consistent slab snapshots. */
void tnt_report_entries(uint64_t *total, uint64_t *stale, uint64_t *tombstones);

#ifdef __cplusplus
}
#endif
#endif
