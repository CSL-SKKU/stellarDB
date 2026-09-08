#include "headers.h"
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <time.h>

uint64_t report_mask;
static FILE *output;
static uint64_t interval_ns, began_ns, previous_ns;
static _Atomic int active;
static _Atomic uint64_t events[REPORT_METRICS - REPORT_REBALANCE];
static const char *names[REPORT_METRICS] = {
  "throughput_rps", "latency_avg_ms", "latency_p99_ms", "entries_total",
  "entries_stale", "entries_live_tombstones", "entries_live_normal", "nodes",
  "max_depth", "upward_hops_avg", "downward_hops_avg",
  "rebalance_attempts", "reinsertion_attempts",
  "pruning_successes", "migration_successes"
};

/* Sixteen buckets per power of two; upper bounds overestimate by <= 6.25%.
 * Each completion thread owns a bounded histogram. Its lock also makes an
 * interval snapshot consistent with its count and sum. No raw samples. */
#define HIST_BUCKETS 1024
struct measurements {
  uint64_t count, sum_ns, hist[HIST_BUCKETS];
  uint64_t reads, upward_hops, downward_hops;
};
struct reporter {
  pthread_mutex_t lock;
  uint64_t count, sum_ns;
  uint64_t reads, upward_hops, downward_hops;
  uint64_t *hist;
  struct reporter *next;
};
static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;
static struct reporter *reporters;
static __thread struct reporter *mine;
static pthread_t sampler;
static pthread_mutex_t timer_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t timer_cond;
static int stopping, sampler_started;

uint64_t report_now_ns(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

static unsigned bucket(uint64_t ns) {
  if (ns < 16) return ns;
  unsigned exponent = 63 - __builtin_clzll(ns);
  return exponent * 16 + ((ns >> (exponent - 4)) & 15);
}
static uint64_t bucket_upper(unsigned b) {
  if (b < 16) return b;
  unsigned exponent = b / 16;
  if (exponent == 63 && b % 16 == 15) return UINT64_MAX;
  return ((uint64_t)(17 + b % 16) << (exponent - 4)) - 1;
}
uint64_t report_request_start(void) {
  return (report_enabled(REPORT_LATENCY_AVG) ||
          report_enabled(REPORT_LATENCY_P99)) ? report_now_ns() : 0;
}
static void ensure_reporter(void) {
  if (!mine) {
    mine = calloc(1, sizeof(*mine));
    if (!mine) die("Cannot allocate report histogram\n");
    if (report_enabled(REPORT_LATENCY_P99)) {
      mine->hist = calloc(HIST_BUCKETS, sizeof(*mine->hist));
      if (!mine->hist) die("Cannot allocate report histogram\n");
    }
    pthread_mutex_init(&mine->lock, NULL);
    pthread_mutex_lock(&registry_lock);
    mine->next = reporters;
    reporters = mine;
    pthread_mutex_unlock(&registry_lock);
  }
}
void report_read_hops(uint64_t upward, uint64_t downward) {
  if (!report_read_hops_enabled() ||
      !atomic_load_explicit(&active, memory_order_relaxed)) return;
  ensure_reporter();
  pthread_mutex_lock(&mine->lock);
  mine->reads++;
  if (report_enabled(REPORT_UPWARD_HOPS_AVG)) mine->upward_hops += upward;
  if (report_enabled(REPORT_DOWNWARD_HOPS_AVG)) mine->downward_hops += downward;
  pthread_mutex_unlock(&mine->lock);
}
void report_request_complete(uint64_t start_ns) {
  if (!(report_mask & 7) || !atomic_load_explicit(&active, memory_order_relaxed))
    return;
  uint64_t elapsed = start_ns ? report_now_ns() - start_ns : 0;
  ensure_reporter();
  pthread_mutex_lock(&mine->lock);
  mine->count++;
  if (report_enabled(REPORT_LATENCY_AVG)) mine->sum_ns += elapsed;
  if (report_enabled(REPORT_LATENCY_P99)) mine->hist[bucket(elapsed)]++;
  pthread_mutex_unlock(&mine->lock);
}
void report_event(enum report_metric metric) {
  if (report_enabled(metric) && atomic_load_explicit(&active, memory_order_relaxed))
    atomic_fetch_add_explicit(&events[metric - REPORT_REBALANCE], 1, memory_order_relaxed);
}

static char *trim(char *s) {
  while (isspace((unsigned char)*s)) s++;
  char *end = s + strlen(s);
  while (end > s && isspace((unsigned char)end[-1])) *--end = 0;
  return s;
}
int report_init(const char *path, const char *config, double seconds) {
  report_mask = path && strcmp(path, "none") ? (UINT64_C(1) << REPORT_METRICS) - 1 : 0;
  if (!report_mask) return 0;
  if (config) {
    FILE *f = fopen(config, "r");
    if (!f) { perror(config); return -1; }
    char *line = NULL;
    size_t capacity = 0, number = 0;
    while (getline(&line, &capacity, f) >= 0) {
      number++;
      char *comment = strchr(line, '#');
      if (comment) *comment = 0;
      char *key = trim(line);
      if (!*key) continue;
      char *value = strchr(key, '=');
      if (!value) goto invalid;
      *value++ = 0;
      key = trim(key); value = trim(value);
      int enabled;
      if (!strcmp(value, "1") || !strcmp(value, "true")) enabled = 1;
      else if (!strcmp(value, "0") || !strcmp(value, "false")) enabled = 0;
      else goto invalid;
      if (!strcmp(key, "all")) {
        report_mask = enabled ? (UINT64_C(1) << REPORT_METRICS) - 1 : 0;
        continue;
      }
      unsigned metric;
      for (metric = 0; metric < REPORT_METRICS; metric++)
        if (!strcmp(key, names[metric])) break;
      if (metric == REPORT_METRICS) goto invalid;
      if (enabled) report_mask |= UINT64_C(1) << metric;
      else report_mask &= ~(UINT64_C(1) << metric);
      continue;
invalid:
      fprintf(stderr, "%s:%zu: expected a report metric = true/false (or 1/0)\n", config, number);
      free(line); fclose(f); return -1;
    }
    int error = ferror(f);
    free(line); fclose(f);
    if (error) { perror(config); return -1; }
  }
  output = fopen(path, "wx");
  if (!output) { perror(path); return -1; }
  interval_ns = (uint64_t)(seconds * 1e9);
  pthread_condattr_t attr;
  pthread_condattr_init(&attr);
  pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
  pthread_cond_init(&timer_cond, &attr);
  pthread_condattr_destroy(&attr);
  fputs("time_s", output);
  for (int i = 0; i < REPORT_METRICS; i++)
    if (report_enabled(i)) fprintf(output, ",%s", names[i]);
  fputc('\n', output);
  return 0;
}

static void write_row(uint64_t now) {
  struct measurements sum = {0};
  pthread_mutex_lock(&registry_lock);
  for (struct reporter *r = reporters; r; r = r->next) {
    pthread_mutex_lock(&r->lock);
    sum.count += r->count;
    sum.sum_ns += r->sum_ns;
    sum.reads += r->reads;
    sum.upward_hops += r->upward_hops;
    sum.downward_hops += r->downward_hops;
    if (report_enabled(REPORT_LATENCY_P99))
      for (unsigned b = 0; b < HIST_BUCKETS; b++) sum.hist[b] += r->hist[b];
    r->count = r->sum_ns = 0;
    r->reads = r->upward_hops = r->downward_hops = 0;
    if (r->hist) memset(r->hist, 0, HIST_BUCKETS * sizeof(*r->hist));
    pthread_mutex_unlock(&r->lock);
  }
  pthread_mutex_unlock(&registry_lock);
  uint64_t counts[REPORT_METRICS] = {0}, marked = 0, tombstones = 0;
  if (report_mask & (UINT64_C(15) << REPORT_ENTRIES_TOTAL)) {
    tnt_report_entries(&counts[REPORT_ENTRIES_TOTAL], &marked, &tombstones);
    counts[REPORT_ENTRIES_STALE] = marked;
    counts[REPORT_ENTRIES_TOMBSTONES] = tombstones;
    counts[REPORT_ENTRIES_NORMAL] = counts[REPORT_ENTRIES_TOTAL] - marked - tombstones;
  }
  if (report_enabled(REPORT_NODES)) counts[REPORT_NODES] = tnt_get_node_count();
  if (report_enabled(REPORT_DEPTH)) counts[REPORT_DEPTH] = tnt_get_depth();
  for (int i = REPORT_REBALANCE; i < REPORT_METRICS; i++)
    if (report_enabled(i)) counts[i] = atomic_load_explicit(&events[i - REPORT_REBALANCE], memory_order_relaxed);
  double p99 = 0;
  uint64_t accumulated = 0, rank = sum.count - sum.count / 100;
  if (sum.count && report_enabled(REPORT_LATENCY_P99))
    for (unsigned b = 0; b < HIST_BUCKETS; b++) {
      accumulated += sum.hist[b];
      if (accumulated >= rank) { p99 = bucket_upper(b) / 1e6; break; }
    }
  fprintf(output, "%.9f", (now - began_ns) / 1e9);
  for (int i = 0; i < REPORT_METRICS; i++) {
    if (!report_enabled(i)) continue;
    fputc(',', output);
    if (i == REPORT_THROUGHPUT)
      fprintf(output, "%.6f", now > previous_ns ? sum.count * 1e9 / (now - previous_ns) : 0);
    else if (i == REPORT_LATENCY_AVG) {
      if (sum.count) fprintf(output, "%.9f", sum.sum_ns / 1e6 / sum.count);
    } else if (i == REPORT_LATENCY_P99) {
      if (sum.count) fprintf(output, "%.9f", p99);
    } else if (i == REPORT_UPWARD_HOPS_AVG || i == REPORT_DOWNWARD_HOPS_AVG) {
      uint64_t hops = i == REPORT_UPWARD_HOPS_AVG ? sum.upward_hops : sum.downward_hops;
      if (sum.reads) fprintf(output, "%.9f", (double)hops / sum.reads);
    } else fprintf(output, "%" PRIu64, counts[i]);
  }
  fputc('\n', output);
  if (fflush(output) != 0) die("Cannot write report: %s\n", strerror(errno));
  previous_ns = now;
}
static void *sample(void *unused) {
  (void)unused;
  pthread_mutex_lock(&timer_lock);
  uint64_t next = began_ns + interval_ns;
  while (!stopping) {
    struct timespec deadline = {.tv_sec = next / 1000000000, .tv_nsec = next % 1000000000};
    int rc = pthread_cond_timedwait(&timer_cond, &timer_lock, &deadline);
    if (stopping) break;
    if (rc == ETIMEDOUT) {
      uint64_t at = report_now_ns();
      pthread_mutex_unlock(&timer_lock);
      write_row(at);
      pthread_mutex_lock(&timer_lock);
      /* Skip missed deadlines instead of emitting an unbounded catch-up burst. */
      uint64_t now = report_now_ns();
      next += ((now - next) / interval_ns + 1) * interval_ns;
    }
  }
  pthread_mutex_unlock(&timer_lock);
  return NULL;
}
void report_begin(void) {
  if (!output) return;
  for (int i = 0; i < REPORT_METRICS - REPORT_REBALANCE; i++) atomic_store(&events[i], 0);
  pthread_mutex_lock(&timer_lock);
  stopping = 0;
  if (interval_ns) {
    int error = pthread_create(&sampler, NULL, sample, NULL);
    if (error) die("Cannot start report sampler: %s\n", strerror(error));
    sampler_started = 1;
  }
  began_ns = previous_ns = report_now_ns();
  atomic_store(&active, 1);
  pthread_mutex_unlock(&timer_lock);
}
void report_finish(void) {
  if (!output) return;
  /* Freeze the measured duration before joining a sampler that may be
   * waiting for a maintenance snapshot. Report finalization is outside it. */
  pthread_mutex_lock(&timer_lock);
  uint64_t end = report_now_ns();
  atomic_store(&active, 0);
  stopping = 1;
  pthread_cond_signal(&timer_cond);
  pthread_mutex_unlock(&timer_lock);
  if (sampler_started) {
    pthread_join(sampler, NULL);
    sampler_started = 0;
  }
  write_row(end);
}
void report_close(void) {
  if (output && fclose(output) != 0) die("Cannot close report: %s\n", strerror(errno));
  output = NULL;
}

#ifdef STELLAR_TESTING
struct restructuring_stats rstats;
void reset_restructuring_stats(void) { memset(&rstats, 0, sizeof(rstats)); }
#endif
