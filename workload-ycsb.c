/*
 * YCSB Workload
 */

#include "headers.h"
#include "workload-common.h"

static char *_create_unique_item_ycsb(uint64_t uid) {
#ifdef REALKEY_FILE_PATH
  uid = get_real_key(uid);
#endif
  size_t item_size = cfg.kv_size;
  // size_t item_size = sizeof(struct item_metadata) + 2*sizeof(size_t);
  return create_unique_item(item_size, uid);
}

static char *create_unique_item_ycsb(uint64_t uid, uint64_t max_uid) {
  return _create_unique_item_ycsb(uid);
}

/* Is the current request a get or a put? */
static int random_get_put(int test) {
  long random = uniform_next() % 100;
  switch (test) {
    case 0:  // A
      return random >= 50;
    case 1:  // B
      return random >= 95;
    case 2:  // C
      return 0;
    case 3:  // E
      return random >= 95;
  }
  die("Not a valid test\n");
}

/* YCSB A (or D), B, C */
static void _launch_ycsb(int test, int nb_requests, int zipfian) {
  declare_periodic_count;
  for (size_t i = 0; i < nb_requests; i++) {
    struct slab_callback *cb = bench_cb();
    if (zipfian)
      cb->item = _create_unique_item_ycsb(zipf_next());
    else
      cb->item = _create_unique_item_ycsb(uniform_next());
    if (random_get_put(
            test)) {  // In these tests we update with a given probability
      kv_upsert_async(cb);
    } else {  // or we read
      kv_read_async(cb);
    }
    periodic_count(1000, "YCSB Load Injector (%lu%%)", i * 100LU / nb_requests);
  }
}

/* YCSB E */
static void _launch_ycsb_e(int test, int nb_requests, int zipfian) {
  declare_periodic_count;
  random_gen_t rand_next = zipfian ? zipf_next : uniform_next;
  int total_lookup = 0, total_update = 0;
 
  for (size_t i = 0; i < nb_requests; i++) {
    if (random_get_put(
            test)) {  // In this test we update with a given probability
      struct slab_callback *cb = bench_cb();
      cb->item = _create_unique_item_ycsb(rand_next());
      total_update++;
      kv_upsert_async(cb);
    } else {  // or we scan
      uint64_t start_key = rand_next();
      size_t scan_size = uniform_next()%99+1;
      // 2. key와 size를 가지고 트리에서 slab과 idx들을 가져온다.
      for (size_t j = 0; j < scan_size; j++) {
        struct slab_callback *cb = bench_cb();
        cb->item = _create_unique_item_ycsb(start_key+j);
        total_lookup++;
        kv_read_async(cb);
      }
    }
    periodic_count(1000, "YCSB Load Injector (scans) (%lu%%)",
                   i * 100LU / nb_requests);
  }
  printf("YCSB E: %d updates, %d lookups\n", total_update, total_lookup);
}

/*
 * YCSB D: 95% reads with the "latest" distribution, 5% inserts of new keys.
 * New keys are appended at the top of the key space (nb_items_in_db, +1, ...),
 * shared by all load injectors. A read picks a Zipfian rank and reads that
 * many keys below the newest one, so the most recently inserted keys are the
 * most popular. A read may target an insert still in flight and miss; that is
 * counted like any other request, as in YCSB.
 */
static _Atomic uint64_t ycsb_next_new_key;

static void _launch_ycsb_d(int nb_requests) {
  declare_periodic_count;
  int inserts = 0, reads = 0, misses_possible = 0;

  for (size_t i = 0; i < nb_requests; i++) {
    struct slab_callback *cb = bench_cb();

    if (uniform_next() % 100 >= 95) {
      uint64_t key = atomic_fetch_add_explicit(&ycsb_next_new_key, 1,
                                               memory_order_relaxed);
      cb->item = _create_unique_item_ycsb(key);
      inserts++;
      kv_upsert_async(cb);
    } else {
      uint64_t newest = atomic_load_explicit(&ycsb_next_new_key,
                                             memory_order_relaxed);
      uint64_t rank = (uint64_t)zipf_next(); /* 0 = most popular */
      uint64_t key = rank < newest ? newest - 1 - rank : 0;

      if (rank == 0) misses_possible++;
      cb->item = _create_unique_item_ycsb(key);
      reads++;
      kv_read_async(cb);
    }
    periodic_count(1000, "YCSB Load Injector (%lu%%)", i * 100LU / nb_requests);
  }
  printf("YCSB D: %d reads, %d inserts (new keys up to %lu)\n", reads, inserts,
         atomic_load_explicit(&ycsb_next_new_key, memory_order_relaxed));
}

/*
 * YCSB F: 50% reads, 50% read-modify-write. An RMW reads the record, changes a
 * byte of the value and writes the whole record back to the same key. As in
 * YCSB the two halves are separate requests with no atomicity; the latency of
 * an RMW is the time from the read's enqueue to the write's completion.
 *
 * The write is not issued from the read's completion (that runs on an I/O
 * worker, which must never block on a full distributor queue). The completion
 * parks the prepared write on a list and the load injector issues it.
 */
struct rmw_ready {
  struct slab_callback *write;
  struct rmw_ready *next;
};
static pthread_mutex_t rmw_lock = PTHREAD_MUTEX_INITIALIZER;
static struct rmw_ready *rmw_ready_list;
static _Atomic long rmw_inflight;

static void rmw_write_done(struct slab_callback *cb, void *item) {
  uint64_t end;

  (void)item;
  rdtscll(end);
  add_timing_stat(end - cb->user_start);
  if (cfg.latency_series_ms)
    lat_series_record(end - cb->user_start, 1); /* the whole RMW counts as a write */
  atomic_fetch_sub_explicit(&rmw_inflight, 1, memory_order_relaxed);
  free(cb->item);
  if (DEBUG) free_payload(cb);
  free(cb);
}

static void rmw_read_done(struct slab_callback *cb, void *item) {
  struct slab_callback *w = bench_cb();
  struct item_metadata *meta = (struct item_metadata *)cb->item;
  char *value = cb->item + sizeof(*meta) + item_key_size(meta);
  struct rmw_ready *r = malloc(sizeof(*r));

  (void)item; /* the stored record; the client rewrites its own copy */
  value[8] = (char)(value[8] + 1); /* the "modify" */
  w->item = cb->item;
  w->cb = rmw_write_done;
  w->user_start = get_time_from_payload(cb, 0);
  if (DEBUG) free_payload(cb);
  free(cb);

  r->write = w;
  pthread_mutex_lock(&rmw_lock);
  r->next = rmw_ready_list;
  rmw_ready_list = r;
  pthread_mutex_unlock(&rmw_lock);
}

static void rmw_issue_ready(void) {
  struct rmw_ready *list;

  pthread_mutex_lock(&rmw_lock);
  list = rmw_ready_list;
  rmw_ready_list = NULL;
  pthread_mutex_unlock(&rmw_lock);
  while (list) {
    struct rmw_ready *next = list->next;

    kv_upsert_async(list->write);
    free(list);
    list = next;
  }
}

static void _launch_ycsb_f(int nb_requests, int zipfian) {
  declare_periodic_count;
  int reads = 0, rmws = 0;

  for (size_t i = 0; i < nb_requests; i++) {
    struct slab_callback *cb = bench_cb();

    cb->item = _create_unique_item_ycsb(zipfian ? zipf_next() : uniform_next());
    if (uniform_next() % 100 >= 50) {
      cb->cb = rmw_read_done;
      atomic_fetch_add_explicit(&rmw_inflight, 1, memory_order_relaxed);
      rmws++;
    } else {
      reads++;
    }
    kv_read_async(cb);
    rmw_issue_ready();
    periodic_count(1000, "YCSB Load Injector (%lu%%)", i * 100LU / nb_requests);
  }
  /* Every RMW this and the other injectors started must finish its write. */
  while (atomic_load_explicit(&rmw_inflight, memory_order_relaxed) > 0) {
    rmw_issue_ready();
    usleep(100);
  }
  printf("YCSB F: %d reads, %d read-modify-writes\n", reads, rmws);
}

/* Generic interface */
static void launch_ycsb(struct workload *w, bench_t b) {
  uint64_t zero = 0;

  atomic_compare_exchange_strong(&ycsb_next_new_key, &zero, w->nb_items_in_db);
  switch (b) {
    case ycsb_a_uniform:
      return _launch_ycsb(0, w->nb_requests_per_thread, 0);
    case ycsb_b_uniform:
      return _launch_ycsb(1, w->nb_requests_per_thread, 0);
    case ycsb_c_uniform:
      return _launch_ycsb(2, w->nb_requests_per_thread, 0);
    case ycsb_e_uniform:
      return _launch_ycsb_e(3, w->nb_requests_per_thread, 0);
    case ycsb_a_zipfian:
      return _launch_ycsb(0, w->nb_requests_per_thread, 1);
    case ycsb_b_zipfian:
      return _launch_ycsb(1, w->nb_requests_per_thread, 1);
    case ycsb_c_zipfian:
      return _launch_ycsb(2, w->nb_requests_per_thread, 1);
    case ycsb_e_zipfian:
      return _launch_ycsb_e(3, w->nb_requests_per_thread, 1);
    case ycsb_d_latest:
      return _launch_ycsb_d(w->nb_requests_per_thread);
    case ycsb_f_uniform:
      return _launch_ycsb_f(w->nb_requests_per_thread, 0);
    case ycsb_f_zipfian:
      return _launch_ycsb_f(w->nb_requests_per_thread, 1);
    default:
      die("Unsupported workload\n");
  }
}

/* Pretty printing */
static const char *name_ycsb(bench_t w) {
  switch (w) {
    case ycsb_a_uniform:
      return "YCSB A - Uniform";
    case ycsb_b_uniform:
      return "YCSB B - Uniform";
    case ycsb_c_uniform:
      return "YCSB C - Uniform";
    case ycsb_e_uniform:
      return "YCSB E - Uniform";
    case ycsb_a_zipfian:
      return "YCSB A - Zipf";
    case ycsb_b_zipfian:
      return "YCSB B - Zipf";
    case ycsb_c_zipfian:
      return "YCSB C - Zipf";
    case ycsb_e_zipfian:
      return "YCSB E - Zipf";
    case ycsb_d_latest:
      return "YCSB D - Latest";
    case ycsb_f_uniform:
      return "YCSB F - Uniform";
    case ycsb_f_zipfian:
      return "YCSB F - Zipf";
    default:
      return "??";
  }
}

static int handles_ycsb(bench_t w) {
  switch (w) {
    case ycsb_a_uniform:
    case ycsb_b_uniform:
    case ycsb_c_uniform:
    case ycsb_e_uniform:
    case ycsb_a_zipfian:
    case ycsb_b_zipfian:
    case ycsb_c_zipfian:
    case ycsb_e_zipfian:
    case ycsb_d_latest:
    case ycsb_f_uniform:
    case ycsb_f_zipfian:
      return 1;
    default:
      return 0;
  }
}

static const char *api_name_ycsb(void) { return "YCSB"; }

struct workload_api YCSB = {
    .handles = handles_ycsb,
    .launch = launch_ycsb,
    .api_name = api_name_ycsb,
    .name = name_ycsb,
    .create_unique_item = create_unique_item_ycsb,
};
