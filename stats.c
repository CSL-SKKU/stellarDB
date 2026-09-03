#include "headers.h"
#include "utils.h"
#include "slab.h"

#define MAX_STATS 10000000LU

struct stats {
  uint64_t *timing_time;
  uint64_t *timing_value;
  size_t timing_idx;
  size_t max_timing_idx;
} stats;

void add_timing_stat(uint64_t elapsed) {
  if (!stats.timing_value) {
    stats.timing_time = malloc(MAX_STATS * sizeof(*stats.timing_time));
    stats.timing_value = malloc(MAX_STATS * sizeof(*stats.timing_value));
    stats.timing_idx = 0;
    stats.max_timing_idx = MAX_STATS;
  }
  if (stats.timing_idx >= stats.max_timing_idx) return;
  // die("Cannot collect all stats, buffer is full!\n");
  rdtscll(stats.timing_time[stats.timing_idx]);
  stats.timing_value[stats.timing_idx] = elapsed;
  stats.timing_idx++;
}

int cmp_uint(const void *_a, const void *_b) {
  uint64_t a = *(uint64_t *)_a;
  uint64_t b = *(uint64_t *)_b;
  if (a > b)
    return 1;
  else if (a < b)
    return -1;
  else
    return 0;
}

struct restructuring_stats rstats;

void rstat_max(uint64_t *slot, uint64_t v) {
  uint64_t cur = __atomic_load_n(slot, __ATOMIC_RELAXED);
  while (v > cur &&
         !__atomic_compare_exchange_n(slot, &cur, v, 1, __ATOMIC_RELAXED,
                                      __ATOMIC_RELAXED))
    ;
}

void reset_restructuring_stats(void) {
  memset(&rstats, 0, sizeof(rstats));
}

/* VmRSS / VmSize / VmHWM of this process in KiB, from /proc/self/status. */
void process_memory_kb(uint64_t *rss, uint64_t *vsz, uint64_t *hwm) {
  char line[256];
  FILE *f = fopen("/proc/self/status", "r");

  *rss = *vsz = *hwm = 0;
  if (f == NULL)
    return;
  while (fgets(line, sizeof(line), f) != NULL) {
    if (strncmp(line, "VmRSS:", 6) == 0)
      *rss = strtoull(line + 6, NULL, 10);
    else if (strncmp(line, "VmSize:", 7) == 0)
      *vsz = strtoull(line + 7, NULL, 10);
    else if (strncmp(line, "VmHWM:", 6) == 0)
      *hwm = strtoull(line + 6, NULL, 10);
  }
  fclose(f);
}

/* One block per phase; every line is "#R <phase> key=value ..." for grep. */
void print_restructuring_stats(const char *phase) {
  struct restructuring_stats r;
  memcpy(&r, &rstats, sizeof(r));
  printf("#R %s tree: nodes=%lu depth=%lu splits=%lu\n", phase,
         tnt_get_node_count(), tnt_get_depth(), r.splits);
  printf("#R %s worker: wakeups=%lu gate_open=%lu rebalance_needed=%lu\n",
         phase, r.worker_wakeups, r.worker_gate_open, r.rebalance_needed);
  printf("#R %s rebalance: calls=%lu success=%lu noop=%lu failed=%lu "
         "total_ms=%.1f max_ms=%.1f\n", phase, r.rebalance_calls,
         r.rebalance_success, r.rebalance_noop, r.rebalance_failed,
         r.rebalance_us / 1000.0, r.rebalance_max_us / 1000.0);
  printf("#R %s reinsertion: slabs_queued=%lu slabs_processed=%lu "
         "slots_examined=%lu copies_issued=%lu published=%lu abandoned=%lu\n",
         phase, r.reins_queued, r.reins_slabs, r.reins_examined,
         r.reins_issued, r.reins_published, r.reins_abandoned);
  printf("#R %s prune: bursts=%lu calls=%lu done=%lu noop=%lu dropped=%lu "
         "failed=%lu total_ms=%.1f max_ms=%.1f\n", phase, r.prune_bursts,
         r.prune_calls, r.prune_done, r.prune_noop, r.prune_dropped,
         r.prune_failed, r.prune_us / 1000.0, r.prune_max_us / 1000.0);
  {
    uint64_t rss, vsz, hwm;

    process_memory_kb(&rss, &vsz, &hwm);
    printf("#R %s mem: rss_mb=%lu vsz_mb=%lu peak_rss_mb=%lu\n", phase,
           rss / 1024, vsz / 1024, hwm / 1024);
  }
  printf("#R %s prune-stale: stale=%lu reserved=%lu ratio=%.3f\n", phase,
         r.prune_stale_last, r.prune_reserved_last,
         r.prune_reserved_last ? (double)r.prune_stale_last / r.prune_reserved_last : 0.0);
}

void print_stats(void) {
  uint64_t avg = 0;

  if (stats.timing_idx == 0) {
    printf("#No stat has been collected\n");
    return;
  }

  size_t last = stats.timing_idx;
  qsort(stats.timing_value, last, sizeof(*stats.timing_value), cmp_uint);
  for (size_t i = 0; i < last; i++) avg += stats.timing_value[i];

  printf("#Latency:\n#\tAVG - %lu us\n#\t99p - %lu us\n#\tmax - %lu us\n",
         cycles_to_us(avg / last),
         cycles_to_us(stats.timing_value[last * 99 / 100]),
         cycles_to_us(stats.timing_value[last - 1]));

  stats.timing_idx = 0;
}

struct timing_s {
  enum timing_stage origin;
  size_t time;
};

void *allocate_payload(void) {
#if DEBUG
  return calloc(20, sizeof(struct timing_s));
#else
  return NULL;
#endif
}

void add_time_in_payload(struct slab_callback *c, enum timing_stage origin) {
#if DEBUG
  struct timing_s *payload = c->payload;
  if (!payload) return;

  uint64_t t, pos = 0;
  rdtscll(t);
  while (pos < 20 && payload[pos].time) pos++;
  if (pos == 20) die("Too many times added!\n");
  payload[pos].time = t;
  payload[pos].origin = origin;
#else
  if (origin != TIMING_STAGE_REQUEST_ENQUEUED) return;
  uint64_t t;
  rdtscll(t);
  c->payload = (void *)t;
#endif
}

uint64_t get_origin_from_payload(struct slab_callback *c, size_t pos) {
#if DEBUG
  struct timing_s *payload = c->payload;
  if (!payload) return 0;
  return payload[pos].origin;
#else
  return 0;
#endif
}

uint64_t get_time_from_payload(struct slab_callback *c, size_t pos) {
#if DEBUG
  struct timing_s *payload = c->payload;
  if (!payload) return 0;
  return payload[pos].time;
#else
  return (uint64_t)c->payload;
#endif
}

void free_payload(struct slab_callback *c) {
#if DEBUG
  free(c->payload);
#endif
}
