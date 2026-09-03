#include "headers.h"

extern uint64_t nb_totals;
/*
 * Create a workload item for the database
 */
char *create_unique_item(size_t item_size, uint64_t uid) {
  char *item = malloc(item_size);
  struct item_metadata *meta = (struct item_metadata *)item;
  item_init(meta, 8, item_size - 8 - sizeof(*meta));

  char *item_key = &item[sizeof(*meta)];
  char *item_value = &item[sizeof(*meta) + item_key_size(meta)];
  *(uint64_t *)item_key = uid;
  *(uint64_t *)item_value = uid;
  return item;
}

/* We also store an item in the database that says if the database has been
 * populated for YCSB, PRODUCTION, or another workload. */
char *create_workload_item(struct workload *w) {
  const uint64_t key = -10;
  const char *name = w->api->api_name();  // YCSB or PRODUCTION?
  size_t key_size = 16;
  size_t value_size = strlen(name) + 1;

  struct item_metadata *meta;
  char *item = malloc(sizeof(*meta) + key_size + value_size);
  meta = (struct item_metadata *)item;
  item_init(meta, key_size, value_size);

  char *item_key = &item[sizeof(*meta)];
  char *item_value = &item[sizeof(*meta) + item_key_size(meta)];
  *(uint64_t *)item_key = key;
  strcpy(item_value, name);
  return item;
}

/*
 * Fill the DB with missing items
 */
void add_in_tree(struct slab_callback *cb, void *item) {
  struct slab *s = cb->slab;
  struct item_metadata *meta = (struct item_metadata *)item;
  char *item_key = &item[sizeof(*meta)];
  uint64_t key = *(uint64_t *)item_key;

  W_LOCK(&s->tree_lock);
  tnt_index_add(cb, item);
  // s->nb_items++;
#if WITH_FILTER
  if (filter_add((filter_t *)s->filter, (unsigned char *)&key) == 0) {
    printf("Fail adding to filter %p %lu %lu\n", s->filter, key, s->nb_items);
  } else if (!filter_contain(s->filter, (unsigned char *)&key)) {
    printf("FIFIFIFIF\n");
  }
#endif

  slab_widen_range(s, key);

  __sync_fetch_and_sub(&s->update_ref, 1);

  W_UNLOCK(&s->tree_lock);

  slab_release_if_idle(s);

  __sync_fetch_and_add(&nb_totals, 1);

  if (!cb->cb_cb) {
    free(cb->item);
    free(cb);
  }
}

struct rebuild_pdata {
  size_t id;
  size_t *pos;
  size_t start;
  size_t end;
  struct workload *w;
};

void *repopulate_db_worker(void *pdata) {
  declare_periodic_count;
  struct rebuild_pdata *data = pdata;

  pin_me_on(get_nb_workers() + get_nb_distributors() + data->id);

  size_t *pos = data->pos;
  struct workload *w = data->w;
  struct workload_api *api = w->api;
  size_t start = data->start;
  size_t end = data->end;
  for (size_t i = start; i < end; i++) {
    struct slab_callback *cb = malloc(sizeof(*cb));
    cb->cb = add_in_tree;
    cb->cb_cb = NULL;
    cb->payload = NULL;
    cb->fsst_slab = NULL;
    cb->fsst_idx = -1;
    cb->item = api->create_unique_item(pos[i], w->nb_items_in_db);
    kv_add_async(cb);
    periodic_count(1000, "Repopulating database (%lu%%)",
                   100LU - (end - i) * 100LU / (end - start));
  }

  return NULL;
}

void repopulate_db(struct workload *w) {
  declare_timer;
  void *workload_item = create_workload_item(w);
  int64_t nb_inserts = (get_database_size() > w->nb_items_in_db)
                           ? 0
                           : (w->nb_items_in_db - get_database_size());

  if (nb_inserts == 0) {
    free(workload_item);
    return;
  }

  uint64_t nb_items_already_in_db = get_database_size();

  if (nb_items_already_in_db != 0 &&
      nb_items_already_in_db != w->nb_items_in_db) {
    /*
     * Because we shuffle elements, we don't really want to start with a small
     * database and have all the higher order elements at the end, that would be
     * cheating. Plus, we insert database items at random positions (see shuffle
     * below) and I am too lazy to implement the logic of doing the shuffle
     * minus existing elements.
     */
    die("The database contains %lu elements but the benchmark is configured to "
        "use %lu. Please delete the DB first.\n",
        nb_items_already_in_db, w->nb_items_in_db);
  }

  size_t *pos = NULL;
  start_timer {
    printf(
        "Initializing big array to insert elements in random order... This "
        "might take a while. (Feel free to comment but then the database will "
        "be sorted and scans much faster -- unfair vs other systems)\n");
    pos = malloc(w->nb_items_in_db * sizeof(*pos));
    if (cfg.insert_mode == ASCEND || cfg.insert_mode == RANDOM)
    	for (size_t i = 0; i < w->nb_items_in_db; i++) pos[i] = i;

    if (cfg.insert_mode == DESCEND)
    	for (size_t i = 0; i < w->nb_items_in_db; i++) pos[i] = w->nb_items_in_db - 1 - i;

    if (cfg.insert_mode == RANDOM) {
    	if (w->api == &BGWORK)
    	  shuffle_ranges(pos, nb_inserts, cfg.chunk_for_shuffle);  
    	else
    	  shuffle(pos, nb_inserts);  // To be fair to other systems, we shuffle items in
    }
  }
  stop_timer("Big array of random positions");

  start_timer {
    struct rebuild_pdata *pdata = malloc(w->nb_load_injectors * sizeof(*pdata));
    pthread_t *threads = malloc(w->nb_load_injectors * sizeof(*threads));
    for (size_t i = 0; i < w->nb_load_injectors; i++) {
      pdata[i].id = i;
      pdata[i].start = (w->nb_items_in_db / w->nb_load_injectors) * i;
      pdata[i].end = (w->nb_items_in_db / w->nb_load_injectors) * (i + 1);
      if (i == w->nb_load_injectors - 1) pdata[i].end = w->nb_items_in_db;
      pdata[i].w = w;
      pdata[i].pos = pos;
      if (i) pthread_create(&threads[i], NULL, repopulate_db_worker, &pdata[i]);
    }
    repopulate_db_worker(&pdata[0]);
    for (size_t i = 1; i < w->nb_load_injectors; i++)
      pthread_join(threads[i], NULL);
    free(threads);
    free(pdata);
  }
  stop_timer("Repopulating %lu elements (%lu req/s)", nb_inserts,
             nb_inserts * 1000000 / elapsed);

  cp_old_keys(pos, nb_inserts);
  free(pos);
}

/*
 *  Print an item stored on disk
 */
void print_item(size_t idx, void *_item) {
  char *item = _item;
  struct item_metadata *meta = (struct item_metadata *)item;
  char *item_key = &item[sizeof(*meta)];
  if (item_is_legacy(meta))
    die("Legacy item metadata passed to print_item\n");
  if (item_is_empty(meta))
    printf("[%lu] Non existant?\n", idx);
  else if (item_is_tombstone(meta))
    printf("[%lu] Removed\n", idx);
  else
    printf("[%lu] K=%lu V=%s\n", idx, *(uint64_t *)item_key,
           &item[sizeof(*meta) + item_key_size(meta)]);
}

/*
 * Various callbacks that are called once an item has been read / written
 */
void show_item(struct slab_callback *cb, void *item) {
  if (item)
    print_item(cb->slab_idx, item);
  else
    printf("Item not found\n");
  free(cb->item);
  free(cb);
}

void free_callback(struct slab_callback *cb, void *item) {
  free(cb->item);
  free(cb);
}

#if DEBUG
#define SLOW_CAP 100000

typedef struct {
  uint64_t elapsed;              // 사이클 단위 경과 시간
  struct slab_callback *cb;      // 해당 콜백
} slow_entry_t;

// min‐heap(가장 작은 elapsed가 root)
static slow_entry_t slow_heap[SLOW_CAP];
static size_t      slow_size = 0;
static pthread_mutex_t slow_lock = PTHREAD_MUTEX_INITIALIZER;

// 힙 연산
static void sift_up(size_t idx) {
  while (idx > 0) {
    size_t parent = (idx - 1) / 2;
    if (slow_heap[parent].elapsed <= slow_heap[idx].elapsed) break;
    slow_entry_t tmp = slow_heap[parent];
    slow_heap[parent] = slow_heap[idx];
    slow_heap[idx] = tmp;
    idx = parent;
  }
}
static void sift_down(size_t idx) {
  for (;;) {
    size_t l = 2*idx + 1;
    if (l >= slow_size) break;
    size_t r = l + 1;
    size_t smallest = (r < slow_size && slow_heap[r].elapsed < slow_heap[l].elapsed)
                      ? r : l;
    if (slow_heap[smallest].elapsed >= slow_heap[idx].elapsed) break;
    slow_entry_t tmp = slow_heap[idx];
    slow_heap[idx] = slow_heap[smallest];
    slow_heap[smallest] = tmp;
    idx = smallest;
  }
}

// 새 기록을 힙에 추가 (100k 미만이거나, root보다 크면 교체)
static unsigned char add_slow_entry(uint64_t elapsed, struct slab_callback *cb) {
  unsigned char stored = 0;
  pthread_mutex_lock(&slow_lock);
  if (slow_size < SLOW_CAP) {
    slow_heap[slow_size].elapsed = elapsed;
    slow_heap[slow_size].cb      = cb;
    sift_up(slow_size++);
    stored = 1;
  } else if (elapsed > slow_heap[0].elapsed) {
    slow_heap[0].elapsed = elapsed;
    slow_heap[0].cb      = cb;
    sift_down(0);
    stored = 1;
  }
  pthread_mutex_unlock(&slow_lock);
  return stored;
}
static int cmp_desc(const void *a, const void *b) {
  uint64_t ea = ((slow_entry_t*)a)->elapsed;
  uint64_t eb = ((slow_entry_t*)b)->elapsed;
  return (ea < eb) ? 1 : (ea > eb) ? -1 : 0;
}

void print_slow_payloads(void) {
  pthread_mutex_lock(&slow_lock);
  size_t n = slow_size;
  slow_entry_t *arr = malloc(n * sizeof(*arr));
  for (size_t i = 0; i < n; i++) arr[i] = slow_heap[i];
  pthread_mutex_unlock(&slow_lock);

  // 오래된 순으로 정렬
  qsort(arr, n, sizeof(*arr), cmp_desc);

  printf("=== TOP %zu SLOW PAYLOADS ===\n", n);
  for (size_t i = 0; i < n; i++) {
    struct slab_callback *cb = arr[i].cb;
    struct item_metadata *meta = (struct item_metadata *)cb->item;
    char *item_key = &cb->item[sizeof(*meta)];
    uint64_t key = *(uint64_t *)item_key;
    uint64_t start;
    printf("[%lu] key=%lu elapsed=%lu, ", i, key, cycles_to_us(arr[i].elapsed));
    // payload 내부 시퀀스 덤프
    start = get_time_from_payload(cb, 0);

    for (int p = 1; p < 20; p++) {
      uint64_t t = get_time_from_payload(cb, p);
      if (!t) break;
      printf("%lu: %lu, ", get_origin_from_payload(cb, p), cycles_to_us(t - start));
    }
    printf("\n");
    // 메모리 정리
    if (cb->cb_cb == compute_stats || !cb->cb_cb) free(cb->item);
    free_payload(cb);
    free(cb);
  }

  free(arr);
}
#endif

void compute_stats(struct slab_callback *cb, void *item) {
  uint64_t start, end;
  declare_debug_timer;
  start_debug_timer {
    start = get_time_from_payload(cb, 0);
    add_time_in_payload(cb, TIMING_STAGE_REQUEST_COMPLETE);
    rdtscll(end);
    uint64_t diff = end - start;
    add_timing_stat(diff);

#if DEBUG
    unsigned char kept = add_slow_entry(diff, cb);
#endif

    /*if (DEBUG &&*/
    /*    cycles_to_us(end - start) > 10000) {  // request took more than 10ms*/
    /*  printf(*/
    /*      "Request [%lu: %lu] [%lu: %lu] [%lu: %lu] [%lu: %lu] [%lu: %lu] "*/
    /*      "[%lu: %lu] [%lu: %lu] [%lu]\n",*/
    /*      get_origin_from_payload(cb, 1),*/
    /*      get_time_from_payload(cb, 1) < start*/
    /*          ? 0*/
    /*          : cycles_to_us(get_time_from_payload(cb, 1) - start),*/
    /*      get_origin_from_payload(cb, 2),*/
    /*      get_time_from_payload(cb, 2) < start*/
    /*          ? 0*/
    /*          : cycles_to_us(get_time_from_payload(cb, 2) - start),*/
    /*      get_origin_from_payload(cb, 3),*/
    /*      get_time_from_payload(cb, 3) < start*/
    /*          ? 0*/
    /*          : cycles_to_us(get_time_from_payload(cb, 3) - start),*/
    /*      get_origin_from_payload(cb, 4),*/
    /*      get_time_from_payload(cb, 4) < start*/
    /*          ? 0*/
    /*          : cycles_to_us(get_time_from_payload(cb, 4) - start),*/
    /*      get_origin_from_payload(cb, 5),*/
    /*      get_time_from_payload(cb, 5) < start*/
    /*          ? 0*/
    /*          : cycles_to_us(get_time_from_payload(cb, 5) - start),*/
    /*      get_origin_from_payload(cb, 6),*/
    /*      get_time_from_payload(cb, 6) < start*/
    /*          ? 0*/
    /*          : cycles_to_us(get_time_from_payload(cb, 6) - start),*/
    /*      get_origin_from_payload(cb, 7),*/
    /*      get_time_from_payload(cb, 7) < start*/
    /*          ? 0*/
    /*          : cycles_to_us(get_time_from_payload(cb, 7) - start),*/
    /*      cycles_to_us(end - start));*/
    /*}*/

if (!( 
#if DEBUG
    kept 
#else
    0
#endif
      )) {
      if (cb->cb_cb == compute_stats || !cb->cb_cb) free(cb->item);
      if (DEBUG) free_payload(cb);
      if (cb->cb_cb == compute_stats || !cb->cb_cb) free(cb);
    }
  }
  stop_debug_timer(5000, "Callback took more than 5ms???");
}

struct slab_callback *bench_cb(void) {
  struct slab_callback *cb = malloc(sizeof(*cb));
  cb->cb = compute_stats;
  cb->cb_cb = NULL;
  cb->payload = allocate_payload();
  cb->fsst_slab = NULL;
  cb->fsst_idx = -1;
  return cb;
}

/*
 * Generic worklad API.
 */
struct thread_data {
  size_t id;
  struct workload *workload;
  bench_t benchmark;
};

struct workload_api *get_api(bench_t b) {
  if (YCSB.handles(b)) return &YCSB;
  if (PRODUCTION.handles(b)) return &PRODUCTION;
  die("Unknown workload for benchmark!\n");
}

static pthread_barrier_t barrier;
void *do_workload_thread(void *pdata) {
  struct thread_data *d = pdata;

  init_seed();
  pin_me_on(get_nb_workers() + get_nb_distributors() + d->id);
  pthread_barrier_wait(&barrier);

  d->workload->api->launch(d->workload, d->benchmark);

  return NULL;
}

void run_workload(struct workload *w, bench_t b) {
  struct thread_data *pdata = malloc(w->nb_load_injectors * sizeof(*pdata));

  printf("START\n");
  w->nb_requests_per_thread = w->nb_requests / w->nb_load_injectors;
  pthread_barrier_init(&barrier, NULL, w->nb_load_injectors);

  if (!w->api->handles(b))
    die("The database has not been configured to run this benchmark! (Are you "
        "trying to run a production benchmark on a database configured for "
        "YCSB?)");
  if (b == dbbench_all_random || b == dbbench_all_dist ||
      b == dbbench_prefix_random || b == dbbench_prefix_dist || b == latprobe
      || b == locality_random || b == locality_temporal ||
         b == locality_keyspace || b == locality_both)
    w->api->init(w, b);

  declare_timer;
  start_timer {
    pthread_t *threads = malloc(w->nb_load_injectors * sizeof(*threads));
    for (int i = 0; i < w->nb_load_injectors; i++) {
      pdata[i].id = i;
      pdata[i].workload = w;
      pdata[i].benchmark = b;
      if (i) pthread_create(&threads[i], NULL, do_workload_thread, &pdata[i]);
    }

    //tnt_rebalancing();
    //fsst_worker_init();

    do_workload_thread(&pdata[0]);
    for (int i = 1; i < w->nb_load_injectors; i++)
      pthread_join(threads[i], NULL);
    free(threads);
    printf("END\n");
  }
  stop_timer("%s - %lu requests (%lu req/s)", w->api->name(b), w->nb_requests,
             w->nb_requests * 1000000 / elapsed);
  print_stats();
  print_restructuring_stats("run");
  prune_scan_report("run");

  free(pdata);
}
