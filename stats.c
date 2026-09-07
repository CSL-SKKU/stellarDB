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

/* ---- latency series ---- */
#define LAT_BUCKETS 128
#define LAT_MAX_THREADS 512
struct lat_set {
  uint64_t count, sum_cycles;
  uint64_t hist[LAT_BUCKETS];
};
struct lat_tl {
  struct lat_set op[2]; /* [0] reads, [1] writes */
  struct lat_set stage[3]; /* [0] queue wait, [1] distributor service, [2] I/O service */
};
static struct lat_tl *lat_threads[LAT_MAX_THREADS];
static _Atomic int lat_nb_threads;
static __thread struct lat_tl *my_lat;
static struct lat_tl *lat_mine(void);

static inline unsigned lat_bucket(uint64_t us) {
  if (us < 4)
    return (unsigned)us;                         /* 0..3 exact */
  unsigned exp = 63 - __builtin_clzll(us);       /* >= 2 */
  unsigned sub = (unsigned)((us >> (exp - 2)) & 3);
  unsigned idx = exp * 4 + sub;
  return idx < LAT_BUCKETS ? idx : LAT_BUCKETS - 1;
}

static inline uint64_t lat_bucket_low_us(unsigned idx) {
  if (idx < 4)
    return idx;
  unsigned exp = idx / 4, sub = idx % 4;
  return (uint64_t)(4 + sub) << (exp - 2);
}

void lat_series_record(uint64_t cycles, int is_write) {
  struct lat_tl *t = lat_mine();

  if (t == NULL)
    return;
  struct lat_set *s = &t->op[is_write ? 1 : 0];

  s->count++;
  s->sum_cycles += cycles;
  s->hist[lat_bucket(cycles_to_us(cycles))]++;
}

static struct lat_tl *lat_mine(void) {
  struct lat_tl *t = my_lat;

  if (t == NULL) {
    int slot = atomic_fetch_add_explicit(&lat_nb_threads, 1, memory_order_relaxed);

    if (slot >= LAT_MAX_THREADS)
      return NULL;
    t = calloc(1, sizeof(*t));
    if (t == NULL)
      return NULL;
    my_lat = t;
    __atomic_store_n(&lat_threads[slot], t, __ATOMIC_RELEASE);
  }
  return t;
}

/*
 * Per-stage service times of one completed request, from the stamps the
 * enqueue/dequeue sites left on the callback (see add_time_in_payload()).
 * A request that never reached an I/O worker (a read miss answered by the
 * distributor) has no second queue and no I/O stage.
 */
void lat_series_record_stages(struct slab_callback *c, uint64_t end) {
  struct lat_tl *t = lat_mine();
  uint64_t t0 = (uint64_t)c->payload, t1 = c->t_stage[0], t2 = c->t_stage[1],
           t3 = c->t_stage[2];
  uint64_t q = 0, d = 0, io = 0;

  if (t == NULL || !t0 || !t1 || t1 < t0)
    return;
  q = t1 - t0;
  if (t2 && t3 && t2 >= t1 && t3 >= t2 && end >= t3) {
    d = t2 - t1;
    q += t3 - t2;
    io = end - t3;
  } else if (end >= t1) {
    d = end - t1;
  }
  uint64_t v[3] = {q, d, io};
  for (int k = 0; k < 3; k++) {
    struct lat_set *s = &t->stage[k];

    s->count++;
    s->sum_cycles += v[k];
    s->hist[lat_bucket(cycles_to_us(v[k]))]++;
  }
}

/* Percentiles of the interval (cur - prev) for one counter set. */
static void lat_set_stats(const struct lat_set *cur, const struct lat_set *prev,
                          uint64_t *count, uint64_t *avg, uint64_t *p50,
                          uint64_t *p99, uint64_t *p999, uint64_t *max_us) {
  uint64_t acc = 0, sum = cur->sum_cycles - prev->sum_cycles;

  *count = cur->count - prev->count;
  *avg = *count ? cycles_to_us(sum / *count) : 0;
  *p50 = *p99 = *p999 = *max_us = 0;
  for (unsigned b = 0; b < LAT_BUCKETS && *count; b++) {
    uint64_t in_bucket = cur->hist[b] - prev->hist[b];

    if (!in_bucket)
      continue;
    *max_us = lat_bucket_low_us(b);
    acc += in_bucket;
    if (!*p50 && acc * 2 >= *count) *p50 = lat_bucket_low_us(b);
    if (!*p99 && acc * 100 >= *count * 99) *p99 = lat_bucket_low_us(b);
    if (!*p999 && acc * 1000 >= *count * 999) *p999 = lat_bucket_low_us(b);
  }
}

void lat_series_report(double t_s) {
  static struct lat_set prev_all, prev_rd, prev_wr, prev_st[3];
  struct lat_set all = {0}, rd = {0}, wr = {0}, st[3] = {{0}};
  uint64_t sc[3], savg[3], sp50[3], sp99[3], sp999[3], smx[3];
  int n = atomic_load_explicit(&lat_nb_threads, memory_order_relaxed);
  uint64_t c, avg, p50, p99, p999, mx, rc, ravg, rp50, rp99, rp999, rmx, wc,
      wavg, wp50, wp99, wp999, wmx;

  if (n > LAT_MAX_THREADS)
    n = LAT_MAX_THREADS;
  for (int i = 0; i < n; i++) {
    struct lat_tl *t = __atomic_load_n(&lat_threads[i], __ATOMIC_ACQUIRE);

    if (t == NULL)
      continue;
    for (int k = 0; k < 2; k++) {
      struct lat_set *dst = k ? &wr : &rd;

      dst->count += t->op[k].count;
      dst->sum_cycles += t->op[k].sum_cycles;
      for (unsigned b = 0; b < LAT_BUCKETS; b++)
        dst->hist[b] += t->op[k].hist[b];
    }
    for (int k = 0; k < 3; k++) {
      st[k].count += t->stage[k].count;
      st[k].sum_cycles += t->stage[k].sum_cycles;
      for (unsigned b = 0; b < LAT_BUCKETS; b++)
        st[k].hist[b] += t->stage[k].hist[b];
    }
  }
  all.count = rd.count + wr.count;
  all.sum_cycles = rd.sum_cycles + wr.sum_cycles;
  for (unsigned b = 0; b < LAT_BUCKETS; b++)
    all.hist[b] = rd.hist[b] + wr.hist[b];

  lat_set_stats(&all, &prev_all, &c, &avg, &p50, &p99, &p999, &mx);
  lat_set_stats(&rd, &prev_rd, &rc, &ravg, &rp50, &rp99, &rp999, &rmx);
  lat_set_stats(&wr, &prev_wr, &wc, &wavg, &wp50, &wp99, &wp999, &wmx);
  for (int k = 0; k < 3; k++)
    lat_set_stats(&st[k], &prev_st[k], &sc[k], &savg[k], &sp50[k], &sp99[k], &sp999[k], &smx[k]);
  /* ... q_avg q_p99 dist_avg dist_p99 io_avg io_p99: the stages of the same requests */
  printf("#L %.1f %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu\n",
         t_s, c, avg, p50, p99, p999, mx, rc, ravg, rp99, wc, wavg, wp99,
         savg[0], sp99[0], savg[1], sp99[1], savg[2], sp99[2]);
  fflush(stdout);
  prev_all = all;
  prev_rd = rd;
  prev_wr = wr;
  for (int k = 0; k < 3; k++)
    prev_st[k] = st[k];
}

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
  printf("#R %s reins-on-read: reads_seen=%lu deep_and_hot=%lu deferred=%lu dropped=%lu\n",
         phase, r.reins_or_seen, r.reins_or_deep, r.reins_or_deferred, r.reins_or_dropped);
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
  printf("#R %s compaction: compactions=%lu migrations=%lu entries_dropped=%lu "
         "index_entries_freed=%lu bytes_read=%lu bytes_written=%lu "
         "bursts=%lu calls=%lu noop=%lu failed=%lu hard_cap_hits=%lu\n", phase,
         r.compactions, r.migrations, r.rebuild_entries_dropped, r.rebuild_index_freed,
         r.rebuild_bytes_read, r.rebuild_bytes_written, r.compact_bursts,
         r.compact_calls, r.compact_noop, r.compact_failed, r.compact_hard_cap_hits);
  printf("#R %s prune-pick: cold=%lu hot=%lu\n", phase, r.prune_cold_picks,
         r.prune_hot_picks);
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
  /*
   * A request is enqueued twice (client -> distributor, then distributor ->
   * I/O worker after the center-tree descent) and dequeued twice. The first
   * enqueue is the client's stamp (write-once: overwriting it dropped the
   * whole distributor stage from every latency number); the others fill the
   * stage stamps so the completion can split the latency into queue wait,
   * distributor service and I/O service.
   */
  uint64_t t;

  if (origin == TIMING_STAGE_REQUEST_ENQUEUED) {
    rdtscll(t);
    if (!c->payload) {
      c->payload = (void *)t;
      c->t_stage[0] = c->t_stage[1] = c->t_stage[2] = 0;
    } else if (!c->t_stage[1]) {
      c->t_stage[1] = t;
    }
  } else if (origin == TIMING_STAGE_REQUEST_DEQUEUED && c->payload) {
    rdtscll(t);
    if (!c->t_stage[0])
      c->t_stage[0] = t;
    else if (!c->t_stage[2])
      c->t_stage[2] = t;
  }
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
