#ifndef STATS_H
#define STATS_H 1

struct slab_callback;

enum timing_stage {
  TIMING_STAGE_INVALID = -1,
  TIMING_STAGE_REQUEST_ENQUEUED = 0,
  TIMING_STAGE_REQUEST_DEQUEUED,
  TIMING_STAGE_LEAF_FOUND,
  TIMING_STAGE_INDEX_LOOKUP_DONE,
  TIMING_STAGE_STORAGE_TARGET_READY,
  TIMING_STAGE_IO_SUBMIT,
  TIMING_STAGE_IO_COMPLETE,
  TIMING_STAGE_NEW_INDEX_PUBLISHED,
  TIMING_STAGE_INVALIDATION_QUEUED,
  TIMING_STAGE_REQUEST_COMPLETE,
};

/*
 * Restructuring counters: did each mechanism actually fire, how often, and how
 * long it held the tree. Relaxed atomics, bumped from any thread.
 */
struct restructuring_stats {
  uint64_t splits;
  uint64_t worker_wakeups, worker_gate_open, rebalance_needed;
  uint64_t rebalance_calls, rebalance_success, rebalance_noop, rebalance_failed;
  uint64_t rebalance_us, rebalance_max_us;
  uint64_t reins_queued, reins_slabs, reins_examined, reins_issued;
  uint64_t reins_published, reins_abandoned;
  uint64_t prune_bursts, prune_calls, prune_done, prune_noop, prune_dropped;
  uint64_t prune_failed, prune_us, prune_max_us;
  uint64_t prune_stale_last, prune_reserved_last; /* last -p measurement */
  /* migration */
  uint64_t migrations, rebuild_entries_dropped, rebuild_index_freed,
      rebuild_bytes_read, rebuild_bytes_written;
  uint64_t migrate_calls, migrate_noop, migrate_failed;
  uint64_t prune_cold_picks, prune_hot_picks; /* ILI candidate had 0 / >0 recent writes */
  uint64_t reins_or_seen, reins_or_deep, reins_or_deferred, reins_or_dropped; /* on-read */
  uint64_t stale_queued, stale_processed, stale_invalidated, stale_skipped;
  uint64_t stale_dropped, stale_queue_max, stale_lag_max_ms;
};
extern struct restructuring_stats rstats;
#define RSTAT_ADD(field, n) __atomic_fetch_add(&rstats.field, (uint64_t)(n), __ATOMIC_RELAXED)
#define RSTAT_INC(field) RSTAT_ADD(field, 1)
void rstat_max(uint64_t *slot, uint64_t v);
void print_restructuring_stats(const char *phase);
void process_memory_kb(uint64_t *rss, uint64_t *vsz, uint64_t *hwm);
void reset_restructuring_stats(void);

void add_timing_stat(uint64_t elapsed);

/*
 * Opt-in per-interval latency series (--latency-series). Completions update a
 * thread-local counter set; the sampler thread diffs the totals per interval
 * and prints "#L t_s count avg_us p50_us p99_us p999_us max_us". Percentiles
 * come from a log histogram (4 buckets per octave), so they are approximate.
 */
void lat_series_record(uint64_t cycles, int is_write);
void lat_series_record_stages(struct slab_callback *c, uint64_t end);
void lat_series_report(double t_s);
void print_stats(void);

uint64_t cycles_to_us(uint64_t cycles);

void *allocate_payload(void);
void free_payload(struct slab_callback *c);
void add_time_in_payload(struct slab_callback *c, enum timing_stage origin);
uint64_t get_time_from_payload(struct slab_callback *c, size_t pos);
uint64_t get_origin_from_payload(struct slab_callback *c, size_t pos);
#endif
