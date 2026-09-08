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

static void init_dbbench(struct workload *w, bench_t b) {
  switch (b) {
    case dbbench_prefix_random:
    case dbbench_prefix_dist:
      gen_exp = init_exp_prefix(w->nb_items_in_db, 30, 14.18, -2.917, 0.0164,
                                -0.08082);
      query = init_query(0.83, 0.14, 0.03);
      init_rand();
      return;
    case dbbench_all_random:
    case dbbench_all_dist:
    default:
      return;
  }
}

static char *_create_unique_item_dbbench(uint64_t uid) {
#ifdef REALKEY_FILE_PATH
  uid = get_real_key(uid);
#endif
  size_t item_size = cfg.kv_size;
  // size_t item_size = sizeof(struct item_metadata) + 2*sizeof(size_t);
  return create_unique_item(item_size, uid);
}

static char *create_unique_item_dbbench(uint64_t uid, uint64_t max_uid) {
  return _create_unique_item_dbbench(uid);
}

static void _launch_dbbench(int total, int nb_requests, double dist_a,
                            double dist_b, int range_num, double range_a,
                            double range_b, double range_c, double range_d) {

  unsigned char prefix_model = 0, random_model = 0;
  int query_type;
  //void *rand_var;

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
      key_rand = (int64_t)(rand_next_seed(key_seed)) % total;
    }
    // printf("KEY %ld\n", key_rand);

    query_type = query_get_type(query, rand_v);

    if (query_type == 0) {
      struct slab_callback *cb = bench_cb();
      cb->item = _create_unique_item_dbbench(key_rand);

      bench_read(cb);
    } else if (query_type == 1) {
      struct slab_callback *cb = bench_cb();
      cb->item = _create_unique_item_dbbench(key_rand);

      bench_upsert(cb);
    } else if (query_type == 2) {
      int scan_len_max = 10000;
      int64_t scan_length =
        ParetoCdfInversion(u, 0.0, 2.517,
                           14.236) % scan_len_max;
      /*std::cout << "scan len: " << scan_length << "\n";*/
      bench_group_begin(scan_length);
      for (size_t j = 0; j < scan_length; j++) {
        struct slab_callback *cb = bench_cb();
        cb->item = _create_unique_item_dbbench(key_rand+j);
        bench_read(cb);
      }
      bench_group_end();

    }

  }

}

/* Generic interface */
static void launch_dbbench(struct workload *w, bench_t b) {
  switch (b) {
    case dbbench_all_random:
      return _launch_dbbench(w->nb_items_in_db, w->nb_requests_per_thread, 0, 0,
                             1, 0, 0, 0, 0);
    case dbbench_all_dist:
      return _launch_dbbench(w->nb_items_in_db, w->nb_requests_per_thread,
                             0.002312, 0.3467, 1, 0, 0, 0, 0);
    case dbbench_prefix_random:
      return _launch_dbbench(w->nb_items_in_db, w->nb_requests_per_thread, 0, 0,
                             30, 14.18, -2.917, 0.0164, -0.08082);
    case dbbench_prefix_dist:
      return _launch_dbbench(w->nb_items_in_db, w->nb_requests_per_thread,
                             0.002312, 0.3467, 30, 14.18, -2.917, 0.0164,
                             -0.08082);
    default:
      die("Unsupported workload\n");
  }
}

/* Pretty printing */
static const char *name_dbbench(bench_t w) {
  switch (w) {
    case dbbench_all_random:
      return "DB_BENCH - All Random";
    case dbbench_all_dist:
      return "DB_BENCH - All Dist";
    case dbbench_prefix_random:
      return "DB_BENCH - Prefix Random";
    case dbbench_prefix_dist:
      return "DB_BENCH - Prefix Dist";
    default:
      return "??";
  }
}

static int handles_dbbench(bench_t w) {
  switch (w) {
    case dbbench_all_random:
    case dbbench_all_dist:
    case dbbench_prefix_random:
    case dbbench_prefix_dist:
      return 1;
    default:
      return 0;
  }
}

static const char *api_name_dbbench(void) { return "DBBENCH"; }

struct workload_api DBBENCH = {
    .init = init_dbbench,
    .handles = handles_dbbench,
    .launch = launch_dbbench,
    .api_name = api_name_dbbench,
    .name = name_dbbench,
    .create_unique_item = create_unique_item_dbbench,
};
