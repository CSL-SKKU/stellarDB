#include "headers.h"

/* Completion ownership belongs to the harness. The engine treats payload as
 * opaque; reporting can be disabled without losing the completion barrier. */
struct bench_owner { uint64_t issued; _Atomic uint64_t completed; };
struct bench_group {
  struct bench_owner *owner;
  uint64_t start_ns;
  _Atomic uint64_t remaining;
};
struct bench_request {
  struct slab_callback cb; /* first, so existing callbacks can free(cb) */
  struct bench_owner *owner;
  struct bench_group *group;
  uint64_t start_ns;
  int started;
  void *data;
};
static __thread struct bench_owner *owner;
static __thread struct bench_group *group;
static void wait_owner(struct bench_owner *o) {
  while (atomic_load_explicit(&o->completed, memory_order_acquire) != o->issued)
    usleep(100);
}
static void complete_group(struct bench_group *g) {
  if (atomic_fetch_sub_explicit(&g->remaining, 1, memory_order_acq_rel) == 1) {
    report_request_complete(g->start_ns);
    struct bench_owner *o = g->owner;
    free(g);
    atomic_fetch_add_explicit(&o->completed, 1, memory_order_release);
  }
}
void bench_group_begin(uint64_t count) {
  assert(group == NULL);
  group = calloc(1, sizeof(*group));
  if (!group) die("Cannot allocate scan completion\n");
  group->owner = owner;
  group->start_ns = report_request_start();
  atomic_init(&group->remaining, count + 1); /* issuer's reference */
  owner->issued++;
}
void bench_group_end(void) {
  struct bench_group *g = group;
  group = NULL;
  complete_group(g);
}
struct slab_callback *bench_cb(void) {
  struct bench_request *r = calloc(1, sizeof(*r));
  if (!r) die("Cannot allocate benchmark request\n");
  r->cb.cb = compute_stats;
  r->cb.fsst_idx = -1;
  r->group = group;
  return &r->cb;
}
void bench_set_data(struct slab_callback *cb, void *data) {
  ((struct bench_request *)cb)->data = data;
}
void *bench_get_data(struct slab_callback *cb) {
  return ((struct bench_request *)cb)->data;
}
/* Continue an RMW after its read completes, retaining the original clock and
 * completion owner even when another injector submits the prepared write. */
void bench_continue(struct slab_callback *next, struct slab_callback *prev) {
  struct bench_request *n = (struct bench_request *)next;
  struct bench_request *p = (struct bench_request *)prev;
  n->owner = p->owner;
  n->start_ns = p->start_ns;
  n->started = p->started;
  n->group = p->group;
}
static void bench_start(struct slab_callback *cb, int measured) {
  struct bench_request *r = (struct bench_request *)cb;
  if (r->started || r->group) return;
  r->owner = owner;
  owner->issued++;
  r->start_ns = measured ? report_request_start() : 0;
  r->started = 1;
}
void bench_read(struct slab_callback *cb) { bench_start(cb, 1); kv_read_async(cb); }
void bench_upsert(struct slab_callback *cb) { bench_start(cb, 1); kv_upsert_async(cb); }
void bench_remove(struct slab_callback *cb) { bench_start(cb, 1); kv_remove_async(cb); }
static void bench_load(struct slab_callback *cb) { bench_start(cb, 0); kv_add_async(cb); }
void compute_stats(struct slab_callback *cb, void *item) {
  (void)item;
  struct bench_request *r = (struct bench_request *)cb;
  struct bench_owner *o = r->owner;
  struct bench_group *g = r->group;
  if (!g) report_request_complete(r->start_ns);
  free(cb->item);
  free(r);
  if (g) complete_group(g);
  else atomic_fetch_add_explicit(&o->completed, 1, memory_order_release);
}

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
struct rebuild_pdata {
  struct bench_owner completion;
  size_t id;
  size_t *pos;
  size_t start;
  size_t end;
  struct workload *w;
};

void *repopulate_db_worker(void *pdata) {

  struct rebuild_pdata *data = pdata;
  owner = &data->completion;

  pin_me_on(get_nb_workers() + get_nb_distributors() + data->id);

  size_t *pos = data->pos;
  struct workload *w = data->w;
  struct workload_api *api = w->api;
  size_t start = data->start;
  size_t end = data->end;
  for (size_t i = start; i < end; i++) {
    struct slab_callback *cb = bench_cb();
    cb->item = api->create_unique_item(pos[i], w->nb_items_in_db);
    bench_load(cb);

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
    struct rebuild_pdata *pdata = calloc(w->nb_load_injectors, sizeof(*pdata));
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
    slab_workers_drain_distributors();
    extern int load;
    load = 0;
    flush_batched_load();
    for (int i = 0; i < w->nb_load_injectors; i++) wait_owner(&pdata[i].completion);
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

/*
 * Generic worklad API.
 */
struct thread_data {
  struct bench_owner completion;
  struct workload local_workload;
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
  owner = &d->completion;

  init_seed();
  pin_me_on(get_nb_workers() + get_nb_distributors() + d->id);
  pthread_barrier_wait(&barrier);
  if (d->id == 0) report_begin();
  pthread_barrier_wait(&barrier);

  d->workload->api->launch(d->workload, d->benchmark);

  return NULL;
}

void run_workload(struct workload *w, bench_t b) {
  struct thread_data *pdata = calloc(w->nb_load_injectors, sizeof(*pdata));

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

  {
    pthread_t *threads = malloc(w->nb_load_injectors * sizeof(*threads));
    for (int i = 0; i < w->nb_load_injectors; i++) {
      pdata[i].id = i;
      pdata[i].local_workload = *w;
      pdata[i].local_workload.nb_requests_per_thread += (uint64_t)i < w->nb_requests % w->nb_load_injectors;
      pdata[i].workload = &pdata[i].local_workload;
      pdata[i].benchmark = b;
      if (i) pthread_create(&threads[i], NULL, do_workload_thread, &pdata[i]);
    }

    //tnt_rebalancing();
    //fsst_worker_init();

    do_workload_thread(&pdata[0]);
    for (int i = 1; i < w->nb_load_injectors; i++)
      pthread_join(threads[i], NULL);
    free(threads);
    for (int i = 0; i < w->nb_load_injectors; i++) wait_owner(&pdata[i].completion);
    report_finish();
    printf("END\n");
  }
  pthread_barrier_destroy(&barrier);

  free(pdata);
}
