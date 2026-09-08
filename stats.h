#ifndef STATS_H
#define STATS_H
struct slab_callback;
#include "report.h"
/* Correctness observations are absent from the official benchmark build. */
#ifdef STELLAR_TESTING
struct restructuring_stats {
  uint64_t worker_wakeups, prune_calls, prune_noop;
  uint64_t migration_calls, prune_stale_scans, prune_write_scans;
  uint64_t reins_or_seen, reins_or_deep, reins_examined, reins_issued, reins_published;
  uint64_t stale_queued, stale_processed, stale_skipped, stale_dropped;
};
extern struct restructuring_stats rstats;
#define TEST_STAT_ADD(field, n) __atomic_fetch_add(&rstats.field, (uint64_t)(n), __ATOMIC_RELAXED)
void reset_restructuring_stats(void);
#else
#define TEST_STAT_ADD(field, n) ((void)0)
#endif
#define TEST_STAT_INC(field) TEST_STAT_ADD(field, 1)
#endif
