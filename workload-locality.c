#include "headers.h"
#include "workload-common.h"
#include "db_bench.h"
#include <math.h>

typedef struct {
  int64_t keyrange_start;
  int64_t keyrange_access;
  int64_t keyrange_keys;
} KeyrangeUnit;

static void *gen_exp;
static void *query;
//static int64_t keyrange_rand_max_ = 0;
//static int64_t keyrange_size_ = 0;
//static int64_t keyrange_num_ = 0;
//static KeyrangeUnit *keyrange_set_ = NULL;

static void init_locality(struct workload *w, bench_t b) {
  switch (b) {
    case locality_keyspace:
    case locality_both:
      gen_exp = init_exp_prefix(w->nb_items_in_db, 30, 14.18, -2.917, 0.0164,
                                -0.08082);
      query = init_query(0.83, 0.14, 0.03);
      init_rand();
      return;
    case locality_random:
    case locality_temporal:
      gen_exp = init_exp_prefix(w->nb_items_in_db, 1, 0, 0, 0,
                                0);
      query = init_query(0.83, 0.14, 0.03);
      init_rand();
      return;
    default:
      return;
  }
}

static char *_create_unique_item_locality(uint64_t uid) {
#ifdef REALKEY_FILE_PATH
  uid = get_real_key(uid);
#endif
  size_t item_size = cfg.kv_size;
  // size_t item_size = sizeof(struct item_metadata) + 2*sizeof(size_t);
  return create_unique_item(item_size, uid);
}

static char *create_unique_item_locality(uint64_t uid, uint64_t max_uid) {
  return _create_unique_item_locality(uid);
}

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

static void _launch_locality(int total, int nb_requests, double dist_a,
                            double dist_b, int range_num, double range_a,
                            double range_b, double range_c, double range_d) {
  declare_periodic_count;
  unsigned char prefix_model = 0, random_model = 0;
  int query_type;
  //void *rand_var;
  int query_count_get = 0;
  int query_count_put = 0;
  int query_count_scan = 0;

  if (range_a != 0.0 || range_b != 0.0 || range_c != 0.0 || range_d != 0.0) {
    prefix_model = 1;
  }

  if (dist_a == 0 || dist_b == 0) random_model = 1;

  // create_rand(rand_var);
  for (size_t i = 0; i < nb_requests; i++) {
    uint64_t ini_rand, rand_v, key_rand, key_seed;
    double u;

    // ini_rand = GetRandomKey(rand_var, total);
    ini_rand = rand_next() % total;
    rand_v = ini_rand % total;
    u = (double)(rand_v) / total;
    if (prefix_model) {
      key_rand = prefix_get_key(gen_exp, ini_rand, dist_a, dist_b);
    } else if (random_model) {
      key_rand = ini_rand;
    } else {
      key_seed = (int64_t)ceil(pow((u / dist_a), (1 / dist_b)));
      uint64_t r = rand_next_seed(key_seed);
      uint64_t key_rand_u = r % (uint64_t)total;
      key_rand = (int64_t)key_rand_u;

      //key_rand = (int64_t)(rand_next_seed(key_seed)) % total;
    }
    //printf("KEY %ld\n", key_rand);

    //query_type = query_get_type(query, rand_v);
    query_type = random_get_put(2);

    
    if (query_type == 0) {
      struct slab_callback *cb = bench_cb();
      cb->item = _create_unique_item_locality(key_rand);
      query_count_get++;
      kv_read_async(cb);
    } else if (query_type == 1) {
      struct slab_callback *cb = bench_cb();
      cb->item = _create_unique_item_locality(key_rand);
      query_count_put++;
      kv_update_async(cb);
    } else if (query_type == 2) {
      int scan_len_max = 10000;
      int64_t scan_length =
        ParetoCdfInversion(u, 0.0, 2.517,
                           14.236) % scan_len_max;
      /*std::cout << "scan len: " << scan_length << "\n";*/
      for (size_t j = 0; j < scan_length; j++) {
        struct slab_callback *cb = bench_cb();
        cb->item = _create_unique_item_locality(key_rand+j);
        kv_read_async(cb);
      }
      query_count_scan++;
    }
    periodic_count(1000, "LOCALITY Load Injector (%lu%%)",
                   i * 100LU / nb_requests);
  }
  printf("LOCALITY: %d updates, %d lookups, %d scans\n", query_count_put, query_count_get, query_count_scan);
}

/* Generic interface */
static void launch_locality(struct workload *w, bench_t b) {
  switch (b) {
    case locality_random:
      return _launch_locality(w->nb_items_in_db, w->nb_requests_per_thread, 0, 0,
                             1, 0, 0, 0, 0);
    case locality_temporal:
      return _launch_locality(w->nb_items_in_db, w->nb_requests_per_thread,
                             0.002312, 0.3467, 1, 0, 0, 0, 0);
    case locality_keyspace:
      return _launch_locality(w->nb_items_in_db, w->nb_requests_per_thread, 0, 0,
                             30, 14.18, -2.917, 0.0164, -0.08082);
    case locality_both:
      return _launch_locality(w->nb_items_in_db, w->nb_requests_per_thread,
                             0.002312, 0.3467, 30, 14.18, -2.917, 0.0164,
                             -0.08082);
    default:
      die("Unsupported workload\n");
  }
}

/* Pretty printing */
static const char *name_locality(bench_t w) {
  switch (w) {
    case locality_random:
      return "LOCALITY - RANDOM";
    case locality_temporal:
      return "LOCALITY - TEMPORAL";
    case locality_keyspace:
      return "LOCALITY - KEYSPACE";
    case locality_both:
      return "LOCALITY - BOTH";
    default:
      return "??";
  }
}

static int handles_locality(bench_t w) {
  switch (w) {
    case locality_random:
    case locality_temporal:
    case locality_keyspace:
    case locality_both:
      return 1;
    default:
      return 0;
  }
}

static const char *api_name_locality(void) { return "LOCALITY"; }

struct workload_api LOCALITY = {
    .init = init_locality,
    .handles = handles_locality,
    .launch = launch_locality,
    .api_name = api_name_locality,
    .name = name_locality,
    .create_unique_item = create_unique_item_locality,
};
