/*
 * BGWORK Workload
 */

#include "headers.h"
#include "workload-common.h"

static char *_create_unique_item_bgwork(uint64_t uid) {
  uid = get_old_key(uid);
  size_t item_size = cfg.kv_size;
  // size_t item_size = sizeof(struct item_metadata) + 2*sizeof(size_t);
  return create_unique_item(item_size, uid);
}

static char *create_unique_item_bgwork(uint64_t uid, uint64_t max_uid) {
  return _create_unique_item_bgwork(uid);
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

static void _launch_bgwork(int test, int nb_requests) {
  declare_periodic_count;
  for (size_t i = 0; i < nb_requests; i++) {
    struct slab_callback *cb = bench_cb();
    cb->item = _create_unique_item_bgwork(zipf_next());
    if (random_get_put(
            test)) {  // In these tests we update with a given probability
      kv_upsert_async(cb);
    } else {  // or we read
      kv_read_async(cb);
    }
    periodic_count(1000, "BGWORK Load Injector (%lu%%)", i * 100LU / nb_requests);
  }
}

static void _launch_bgwork_e(int test, int nb_requests) {
  declare_periodic_count;
  random_gen_t rand_next = zipf_next;
  int total_lookup = 0, total_update = 0;
 
  for (size_t i = 0; i < nb_requests; i++) {
    if (random_get_put(
            test)) {  // In this test we update with a given probability
      struct slab_callback *cb = bench_cb();
      cb->item = _create_unique_item_bgwork(rand_next());
      total_update++;
      kv_upsert_async(cb);
    } else {  // or we scan
      uint64_t start_key = rand_next();
      size_t scan_size = uniform_next()%99+1;
      // 2. key와 size를 가지고 트리에서 slab과 idx들을 가져온다.
      for (size_t j = 0; j < scan_size; j++) {
        struct slab_callback *cb = bench_cb();
        cb->item = _create_unique_item_bgwork(start_key+j);
        total_lookup++;
        kv_read_async(cb);
      }
    }
    periodic_count(1000, "BGWORK Load Injector (scans) (%lu%%)",
                   i * 100LU / nb_requests);
  }
  printf("BGWORK YCSB E: %d updates, %d lookups\n", total_update, total_lookup);
}

/* Generic interface */
static void launch_bgwork(struct workload *w, bench_t b) {
  switch (b) {
    case ycsb_a_zipfian:
      return _launch_bgwork(0, w->nb_requests_per_thread);
    case ycsb_b_zipfian:
      return _launch_bgwork(1, w->nb_requests_per_thread);
    case ycsb_c_zipfian:
      return _launch_bgwork(2, w->nb_requests_per_thread);
    case ycsb_e_zipfian:
      return _launch_bgwork_e(3, w->nb_requests_per_thread);
    default:
      die("Unsupported workload\n");
  }
}

/* Pretty printing */
static const char *name_bgwork(bench_t w) {
  switch (w) {
    case ycsb_a_zipfian:
      return "YCSB A - BGWORK";
    case ycsb_b_zipfian:
      return "YCSB B - BGWORK";
    case ycsb_c_zipfian:
      return "YCSB C - BGWORK";
    case ycsb_e_zipfian:
      return "YCSB E - BGWORK";
    default:
      return "??";
  }
}

static int handles_bgwork(bench_t w) {
  switch (w) {
    case ycsb_a_zipfian:
    case ycsb_b_zipfian:
    case ycsb_c_zipfian:
    case ycsb_e_zipfian:
      return 1;
    default:
      return 0;
  }
}

static const char *api_name_bgwork(void) { return "BGWORK"; }

struct workload_api BGWORK = {
    .handles = handles_bgwork,
    .launch = launch_bgwork,
    .api_name = api_name_bgwork,
    .name = name_bgwork,
    .create_unique_item = create_unique_item_bgwork,
};
