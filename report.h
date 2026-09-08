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
uint64_t report_now_ns(void);
uint64_t report_request_start(void);
void report_request_complete(uint64_t start_ns);
void report_event(enum report_metric metric);
int report_init(const char *output, const char *config, double interval_s);
void report_begin(void);
void report_finish(void);
void report_close(void);
/* O(nodes), no disk I/O or entry scans. Locally consistent slab snapshots. */
void tnt_report_entries(uint64_t *total, uint64_t *stale, uint64_t *tombstones);

#ifdef __cplusplus
}
#endif
#endif
