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

enum test_operation {
  TEST_READ,
  TEST_UPSERT,
  TEST_DELETE,
};

struct test_request {
  struct slab_callback callback;
  _Atomic bool done;
  bool returned_item;
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

static void init_workers(void) {
  init_default_config(&cfg);
  cfg.kv_size = TEST_ITEM_SIZE;
  cfg.max_file_size = PAGE_SIZE;
  cfg.page_cache_size = PAGE_SIZE * 4096;
  cfg.with_reins = 0;
  cfg.with_rebal = 0;
  slab_workers_init(1, 2, 2);
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
    tnt_rebalancing();
    expect_read(1, false);
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
  static const char *single_phases[] = {"basic", "moving", "rebalance"};

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
