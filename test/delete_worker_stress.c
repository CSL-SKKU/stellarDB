#include "headers.h"

#include <errno.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <sys/wait.h>
#include <time.h>

#define TEST_ITEM_SIZE 64
#define INITIAL_KEYS 128
#define KEYSPACE 256
#define DEFAULT_CLIENTS 8
#define DEFAULT_OPS 2500
#define REQUEST_TIMEOUT_SECONDS 10

#define TEST_CHECK(condition)                                                \
  do {                                                                       \
    if (!(condition)) {                                                      \
      fprintf(stderr, "rebalance concurrency test failed at %s:%d: %s\n",  \
              __FILE__, __LINE__, #condition);                               \
      exit(EXIT_FAILURE);                                                    \
    }                                                                        \
  } while (0)

int load = 0;
int print = 0;
int rc_thr = 1;

static char test_db_dir[PATH_MAX] =
    "/tmp/stellardb-delete-workers.XXXXXX";
static _Atomic uint64_t completed_reads;
static _Atomic uint64_t completed_writes;
static _Atomic uint64_t wrong_key_callbacks;
static _Atomic uint64_t read_tombstones;
static _Atomic uint64_t delete_nontombstones;
static _Atomic bool split_hook_reached;
static _Atomic bool split_hook_release;
static _Atomic bool rebalance_hook_reached;
static _Atomic bool rebalance_hook_release;
static _Atomic bool rebalance_postpublish_hook_reached;
static _Atomic bool rebalance_postpublish_hook_release;
static centree_node paused_split_parent;
static centree_node paused_left_child;

struct rebalance_thread_state {
  _Atomic bool started;
  int status;
};

struct split_phase_state {
  _Atomic bool started;
  _Atomic bool acquired;
};

struct concurrent_client_state {
  _Atomic bool ready;
  _Atomic bool stop;
  bool writer;
  uint64_t operations;
};

struct topology_snapshot {
  size_t count;
  centree_node *inorder;
  centree_node *lu_parents;
  uint64_t *keys;
  uint64_t *value_keys;
  struct slab **slabs;
  bool *leaves;
};

enum test_operation {
  TEST_READ,
  TEST_UPSERT,
  TEST_DELETE,
};

struct test_request {
  struct slab_callback callback;
  _Atomic bool done;
  bool returned_item;
  uint64_t returned_version;
  enum test_operation operation;
  uint64_t key;
  unsigned char item[TEST_ITEM_SIZE];
};

struct client_arg {
  size_t operations;
  unsigned int seed;
};

int __real_open(const char *path, int flags, ...);
DIR *__real_opendir(const char *path);

static const char *redirect_path(const char *path, char redirected[PATH_MAX]) {
  static const char prefix[] = "/scratch0/kvell";

  if (strncmp(path, prefix, sizeof(prefix) - 1) != 0)
    return path;

  int written = snprintf(redirected, PATH_MAX, "%s%s", test_db_dir,
                         path + sizeof(prefix) - 1);
  if (written < 0 || written >= PATH_MAX) {
    errno = ENAMETOOLONG;
    return NULL;
  }
  return redirected;
}

int __wrap_open(const char *path, int flags, ...) {
  char redirected[PATH_MAX];
  const char *actual = redirect_path(path, redirected);
  if (!actual)
    return -1;

  if (flags & O_CREAT) {
    va_list args;
    va_start(args, flags);
    mode_t mode = va_arg(args, mode_t);
    va_end(args);
    return __real_open(actual, flags, mode);
  }
  return __real_open(actual, flags);
}

DIR *__wrap_opendir(const char *path) {
  char redirected[PATH_MAX];
  const char *actual = redirect_path(path, redirected);
  return actual ? __real_opendir(actual) : NULL;
}

static void init_item(unsigned char item[TEST_ITEM_SIZE], uint64_t key,
                      uint64_t version) {
  memset(item, 0, TEST_ITEM_SIZE);
  struct item_metadata *meta = (struct item_metadata *)item;
  item_init(meta, sizeof(key),
            TEST_ITEM_SIZE - sizeof(*meta) - sizeof(key));
  memcpy(item + sizeof(*meta), &key, sizeof(key));
  memcpy(item + sizeof(*meta) + sizeof(key), &version, sizeof(version));
}

static void request_complete(struct slab_callback *callback, void *item) {
  struct test_request *request =
      (struct test_request *)((char *)callback -
                              offsetof(struct test_request, callback));

  if (item) {
    struct item_metadata *meta = item;
    uint64_t key;
    memcpy(&key, (char *)item + sizeof(*meta), sizeof(key));
    if (key != request->key)
      atomic_fetch_add_explicit(&wrong_key_callbacks, 1, memory_order_relaxed);
    if (request->operation == TEST_READ)
      memcpy(&request->returned_version,
             (char *)item + sizeof(*meta) + sizeof(key), sizeof(key));

    if (request->operation == TEST_READ && item_is_tombstone(meta))
      atomic_fetch_add_explicit(&read_tombstones, 1, memory_order_relaxed);
    if (request->operation == TEST_DELETE && !item_is_tombstone(meta))
      atomic_fetch_add_explicit(&delete_nontombstones, 1,
                                memory_order_relaxed);
  }

  if (request->operation == TEST_READ)
    atomic_fetch_add_explicit(&completed_reads, 1, memory_order_relaxed);
  else
    atomic_fetch_add_explicit(&completed_writes, 1, memory_order_relaxed);
  request->returned_item = item != NULL;
  atomic_store_explicit(&request->done, true, memory_order_release);
}

static struct test_request *submit_request(enum test_operation operation,
                                           uint64_t key, uint64_t version) {
  struct test_request *request = calloc(1, sizeof(*request));
  if (!request) {
    perror("calloc");
    exit(EXIT_FAILURE);
  }

  request->operation = operation;
  request->key = key;
  init_item(request->item, key, version);
  request->callback.cb = request_complete;
  request->callback.item = request->item;
  request->callback.fsst_slab = NULL;
  request->callback.fsst_idx = (uint64_t)-1;
  atomic_init(&request->done, false);

  switch (operation) {
    case TEST_READ:
      kv_read_async(&request->callback);
      break;
    case TEST_UPSERT:
      kv_upsert_async(&request->callback);
      break;
    case TEST_DELETE:
      kv_remove_async(&request->callback);
      break;
  }
  return request;
}

static bool wait_for_request(struct test_request *request) {
  struct timespec start;
  clock_gettime(CLOCK_MONOTONIC, &start);

  while (!atomic_load_explicit(&request->done, memory_order_acquire)) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec - start.tv_sec >= REQUEST_TIMEOUT_SECONDS)
      return false;
    sched_yield();
  }
  return true;
}

static struct test_request *run_and_wait(enum test_operation operation,
                                         uint64_t key, uint64_t version) {
  struct test_request *request = submit_request(operation, key, version);
  if (!wait_for_request(request)) {
    fprintf(stderr, "worker stress timed out: op=%d key=%lu\n", operation,
            key);
    exit(EXIT_FAILURE);
  }
  /* Worker threads have no shutdown API. Keep callbacks alive until exit. */
  return request;
}

static void *client_main(void *opaque) {
  struct client_arg *arg = opaque;

  for (size_t i = 0; i < arg->operations; i++) {
    uint64_t key;
    if (i < 512)
      key = 1 + (rand_r(&arg->seed) % 8);
    else
      key = 1 + (rand_r(&arg->seed) % KEYSPACE);

    unsigned int choice = rand_r(&arg->seed) % 10;
    enum test_operation operation = choice < 4   ? TEST_DELETE
                                    : choice < 8 ? TEST_UPSERT
                                                 : TEST_READ;
    (void)run_and_wait(operation, key,
                       ((uint64_t)arg->seed << 32) | (uint64_t)i);
  }
  return NULL;
}

static void remove_test_directory(const char *directory) {
  DIR *dir = __real_opendir(directory);
  if (!dir)
    return;

  struct dirent *entry;
  while ((entry = readdir(dir))) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
      continue;
    char path[PATH_MAX];
    int written = snprintf(path, sizeof(path), "%s/%s", directory,
                           entry->d_name);
    if (written > 0 && written < (int)sizeof(path))
      unlink(path);
  }
  closedir(dir);
  rmdir(directory);
}

static void expect_read(uint64_t key, bool present) {
  struct test_request *request = run_and_wait(TEST_READ, key, 0);
  if (request->returned_item != present) {
    fprintf(stderr, "acceptance READ mismatch: key=%lu expected=%s\n", key,
            present ? "present" : "missing");
    exit(EXIT_FAILURE);
  }
}

static void expect_direct_index_hit(uint64_t key) {
  unsigned char item[TEST_ITEM_SIZE];
  struct slab_callback callback = {0};

  init_item(item, key, 0);
  callback.item = item;
  TEST_CHECK(tnt_index_lookup(&callback, item) != NULL);
}

static bool wait_for_flag(_Atomic bool *flag, time_t timeout_seconds) {
  struct timespec start;

  clock_gettime(CLOCK_MONOTONIC, &start);
  while (!atomic_load_explicit(flag, memory_order_acquire)) {
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec - start.tv_sec >= timeout_seconds)
      return false;
    sched_yield();
  }
  return true;
}

static void split_midpoint_test_hook(struct slab *parent_slab) {
  centree_node parent = parent_slab->centree_node;

  TEST_CHECK(parent != NULL);
  centree_node left = tnt_routing_left(parent);
  centree_node right = tnt_routing_right(parent);
  TEST_CHECK(left != NULL);
  TEST_CHECK(right == NULL);
  TEST_CHECK(tnt_routing_parent(left) == parent);
  TEST_CHECK(centree_pivot_load(parent) == parent_slab->key);
  TEST_CHECK(atomic_load_explicit(&parent->child_flag,
                                  memory_order_acquire) == 0);
  paused_split_parent = parent;
  paused_left_child = left;
  atomic_store_explicit(&split_hook_reached, true, memory_order_release);

  while (!atomic_load_explicit(&split_hook_release, memory_order_acquire))
    sched_yield();
}

static void rebalance_precommit_test_hook(void) {
  atomic_store_explicit(&rebalance_hook_reached, true, memory_order_release);
  while (!atomic_load_explicit(&rebalance_hook_release,
                               memory_order_acquire))
    sched_yield();
}

static void rebalance_postpublish_test_hook(void) {
  atomic_store_explicit(&rebalance_postpublish_hook_reached, true,
                        memory_order_release);
  while (!atomic_load_explicit(&rebalance_postpublish_hook_release,
                               memory_order_acquire))
    sched_yield();
}

static void *rebalance_thread_main(void *opaque) {
  struct rebalance_thread_state *state = opaque;

  atomic_store_explicit(&state->started, true, memory_order_release);
  state->status = tnt_rebalancing();
  return NULL;
}

static void *split_phase_main(void *opaque) {
  struct split_phase_state *state = opaque;

  atomic_store_explicit(&state->started, true, memory_order_release);
  tnt_split_phase_enter();
  atomic_store_explicit(&state->acquired, true, memory_order_release);
  tnt_split_phase_exit();
  return NULL;
}

static void *concurrent_rebalance_client(void *opaque) {
  struct concurrent_client_state *state = opaque;

  atomic_store_explicit(&state->ready, true, memory_order_release);
  do {
    uint64_t key = 48 + state->operations % 8;

    if (state->writer)
      (void)run_and_wait(TEST_UPSERT, key, 1000 + state->operations);
    else
      expect_read(key, true);
    state->operations++;
  } while (!atomic_load_explicit(&state->stop, memory_order_acquire));

  return NULL;
}

static centree_node find_routing_root(centree_node node) {
  centree_node parent;

  while (node != NULL && (parent = tnt_routing_parent(node)) != NULL)
    node = parent;
  return node;
}

static size_t count_routing_nodes(centree_node node) {
  if (node == NULL)
    return 0;
  return 1 + count_routing_nodes(tnt_routing_left(node)) +
         count_routing_nodes(tnt_routing_right(node));
}

static void collect_routing_nodes(centree_node node, centree_node *nodes,
                                  size_t *index) {
  if (node == NULL)
    return;
  collect_routing_nodes(tnt_routing_left(node), nodes, index);
  nodes[(*index)++] = node;
  collect_routing_nodes(tnt_routing_right(node), nodes, index);
}

static bool validate_routing_node(centree_node node, centree_node parent,
                                  unsigned int level,
                                  unsigned int *actual_depth,
                                  size_t *node_count) {
  bool has_left;
  bool has_right;

  if (node == NULL)
    return true;
  centree_node left = tnt_routing_left(node);
  centree_node right = tnt_routing_right(node);
  has_left = left != NULL;
  has_right = right != NULL;
  if (has_left != has_right || (has_left && left == right) ||
      tnt_routing_parent(node) != parent ||
      node->value.level != level || node->value.slab == NULL ||
      node->value.slab->centree_node != node)
    return false;

  (*node_count)++;
  if (level > *actual_depth)
    *actual_depth = level;
  return validate_routing_node(left, node, level + 1, actual_depth,
                               node_count) &&
         validate_routing_node(right, node, level + 1, actual_depth,
                               node_count);
}

static bool validate_routing_tree(centree_node any_node) {
  centree_node root = find_routing_root(any_node);
  unsigned int actual_depth = 0;
  size_t node_count = 0;

  return root != NULL &&
         validate_routing_node(root, NULL, 1, &actual_depth, &node_count) &&
         node_count % 2 == 1 && tnt_get_depth() == actual_depth;
}

static struct topology_snapshot take_topology_snapshot(centree_node any_node) {
  struct topology_snapshot snapshot = {0};
  centree_node root = find_routing_root(any_node);
  size_t index = 0;

  TEST_CHECK(validate_routing_tree(root));
  snapshot.count = count_routing_nodes(root);
  snapshot.inorder = calloc(snapshot.count, sizeof(*snapshot.inorder));
  snapshot.lu_parents = calloc(snapshot.count, sizeof(*snapshot.lu_parents));
  snapshot.keys = calloc(snapshot.count, sizeof(*snapshot.keys));
  snapshot.value_keys = calloc(snapshot.count, sizeof(*snapshot.value_keys));
  snapshot.slabs = calloc(snapshot.count, sizeof(*snapshot.slabs));
  snapshot.leaves = calloc(snapshot.count, sizeof(*snapshot.leaves));
  TEST_CHECK(snapshot.inorder != NULL && snapshot.lu_parents != NULL &&
             snapshot.keys != NULL && snapshot.value_keys != NULL &&
             snapshot.slabs != NULL && snapshot.leaves != NULL);

  collect_routing_nodes(root, snapshot.inorder, &index);
  TEST_CHECK(index == snapshot.count);
  for (size_t i = 0; i < snapshot.count; i++) {
    centree_node node = snapshot.inorder[i];

    snapshot.lu_parents[i] = node->lu_parent;
    snapshot.keys[i] = centree_pivot_load(node);
    snapshot.value_keys[i] = node->value.key;
    snapshot.slabs[i] = node->value.slab;
    snapshot.leaves[i] = tnt_routing_left(node) == NULL &&
                         tnt_routing_right(node) == NULL;
  }
  return snapshot;
}

static bool topology_matches_snapshot(const struct topology_snapshot *snapshot,
                                      centree_node any_node) {
  centree_node root = find_routing_root(any_node);
  size_t count = count_routing_nodes(root);
  size_t index = 0;
  centree_node *inorder;
  bool matches = validate_routing_tree(root) && count == snapshot->count;

  if (!matches)
    return false;
  inorder = calloc(count, sizeof(*inorder));
  if (inorder == NULL)
    return false;
  collect_routing_nodes(root, inorder, &index);
  matches = index == count;
  for (size_t i = 0; matches && i < count; i++) {
    centree_node node = inorder[i];
    bool is_leaf = tnt_routing_left(node) == NULL &&
                   tnt_routing_right(node) == NULL;

    matches = node == snapshot->inorder[i] &&
              node->lu_parent == snapshot->lu_parents[i] &&
              centree_pivot_load(node) == snapshot->keys[i] &&
              node->value.key == snapshot->value_keys[i] &&
              node->value.slab == snapshot->slabs[i] &&
              is_leaf == snapshot->leaves[i];
  }
  free(inorder);
  return matches;
}

static void free_topology_snapshot(struct topology_snapshot *snapshot) {
  free(snapshot->leaves);
  free(snapshot->slabs);
  free(snapshot->value_keys);
  free(snapshot->keys);
  free(snapshot->lu_parents);
  free(snapshot->inorder);
  memset(snapshot, 0, sizeof(*snapshot));
}

static void expect_present_range(uint64_t first, uint64_t last) {
  for (uint64_t key = first; key <= last; key++)
    expect_read(key, true);
}

static uint64_t *capture_read_versions(uint64_t first, uint64_t last) {
  size_t count = last - first + 1;
  uint64_t *versions = calloc(count, sizeof(*versions));

  TEST_CHECK(versions != NULL);
  for (uint64_t key = first; key <= last; key++) {
    struct test_request *request = run_and_wait(TEST_READ, key, 0);

    TEST_CHECK(request->returned_item);
    versions[key - first] = request->returned_version;
  }
  return versions;
}

static void expect_read_versions(uint64_t first, uint64_t last,
                                 const uint64_t *versions) {
  for (uint64_t key = first; key <= last; key++) {
    struct test_request *request = run_and_wait(TEST_READ, key, 0);

    TEST_CHECK(request->returned_item);
    TEST_CHECK(request->returned_version == versions[key - first]);
  }
}

static void init_workers(void) {
  init_default_config(&cfg);
  cfg.kv_size = TEST_ITEM_SIZE;
  cfg.max_file_size = PAGE_SIZE;
  cfg.page_cache_size = PAGE_SIZE * 4096;
  cfg.with_reins = 0;
  cfg.with_rebal = 0;
  slab_workers_init(1, 2, 2);
}

static int timed_join(pthread_t thread, long timeout_milliseconds) {
  struct timespec deadline;

  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += timeout_milliseconds / 1000;
  deadline.tv_nsec += (timeout_milliseconds % 1000) * 1000000L;
  if (deadline.tv_nsec >= 1000000000L) {
    deadline.tv_sec++;
    deadline.tv_nsec -= 1000000000L;
  }
  return pthread_timedjoin_np(thread, NULL, &deadline);
}

static void run_mid_split_rebalance_test(void) {
  struct test_request *split_request;
  struct rebalance_thread_state rebalance_state = {0};
  struct split_phase_state late_split = {0};
  struct concurrent_client_state clients[2] = {
      {.writer = false},
      {.writer = true},
  };
  pthread_t rebalance_thread;
  pthread_t late_split_thread;
  pthread_t client_threads[2];
  centree_node parent_lu_parent;
  centree_node left_lu_parent;
  struct topology_snapshot snapshot;

  atomic_store_explicit(&split_hook_reached, false, memory_order_relaxed);
  atomic_store_explicit(&split_hook_release, false, memory_order_relaxed);
  atomic_store_explicit(&rebalance_hook_reached, false,
                        memory_order_relaxed);
  atomic_store_explicit(&rebalance_hook_release, false,
                        memory_order_relaxed);
  atomic_store_explicit(&rebalance_postpublish_hook_reached, false,
                        memory_order_relaxed);
  atomic_store_explicit(&rebalance_postpublish_hook_release, false,
                        memory_order_relaxed);
  paused_split_parent = NULL;
  paused_left_child = NULL;

  for (uint64_t key = 1; key < 64; key++)
    (void)run_and_wait(TEST_UPSERT, key, key);

  slab_set_split_midpoint_test_hook(split_midpoint_test_hook);
  tnt_set_rebalance_precommit_test_hook(rebalance_precommit_test_hook);
  tnt_set_rebalance_postpublish_test_hook(rebalance_postpublish_test_hook);
  split_request = submit_request(TEST_UPSERT, 64, 64);
  TEST_CHECK(wait_for_flag(&split_hook_reached, REQUEST_TIMEOUT_SECONDS));
  TEST_CHECK(paused_split_parent != NULL && paused_left_child != NULL);
  TEST_CHECK(tnt_routing_left(paused_split_parent) == paused_left_child);
  TEST_CHECK(tnt_routing_right(paused_split_parent) == NULL);
  parent_lu_parent = paused_split_parent->lu_parent;
  left_lu_parent = paused_left_child->lu_parent;

  /*
   * At this publication boundary, a left-routed READ reaches the new empty
   * child and walks lu_parent, while a right-routed READ stops at the old
   * parent because that child is not published yet. Both must find the item.
   */
  expect_direct_index_hit(16);
  expect_direct_index_hit(48);

  for (size_t i = 0; i < 2; i++) {
    TEST_CHECK(pthread_create(&client_threads[i], NULL,
                              concurrent_rebalance_client, &clients[i]) == 0);
    TEST_CHECK(wait_for_flag(&clients[i].ready, REQUEST_TIMEOUT_SECONDS));
  }

  TEST_CHECK(pthread_create(&rebalance_thread, NULL, rebalance_thread_main,
                            &rebalance_state) == 0);
  TEST_CHECK(wait_for_flag(&rebalance_state.started, REQUEST_TIMEOUT_SECONDS));

  /* Restructuring must remain blocked while a split phase is active. */
  TEST_CHECK(timed_join(rebalance_thread, 1000) == ETIMEDOUT);
  TEST_CHECK(tnt_routing_right(paused_split_parent) == NULL);
  TEST_CHECK(atomic_load_explicit(&paused_split_parent->child_flag,
                                  memory_order_acquire) == 0);

  /* Once restructuring is queued, a later split must not barge ahead. */
  TEST_CHECK(pthread_create(&late_split_thread, NULL,
                            split_phase_main, &late_split) == 0);
  TEST_CHECK(wait_for_flag(&late_split.started, REQUEST_TIMEOUT_SECONDS));
  TEST_CHECK(timed_join(late_split_thread, 1000) == ETIMEDOUT);
  TEST_CHECK(!atomic_load_explicit(&late_split.acquired,
                                   memory_order_acquire));

  atomic_store_explicit(&split_hook_release, true, memory_order_release);
  TEST_CHECK(wait_for_request(split_request));
  TEST_CHECK(wait_for_flag(&rebalance_hook_reached,
                           REQUEST_TIMEOUT_SECONDS));
  TEST_CHECK(!atomic_load_explicit(&late_split.acquired,
                                   memory_order_acquire));

  /* Prepared restructuring must not hold the root write lock. */
  expect_present_range(1, 64);

  atomic_store_explicit(&rebalance_hook_release, true, memory_order_release);
  TEST_CHECK(wait_for_flag(&rebalance_postpublish_hook_reached,
                           REQUEST_TIMEOUT_SECONDS));
  TEST_CHECK(!atomic_load_explicit(&late_split.acquired,
                                   memory_order_acquire));

  /* Published topology and retired-slot cleanup must not hold the root lock. */
  expect_present_range(1, 64);

  atomic_store_explicit(&rebalance_postpublish_hook_release, true,
                        memory_order_release);
  TEST_CHECK(pthread_join(rebalance_thread, NULL) == 0);
  TEST_CHECK(rebalance_state.status == TNT_REBALANCE_SUCCESS);
  TEST_CHECK(pthread_join(late_split_thread, NULL) == 0);
  TEST_CHECK(atomic_load_explicit(&late_split.acquired,
                                  memory_order_acquire));

  for (size_t i = 0; i < 2; i++)
    atomic_store_explicit(&clients[i].stop, true, memory_order_release);
  for (size_t i = 0; i < 2; i++) {
    TEST_CHECK(pthread_join(client_threads[i], NULL) == 0);
    TEST_CHECK(clients[i].operations != 0);
  }
  slab_set_split_midpoint_test_hook(NULL);
  tnt_set_rebalance_precommit_test_hook(NULL);
  tnt_set_rebalance_postpublish_test_hook(NULL);

  TEST_CHECK(atomic_load_explicit(&paused_split_parent->child_flag,
                                  memory_order_acquire) == 1);
  TEST_CHECK(paused_split_parent->lu_parent == parent_lu_parent);
  TEST_CHECK(paused_left_child->lu_parent == left_lu_parent);
  TEST_CHECK(validate_routing_tree(paused_split_parent));
  expect_present_range(1, 64);

  snapshot = take_topology_snapshot(paused_split_parent);
  TEST_CHECK(snapshot.count == 3);
  TEST_CHECK(snapshot.inorder[0] == paused_left_child);
  TEST_CHECK(snapshot.inorder[1] == paused_split_parent);

  for (size_t pass = 0; pass < 3; pass++) {
    uint64_t *versions = capture_read_versions(1, 64);

    TEST_CHECK(tnt_rebalancing() == TNT_REBALANCE_SUCCESS);
    TEST_CHECK(topology_matches_snapshot(&snapshot, paused_split_parent));
    expect_read_versions(1, 64, versions);
    free(versions);
  }
  free_topology_snapshot(&snapshot);

  size_t nodes_before_split =
      count_routing_nodes(find_routing_root(paused_split_parent));
  uint64_t last_inserted = 64;
  for (uint64_t key = 65; key <= 160; key++) {
    (void)run_and_wait(TEST_UPSERT, key, key);
    last_inserted = key;
    if (count_routing_nodes(find_routing_root(paused_split_parent)) >
        nodes_before_split)
      break;
  }
  TEST_CHECK(count_routing_nodes(find_routing_root(paused_split_parent)) >
             nodes_before_split);
  TEST_CHECK(validate_routing_tree(paused_split_parent));
  expect_present_range(1, last_inserted);

  snapshot = take_topology_snapshot(paused_split_parent);
  uint64_t *versions = capture_read_versions(1, last_inserted);
  TEST_CHECK(tnt_rebalancing() == TNT_REBALANCE_SUCCESS);
  TEST_CHECK(topology_matches_snapshot(&snapshot, paused_split_parent));
  expect_read_versions(1, last_inserted, versions);
  free(versions);
  free_topology_snapshot(&snapshot);

  TEST_CHECK(atomic_load_explicit(&wrong_key_callbacks,
                                  memory_order_relaxed) == 0);
  TEST_CHECK(atomic_load_explicit(&read_tombstones,
                                  memory_order_relaxed) == 0);
  puts("deterministic mid-split rebalance test passed");
}

static int run_acceptance_phase(const char *phase, const char *directory) {
  if (snprintf(test_db_dir, sizeof(test_db_dir), "%s", directory) >=
      (int)sizeof(test_db_dir))
    return EXIT_FAILURE;

  init_workers();
  if (!strcmp(phase, "basic")) {
    (void)run_and_wait(TEST_UPSERT, 10, 1);
    (void)run_and_wait(TEST_DELETE, 10, 2);
    expect_read(10, false);

    (void)run_and_wait(TEST_DELETE, 200, 3);
    expect_read(200, false);

    (void)run_and_wait(TEST_UPSERT, 10, 4);
    expect_read(10, true);
  } else if (!strcmp(phase, "moving")) {
    for (uint64_t key = 1; key <= 64; key++)
      (void)run_and_wait(TEST_UPSERT, key, key);
    (void)run_and_wait(TEST_DELETE, 1, 65);
    expect_read(1, false);
  } else if (!strcmp(phase, "persist-delete")) {
    (void)run_and_wait(TEST_UPSERT, 10, 1);
    (void)run_and_wait(TEST_DELETE, 10, 2);
  } else if (!strcmp(phase, "read-missing")) {
    expect_read(10, false);
  } else if (!strcmp(phase, "restart-upsert")) {
    (void)run_and_wait(TEST_UPSERT, 10, 3);
  } else if (!strcmp(phase, "read-present")) {
    expect_read(10, true);
  } else if (!strcmp(phase, "rebalance")) {
    for (uint64_t key = 1; key <= 256; key++)
      (void)run_and_wait(TEST_UPSERT, key, key);
    (void)run_and_wait(TEST_DELETE, 1, 257);
    TEST_CHECK(tnt_rebalancing() == TNT_REBALANCE_SUCCESS);
    expect_read(1, false);
  } else if (!strcmp(phase, "rebalance-mid-split")) {
    run_mid_split_rebalance_test();
  } else {
    fprintf(stderr, "unknown acceptance phase: %s\n", phase);
    return EXIT_FAILURE;
  }
  return atomic_load_explicit(&wrong_key_callbacks, memory_order_relaxed) ||
                 atomic_load_explicit(&read_tombstones,
                                      memory_order_relaxed)
             ? EXIT_FAILURE
             : EXIT_SUCCESS;
}

static bool run_child_phase(const char *executable, const char *directory,
                            const char *phase) {
  pid_t child = fork();
  if (child == 0) {
    execl(executable, executable, "--phase", phase, directory, NULL);
    _exit(127);
  }
  if (child < 0)
    return false;

  int status;
  return waitpid(child, &status, 0) == child && WIFEXITED(status) &&
         WEXITSTATUS(status) == 0;
}

static int run_acceptance_tests(const char *executable) {
  static const char *single_phases[] = {
      "basic", "moving", "rebalance", "rebalance-mid-split"};

  for (size_t i = 0; i < sizeof(single_phases) / sizeof(single_phases[0]);
       i++) {
    char directory[] = "/tmp/stellardb-delete-accept.XXXXXX";
    if (!mkdtemp(directory))
      return EXIT_FAILURE;
    bool passed = run_child_phase(executable, directory, single_phases[i]);
    remove_test_directory(directory);
    if (!passed)
      return EXIT_FAILURE;
  }

  char directory[] = "/tmp/stellardb-delete-restart.XXXXXX";
  if (!mkdtemp(directory))
    return EXIT_FAILURE;
  static const char *restart_phases[] = {
      "persist-delete", "read-missing", "restart-upsert", "read-present"};
  bool passed = true;
  for (size_t i = 0;
       passed && i < sizeof(restart_phases) / sizeof(restart_phases[0]); i++)
    passed = run_child_phase(executable, directory, restart_phases[i]);
  remove_test_directory(directory);

  if (passed)
    puts("DELETE acceptance tests passed");
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}

int main(int argc, char **argv) {
  if (argc == 4 && !strcmp(argv[1], "--phase"))
    return run_acceptance_phase(argv[2], argv[3]);
  if (argc == 2 && !strcmp(argv[1], "--accept"))
    return run_acceptance_tests(argv[0]);
  if (argc == 2 && !strcmp(argv[1], "--rebalance-concurrent")) {
    char directory[] = "/tmp/stellardb-rebalance-concurrent.XXXXXX";

    if (!mkdtemp(directory))
      return EXIT_FAILURE;
    bool passed =
        run_child_phase(argv[0], directory, "rebalance-mid-split");
    remove_test_directory(directory);
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
  }

  size_t clients = argc > 1 ? strtoul(argv[1], NULL, 10) : DEFAULT_CLIENTS;
  size_t operations = argc > 2 ? strtoul(argv[2], NULL, 10) : DEFAULT_OPS;
  unsigned int seed = argc > 3 ? strtoul(argv[3], NULL, 10) : 1;

  if (!clients || !operations || !mkdtemp(test_db_dir)) {
    perror("worker stress setup");
    return EXIT_FAILURE;
  }

  init_workers();

  for (uint64_t key = 1; key <= INITIAL_KEYS; key++)
    (void)run_and_wait(TEST_UPSERT, key, key);

  pthread_t *threads = calloc(clients, sizeof(*threads));
  struct client_arg *args = calloc(clients, sizeof(*args));
  if (!threads || !args)
    return EXIT_FAILURE;

  for (size_t i = 0; i < clients; i++) {
    args[i].operations = operations;
    args[i].seed = seed + (unsigned int)(i * 0x9e3779b9U);
    if (pthread_create(&threads[i], NULL, client_main, &args[i]))
      return EXIT_FAILURE;
  }
  for (size_t i = 0; i < clients; i++)
    pthread_join(threads[i], NULL);

  for (uint64_t key = 1; key <= KEYSPACE; key++)
    (void)run_and_wait(TEST_DELETE, key, seed);
  uint64_t final_present = 0;
  for (uint64_t key = 1; key <= KEYSPACE; key++) {
    struct test_request *request = run_and_wait(TEST_READ, key, seed);
    final_present += request->returned_item;
  }

  uint64_t wrong_keys =
      atomic_load_explicit(&wrong_key_callbacks, memory_order_relaxed);
  uint64_t tombstone_reads =
      atomic_load_explicit(&read_tombstones, memory_order_relaxed);
  uint64_t nontombstone_deletes =
      atomic_load_explicit(&delete_nontombstones, memory_order_relaxed);
  printf("DELETE worker stress: clients=%zu ops/client=%zu seed=%u "
         "reads=%lu writes=%lu wrong_keys=%lu read_tombstones=%lu "
         "delete_nontombstones=%lu final_present=%lu\n",
         clients, operations, seed,
         atomic_load_explicit(&completed_reads, memory_order_relaxed),
         atomic_load_explicit(&completed_writes, memory_order_relaxed),
         wrong_keys, tombstone_reads, nontombstone_deletes, final_present);

  remove_test_directory(test_db_dir);
  return wrong_keys || tombstone_reads || final_present ? EXIT_FAILURE
                                                        : EXIT_SUCCESS;
}
