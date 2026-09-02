#include "headers.h"

#include <stdbool.h>

#define STRESS_ITEM_SIZE 64
#define STRESS_INITIAL_KEYS 256
#define STRESS_KEYSPACE 512
#define STRESS_LEAF_ITEMS 8192
#define DEFAULT_THREADS 16
#define DEFAULT_OPS_PER_THREAD 20000

int load = 0;
int rc_thr = 1;
extern uint64_t nb_totals;

void read_item_async_cb(struct slab_callback *callback);

struct test_slab {
  struct slab *slab;
  unsigned char *data;
  pthread_mutex_t *page_locks;
  size_t nb_pages;
};

struct worker_arg {
  int id;
  size_t ops;
  unsigned int seed;
};

static struct test_slab root_store;
static struct test_slab left_store;
static struct test_slab right_store;
static pthread_barrier_t start_barrier;

static _Atomic uint64_t read_index_misses;
static _Atomic uint64_t read_bad_records;
static _Atomic uint64_t write_bad_destinations;
static _Atomic bool key_created[STRESS_KEYSPACE + 1];

static __thread void *read_result;

#define CHECK(condition)                                                     \
  do {                                                                       \
    if (!(condition)) {                                                      \
      fprintf(stderr, "DELETE stress setup failed at %s:%d: %s\n",         \
              __FILE__, __LINE__, #condition);                               \
      exit(EXIT_FAILURE);                                                    \
    }                                                                        \
  } while (0)

static void keep_callback(struct slab_callback *cb, void *item) {
  (void)cb;
  (void)item;
}

static void capture_read(struct slab_callback *cb, void *item) {
  (void)cb;
  read_result = item;
}

static struct test_slab make_test_slab(uint64_t seq, uint64_t key,
                                       size_t max_items) {
  struct test_slab store = {0};
  struct slab *s = calloc(1, sizeof(*s));
  CHECK(s != NULL);

  size_t items_per_page = PAGE_SIZE / STRESS_ITEM_SIZE;
  size_t nb_pages = (max_items + items_per_page - 1) / items_per_page;
  size_t storage_size = nb_pages * PAGE_SIZE;

  store.data = aligned_alloc(PAGE_SIZE, storage_size);
  store.page_locks = calloc(nb_pages, sizeof(*store.page_locks));
  CHECK(store.data != NULL);
  CHECK(store.page_locks != NULL);
  memset(store.data, 0, storage_size);
  for (size_t i = 0; i < nb_pages; i++)
    CHECK(pthread_mutex_init(&store.page_locks[i], NULL) == 0);

  s->seq = seq;
  s->key = key;
  s->min = UINT64_MAX;
  s->max = 0;
  s->item_size = STRESS_ITEM_SIZE;
  s->nb_max_items = max_items;
  s->size_on_disk = storage_size;
  s->fd = -1;
  s->subtree = tnt_subtree_create();
  CHECK(s->subtree != NULL);
  subtree_set_slab(s->subtree, s);
  atomic_init(&s->full, 0);
  atomic_init(&s->last_item, 0);
  atomic_init(&s->queued, 0);
  atomic_init(&s->upward_maxlen, 0);
  atomic_init(&s->cur_ep, 0);
  atomic_init(&s->epcnt, 0);
  atomic_init(&s->prev_epcnt, 0);
  INIT_LOCK(&s->tree_lock, NULL);

  store.slab = s;
  store.nb_pages = nb_pages;
  return store;
}

static struct test_slab *store_for_slab(struct slab *s) {
  if (s == root_store.slab) return &root_store;
  if (s == left_store.slab) return &left_store;
  if (s == right_store.slab) return &right_store;
  return NULL;
}

static void init_item(unsigned char item[STRESS_ITEM_SIZE], uint64_t key,
                      uint64_t version, bool tombstone) {
  memset(item, 0, STRESS_ITEM_SIZE);
  struct item_metadata *meta = (struct item_metadata *)item;
  item_init(meta, sizeof(key),
            STRESS_ITEM_SIZE - sizeof(*meta) - sizeof(key));
  *(uint64_t *)(item + sizeof(*meta)) = key;
  memcpy(item + sizeof(*meta) + sizeof(key), &version, sizeof(version));
  if (tombstone)
    item_encode_tombstone(meta);
}

static void write_record(struct slab *s, size_t idx, const void *item) {
  struct test_slab *store = store_for_slab(s);
  if (!store || idx >= s->nb_max_items) {
    atomic_fetch_add_explicit(&write_bad_destinations, 1,
                              memory_order_relaxed);
    return;
  }

  size_t items_per_page = PAGE_SIZE / s->item_size;
  size_t page = idx / items_per_page;
  size_t offset = (idx % items_per_page) * s->item_size;
  const struct item_metadata *meta = item;

  pthread_mutex_lock(&store->page_locks[page]);
  memcpy(store->data + page * PAGE_SIZE + offset, item,
         item_stored_size(meta));
  pthread_mutex_unlock(&store->page_locks[page]);
}

static void publish_initial(struct slab *s, size_t idx, void *item) {
  struct slab_callback cb = {
    .item = item,
    .slab = s,
    .slab_idx = idx,
  };
  struct item_metadata *meta = item;
  uint64_t key = *(uint64_t *)((char *)item + sizeof(*meta));

  write_record(s, idx, item);
  tnt_index_add(&cb, item);
  if (key < s->min) s->min = key;
  if (key > s->max) s->max = key;
  s->nb_items++;
}

static void perform_write(uint64_t key, uint64_t version, bool tombstone) {
  _Alignas(struct item_metadata) unsigned char item[STRESS_ITEM_SIZE];
  init_item(item, key, version, tombstone);

  uint64_t idx;
  index_entry_t *old_entry = NULL;
  struct tree_entry *tree =
      centree_lookup_and_reserve(item, &idx, &old_entry);
  if (!tree || !tree->slab) {
    atomic_fetch_add_explicit(&write_bad_destinations, 1,
                              memory_order_relaxed);
    return;
  }

  struct slab *dest = tree->slab;
  struct slab *old_s = old_entry ? old_entry->slab : NULL;
  size_t old_idx = old_entry ? GET_SIDX(old_entry->slab_idx) : UINT64_MAX;

  if (idx == UINT64_MAX) {
    if (!old_entry || old_s != dest || old_idx >= dest->nb_max_items) {
      atomic_fetch_add_explicit(&write_bad_destinations, 1,
                                memory_order_relaxed);
      __sync_fetch_and_sub(&dest->update_ref, 1);
      return;
    }
    write_record(dest, old_idx, item);
    __sync_fetch_and_sub(&dest->update_ref, 1);
    atomic_store_explicit(&key_created[key], true, memory_order_release);
    return;
  }

  write_record(dest, idx, item);
  struct slab_callback cb = {
    .cb_cb = keep_callback,
    .item = item,
    .slab = dest,
    .slab_idx = idx,
    .fsst_slab = old_s,
    .fsst_idx = old_idx,
  };

  if (old_entry)
    add_in_tree_for_upsert(&cb, item);
  else
    add_in_tree(&cb, item);
  atomic_store_explicit(&key_created[key], true, memory_order_release);
}

static void perform_read(uint64_t key) {
  _Alignas(struct item_metadata) unsigned char query[STRESS_ITEM_SIZE];
  init_item(query, key, 0, false);
  struct slab_callback lookup_cb = {.item = query};
  index_entry_t *entry = tnt_index_lookup(&lookup_cb, query);
  if (!entry) {
    if (atomic_load_explicit(&key_created[key], memory_order_acquire))
      atomic_fetch_add_explicit(&read_index_misses, 1, memory_order_relaxed);
    return;
  }

  struct slab *s = entry->slab;
  size_t idx = GET_SIDX(entry->slab_idx);
  struct test_slab *store = store_for_slab(s);
  if (!store || idx >= s->nb_max_items) {
    tnt_index_lookup_unref(entry);
    atomic_fetch_add_explicit(&read_bad_records, 1, memory_order_relaxed);
    return;
  }

  size_t items_per_page = PAGE_SIZE / s->item_size;
  size_t page = idx / items_per_page;
  struct lru lru = {.page = store->data + page * PAGE_SIZE};
  struct slab_callback read_cb = {
    .cb = capture_read,
    .item = query,
    .slab = s,
    .slab_idx = idx,
    .lru_entry = &lru,
  };

  pthread_mutex_lock(&store->page_locks[page]);
  /* tnt_index_lookup() took the reference read_item_async_cb() drops. */
  read_result = (void *)(uintptr_t)1;
  read_item_async_cb(&read_cb);

  if (read_result) {
    struct item_metadata *meta = read_result;
    uint64_t found_key =
        *(uint64_t *)((unsigned char *)read_result + sizeof(*meta));
    if (item_is_legacy(meta) || item_is_empty(meta) ||
        item_is_tombstone(meta) || found_key != key)
      atomic_fetch_add_explicit(&read_bad_records, 1,
                                memory_order_relaxed);
  }
  pthread_mutex_unlock(&store->page_locks[page]);
}

static void *stress_worker(void *arg) {
  struct worker_arg *worker = arg;
  unsigned int seed = worker->seed;
  pthread_barrier_wait(&start_barrier);

  for (size_t i = 0; i < worker->ops; i++) {
    uint64_t key;
    if (i < 1024) {
      uint64_t hot_key = i % 8;
      key = hot_key < 4 ? hot_key + 1
                        : STRESS_INITIAL_KEYS + hot_key - 3;
    } else {
      key = (rand_r(&seed) % STRESS_KEYSPACE) + 1;
    }

    unsigned int action = rand_r(&seed) % 100;
    uint64_t version = ((uint64_t)worker->id << 48) | i;
    if (action < 40)
      perform_write(key, version, true);
    else if (action < 80)
      perform_write(key, version, false);
    else
      perform_read(key);
  }
  return NULL;
}

static int local_entry_state(struct slab *s, void *query, size_t *idx_out) {
  int state = 0;
  R_LOCK(&s->tree_lock);
  index_entry_t *entry = tnt_index_lookup_utree(s->subtree, query);
  if (entry) {
    *idx_out = GET_SIDX(entry->slab_idx);
    state = ((uint32_t)entry->slab_idx & (1u << 31)) ? 2 : 1;
  }
  R_UNLOCK(&s->tree_lock);
  return state;
}

static bool slot_has_key(struct slab *s, size_t idx, uint64_t key) {
  struct test_slab *store = store_for_slab(s);
  if (!store || idx >= s->nb_max_items) return false;

  size_t items_per_page = PAGE_SIZE / s->item_size;
  size_t page = idx / items_per_page;
  size_t offset = (idx % items_per_page) * s->item_size;
  bool matches;

  pthread_mutex_lock(&store->page_locks[page]);
  struct item_metadata *meta =
      (struct item_metadata *)(store->data + page * PAGE_SIZE + offset);
  uint64_t stored_key = *(uint64_t *)((unsigned char *)meta + sizeof(*meta));
  matches = !item_is_legacy(meta) && !item_is_empty(meta) &&
            item_key_size(meta) == sizeof(uint64_t) && stored_key == key;
  pthread_mutex_unlock(&store->page_locks[page]);
  return matches;
}

static uint64_t audit_final_state(void) {
  uint64_t anomalies = 0;
  struct slab *slabs[] = {root_store.slab, left_store.slab, right_store.slab};

  for (uint64_t key = 1; key <= STRESS_KEYSPACE; key++) {
    _Alignas(struct item_metadata) unsigned char query[STRESS_ITEM_SIZE];
    init_item(query, key, 0, false);
    size_t valid_count = 0;

    for (size_t i = 0; i < sizeof(slabs) / sizeof(slabs[0]); i++) {
      size_t idx = 0;
      int state = local_entry_state(slabs[i], query, &idx);
      if (state == 1) {
        valid_count++;
        if (!slot_has_key(slabs[i], idx, key)) anomalies++;
      }
    }
    bool created =
        atomic_load_explicit(&key_created[key], memory_order_acquire);
    if (valid_count != (created ? 1u : 0u)) anomalies++;

    struct slab_callback cb = {.item = query};
    index_entry_t *entry = tnt_index_lookup(&cb, query);
    tnt_index_lookup_unref(entry);
    if (created) {
      if (!entry ||
          ((uint32_t)entry->slab_idx & (1u << 31)) != 0 ||
          !slot_has_key(entry->slab, GET_SIDX(entry->slab_idx), key))
        anomalies++;
    } else if (entry) {
      anomalies++;
    }
  }
  return anomalies;
}

static void initialize_tree(void) {
  centree_init();
  root_store =
      make_test_slab(1, STRESS_INITIAL_KEYS, STRESS_INITIAL_KEYS);
  left_store =
      make_test_slab(2, STRESS_INITIAL_KEYS - 1, STRESS_LEAF_ITEMS);
  right_store =
      make_test_slab(3, STRESS_INITIAL_KEYS + 1, STRESS_LEAF_ITEMS);

  tnt_subtree_add(root_store.slab, root_store.slab->subtree, NULL,
                  root_store.slab->key);
  for (uint64_t key = 1; key <= STRESS_INITIAL_KEYS; key++) {
    _Alignas(struct item_metadata) unsigned char item[STRESS_ITEM_SIZE];
    init_item(item, key, 0, false);
    publish_initial(root_store.slab, key - 1, item);
    atomic_init(&key_created[key], true);
  }
  atomic_store_explicit(&root_store.slab->last_item, STRESS_INITIAL_KEYS,
                        memory_order_release);
  atomic_store_explicit(&root_store.slab->full, 1, memory_order_release);
  nb_totals = STRESS_INITIAL_KEYS;

  tnt_subtree_add(left_store.slab, left_store.slab->subtree, NULL,
                  left_store.slab->key);
  tnt_subtree_add(right_store.slab, right_store.slab->subtree, NULL,
                  right_store.slab->key);
  wakeup_subtree_get(root_store.slab->centree_node);
}

int main(int argc, char **argv) {
  int nb_threads = argc > 1 ? atoi(argv[1]) : DEFAULT_THREADS;
  size_t ops = argc > 2 ? strtoull(argv[2], NULL, 10)
                        : DEFAULT_OPS_PER_THREAD;
  unsigned int seed = argc > 3 ? strtoul(argv[3], NULL, 10) : 1;
  CHECK(nb_threads > 0);
  CHECK(ops > 0);

  init_default_config(&cfg);
  cfg.kv_size = STRESS_ITEM_SIZE;
  cfg.max_file_size = STRESS_LEAF_ITEMS * STRESS_ITEM_SIZE;
  cfg.with_reins = 0;
  initialize_tree();

  pthread_t *threads = calloc(nb_threads, sizeof(*threads));
  struct worker_arg *args = calloc(nb_threads, sizeof(*args));
  CHECK(threads != NULL);
  CHECK(args != NULL);
  CHECK(pthread_barrier_init(&start_barrier, NULL, nb_threads) == 0);

  for (int i = 0; i < nb_threads; i++) {
    args[i].id = i;
    args[i].ops = ops;
    args[i].seed = seed ^ (0x9e3779b9u * (unsigned int)(i + 1));
    CHECK(pthread_create(&threads[i], NULL, stress_worker, &args[i]) == 0);
  }
  for (int i = 0; i < nb_threads; i++)
    CHECK(pthread_join(threads[i], NULL) == 0);

  uint64_t final_anomalies = audit_final_state();
  uint64_t index_misses = atomic_load_explicit(&read_index_misses,
                                                memory_order_relaxed);
  uint64_t bad_reads = atomic_load_explicit(&read_bad_records,
                                             memory_order_relaxed);
  uint64_t bad_writes = atomic_load_explicit(&write_bad_destinations,
                                              memory_order_relaxed);
  printf("DELETE stress: threads=%d ops/thread=%lu seed=%u "
         "index_misses=%lu bad_reads=%lu bad_writes=%lu "
         "final_anomalies=%lu\n",
         nb_threads, ops, seed, index_misses, bad_reads, bad_writes,
         final_anomalies);

  return (index_misses || bad_reads || bad_writes || final_anomalies)
             ? EXIT_FAILURE
             : EXIT_SUCCESS;
}
