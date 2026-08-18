#include "headers.h"
#include "workload-common.h"
#include "db_bench.h"
#include <inttypes.h>
#include <pthread.h>
#include <string.h>

/* One-by-one latency probe workload
 * - Submit a single GET/PUT at a time
 * - Wait until callback completes
 * - Dump per-request timing breakdown (payload stamps)
 */

typedef struct {
  pthread_mutex_t mu;
  pthread_cond_t cv;
  int done;
  FILE *output;

  uint64_t seq;
  uint64_t key;
  int op;             /* 0=GET, 1=PUT */
  uint64_t total_us;  /* end-to-end */
  uint64_t s[10];     /* absolute microseconds since request start */
  uint32_t cached;
  uint32_t upward;
  uint32_t scount;
} lat_ctx_t;

/* Store ctx pointer in cb->fsst_slab (unused in this workload) */
static inline lat_ctx_t *ctx_of_cb(struct slab_callback *cb) {
  return (lat_ctx_t *)cb->fsst_slab;
}

void add_cached_in_lat_ctx(struct slab_callback *cb, uint32_t cached) {
  if (cfg.api != &LATPROBE || cb->fsst_slab == NULL) return;
  lat_ctx_t *ctx = ctx_of_cb(cb);
  ctx->cached = cached;
}

void add_upward_in_lat_ctx(struct slab_callback *cb, uint32_t upward) {
  if (cfg.api != &LATPROBE || cb->fsst_slab == NULL) return;
  lat_ctx_t *ctx = ctx_of_cb(cb);
  ctx->upward = upward;
}

void add_scount_in_lat_ctx(struct slab_callback *cb, uint32_t count) {
  if (cfg.api != &LATPROBE || cb->fsst_slab == NULL) return;
  lat_ctx_t *ctx = ctx_of_cb(cb);
  ctx->scount = count;
}

/* Callback: compute breakdown, signal latch, free cb/item */
static void latprobe_cb(struct slab_callback *cb, void *item) {
  (void)item;
  lat_ctx_t *ctx = ctx_of_cb(cb);
  uint64_t start = get_time_from_payload(cb, 0);
  uint64_t end;
  rdtscll(end);
  ctx->total_us = cycles_to_us(end - start);

  /* Collect absolute times since start for stamps 1..8 (if present) */
  for (int i = 1; i <= 8; i++) {
    uint64_t t = get_time_from_payload(cb, i);
    ctx->s[i] = t >= start ? cycles_to_us(t - start) : 0;
  }

  fprintf(ctx->output, "%" PRIu64 ",%s,%" PRIu64 ",%" PRIu64
                       ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
                       ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
                       ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 "\n",
          ctx->seq, (ctx->op ? "PUT" : "GET"), ctx->key, ctx->total_us,
          ctx->s[1], ctx->s[2], ctx->s[3], ctx->s[4], ctx->s[5],
          ctx->s[6], ctx->s[7], ctx->s[8], ctx->cached, ctx->scount,
          ctx->upward);

  /* Signal done */
  pthread_mutex_lock(&ctx->mu);
  ctx->done = 1;
  pthread_cond_signal(&ctx->cv);
  pthread_mutex_unlock(&ctx->mu);

  /* Free resources like compute_stats would */
  if (cb->cb_cb == compute_stats || !cb->cb_cb) free(cb->item);
  free_payload(cb);
  if (cb->cb_cb == compute_stats || !cb->cb_cb) free(cb);
}

/* Create item same way as YCSB does */
static char *_create_unique_item_latprobe(uint64_t uid) {
#ifdef REALKEY_FILE_PATH
  uid = get_real_key(uid);
#endif
  size_t item_size = cfg.kv_size;
  return create_unique_item(item_size, uid);
}

static char *create_unique_item_latprobe(uint64_t uid, uint64_t max_uid) {
  (void)max_uid;
  return _create_unique_item_latprobe(uid);
}

/* GET/PUT ratio using YCSB A/B/C semantics */
static int random_get_put_ratio(int test) {
  long r = uniform_next() % 100;
  switch (test) {
    case 0:  /* A */ return r >= 50;
    case 1:  /* B */ return r >= 95;
    case 2:  /* C */ return 0;
    default: return r >= 50;
  }
}

/* Init: just seed the query decider if you want A/B/C;
 * we’ll keep it simple with GET-only unless A/B/C is asked. */
static void init_latprobe(struct workload *w, bench_t b) {
  (void)w;
  (void)b;
  init_rand();
#if !DEBUG
  fprintf(stderr,
          "WARN: build with STELLAR_DEBUG=1 to collect latency stage timestamps\n");
#endif
}

/* Issue exactly one op, wait for completion, emit CSV */
static void issue_one_and_wait(uint64_t seq, uint64_t key, int do_put,
                               FILE *output) {
  lat_ctx_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  pthread_mutex_init(&ctx.mu, NULL);
  pthread_cond_init(&ctx.cv, NULL);
  ctx.output = output;
  ctx.seq = seq;
  ctx.key = key;
  ctx.op = do_put ? 1 : 0;

  struct slab_callback *cb = bench_cb();
  cb->cb = latprobe_cb;
  cb->fsst_slab = (struct slab *)&ctx;
  cb->item = _create_unique_item_latprobe(key);

  pthread_mutex_lock(&ctx.mu);
  if (do_put) kv_update_async(cb);
  else        kv_read_async(cb);
  while (!ctx.done) pthread_cond_wait(&ctx.cv, &ctx.mu);
  pthread_mutex_unlock(&ctx.mu);

  pthread_cond_destroy(&ctx.cv);
  pthread_mutex_destroy(&ctx.mu);
}

/* Launch: one-by-one requests. Recommend nb_load_injectors=1 for strict sequencing. */
static void launch_latprobe(struct workload *w, bench_t b) {
  (void)b;
  /* main.c selects one injector so callbacks complete in request order. */
  if (w->nb_load_injectors != 1) {
    die("latprobe requires exactly one load injector\n");
  }

  /* Use YCSB-C semantics (GET-only) for the latency probe. */
  int ycsb_mix = 2; /* 0=A, 1=B, 2=C */
  const uint64_t total = w->nb_items_in_db;
  const uint64_t N = w->nb_requests_per_thread;

  /* Header to stdout; change to a file if desired */
  FILE *fp = stdout;
  fprintf(fp, "seq,op,key,total_us,s1_us,s2_us,s3_us,s4_us,s5_us,"
              "s6_us,s7_us,s8_us,cached,subtree_count,upward_steps\n");

  for (uint64_t i = 0; i < N; i++) {
    uint64_t key = uniform_next() % total;           /* or zipf_next() if desired */
    int do_put = random_get_put_ratio(ycsb_mix);     /* A/B/C ratio */

    issue_one_and_wait(i, key, do_put, fp);

    if ((i % 1000) == 0) fflush(fp);
  }
  fflush(fp);
}

/* Names/handles */
static const char *name_latprobe(bench_t w) {
  switch (w) {
    case latprobe: return "LATENCY PROBE (one-by-one)";
    default: return "LATENCY PROBE (?)";
  }
}

static int handles_latprobe(bench_t w) {
  return (w == latprobe);
}

static const char *api_name_latprobe(void) { return "LATPROBE"; }

/* Wire as a workload_api */
struct workload_api LATPROBE = {
    .init = init_latprobe,
    .handles = handles_latprobe,
    .launch = launch_latprobe,
    .api_name = api_name_latprobe,
    .name = name_latprobe,
    .create_unique_item = create_unique_item_latprobe,
};
