#include "headers.h"

#include <errno.h>
#include <time.h>

/*
 * A slab worker takes care of processing requests sent to the KV-Store.
 * E.g.:
 *    kv_add_async(...) results in a request being enqueued
 * (enqueue_slab_callback function) into a slab worker The worker then dequeues
 * the request, calls functions of slab.c to figure out where the item is on
 * disk (or where it should be placed).
 *
 * Because we use async IO, the worker can enqueue/dequeue more callbacks while
 * IOs are done by the drive (complete_processed_io(...)).
 *
 * A slab worker has its own slab, no other thread should touch the slab. This
 * is straightforward in the current design: a worker sends IO requests for its
 * slabs and processes answers for its slab only.
 *
 * We have the following files on disk:
 *  If we have W disk workers per disk
 *  If we have S slab workers
 *  And Y disks
 *  Then we have W * S * Y files for any given item size:.
 *  /scratchY/slab-a-w-x = slab worker a, disk worker w, item size x on disk Y
 *
 * The slab.c functions abstract many disks into one, so
 *   /scratch** /slab-a-*-x  is the same virtual file
 * but it is a different slab from
 *   /scratch** /slab-b-*-x
 * To find in which slab to insert an element (i.e., which slab worker to use),
 * we use the get_slab function bellow.
 */

static int nb_workers = 0;
static int nb_distributors = 0;
static int nb_disks = 0;
static int nb_workers_launched = 0;
static int nb_workers_ready = 0;

static pthread_mutex_t restructuring_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t restructuring_cond = PTHREAD_COND_INITIALIZER;
static _Atomic int restructuring_started = 0;
static int restructuring_requested = 0;

uint64_t nb_totals;
int try_fsst = 0;
_Atomic size_t epoch;


// static struct pagecache *pagecaches __attribute__((aligned(64)));
int get_nb_distributors(void) { return nb_distributors; }

int get_nb_workers(void) { return nb_workers; }

int get_nb_disks(void) { return nb_disks; }

/*
 * Worker context - Each worker thread in KVell has one of these structure
 */
size_t slab_sizes[] = {100, 128, 256, 400, 512, 1024, 1365, 2048, 4096};
struct slab_context {
  size_t worker_id __attribute__((aligned(64)));  // ID
  struct slab_callback **callbacks;  // Callbacks associated with the requests
  volatile size_t buffered_callbacks_idx;  // Number of requests enqueued or in
                                           // the process of being enqueued
  volatile size_t sent_callbacks;          // Number of requests fully enqueued
  volatile size_t processed_callbacks;     // Number of requests fully submitted
                                           // and processed on disk
  size_t max_pending_callbacks;  // Maximum number of enqueued requests
  struct pagecache *pagecache __attribute__((aligned(64)));
  struct io_context *io_ctx;
  uint64_t rdt;  // Latest timestamp
  _Atomic unsigned int utilization;
  _Atomic uint64_t utilization_sample_ms;
} *slab_contexts;

_Static_assert(DISTRIBUTOR_HIGH_UTIL >= 0 && DISTRIBUTOR_HIGH_UTIL <= 100,
               "DISTRIBUTOR_HIGH_UTIL must be a percentage");
_Static_assert(IO_WORKER_LOW_UTIL >= 0 && IO_WORKER_LOW_UTIL <= 100,
               "IO_WORKER_LOW_UTIL must be a percentage");
_Static_assert(WORKER_UTILIZATION_PERIOD_MS > 0,
               "WORKER_UTILIZATION_PERIOD_MS must be positive");

static uint64_t monotonic_ms(void) {
  struct timespec now;

  clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000LU + (uint64_t)now.tv_nsec / 1000000LU;
}

static unsigned int get_pool_utilization(size_t first, size_t count) {
  uint64_t total = 0;
  uint64_t now = monotonic_ms();

  if (slab_contexts == NULL || count == 0)
    return 0;
  for (size_t i = first; i < first + count; i++) {
    uint64_t sampled = atomic_load_explicit(
        &slab_contexts[i].utilization_sample_ms, memory_order_acquire);

    if (sampled == 0 || now - sampled > 2LU * WORKER_UTILIZATION_PERIOD_MS)
      continue;
    total += atomic_load_explicit(&slab_contexts[i].utilization,
                                  memory_order_relaxed);
  }
  return (unsigned int)(total / count);
}

unsigned int get_distributor_utilization(void) {
  return get_pool_utilization(0, (size_t)nb_distributors);
}

unsigned int get_io_worker_utilization(void) {
  return get_pool_utilization((size_t)nb_distributors, (size_t)nb_workers);
}

static int restructuring_utilization_thresholds_met(void) {
  return get_distributor_utilization() >= DISTRIBUTOR_HIGH_UTIL &&
         get_io_worker_utilization() <= IO_WORKER_LOW_UTIL;
}

static void maybe_wake_restructuring_worker(void) {
  if (!atomic_load_explicit(&restructuring_started, memory_order_acquire) ||
      !restructuring_utilization_thresholds_met())
    return;

  pthread_mutex_lock(&restructuring_lock);
  restructuring_requested = 1;
  pthread_cond_signal(&restructuring_cond);
  pthread_mutex_unlock(&restructuring_lock);
}

static void publish_utilization_sample(struct slab_context *ctx,
                                       uint64_t elapsed,
                                       uint64_t wait_cycles) {
  if (wait_cycles > elapsed)
    wait_cycles = elapsed;
  unsigned int utilization =
      (unsigned int)((elapsed - wait_cycles) * 100LU / elapsed);

  atomic_store_explicit(&ctx->utilization, utilization,
                        memory_order_relaxed);
  atomic_store_explicit(&ctx->utilization_sample_ms, monotonic_ms(),
                        memory_order_release);
  maybe_wake_restructuring_worker();
}


void increase_processed(struct slab_context *ctx) {
  __sync_fetch_and_add(&ctx->processed_callbacks, 1);
}

struct pagecache *get_pagecache(struct slab_context *ctx) {
  return ctx->pagecache;
}

struct io_context *get_io_context(struct slab_context *ctx) {
  return ctx->io_ctx;
}

uint64_t get_rdt(struct slab_context *ctx) { return ctx->rdt; }

void set_rdt(struct slab_context *ctx, uint64_t val) { ctx->rdt = val; }

/*
 * When a request is submitted by a user, it is enqueued. Functions to do that.
 */

/* Get next available slot in a workers's context */
static size_t get_slab_buffer(struct slab_context *ctx) {
  size_t next_buffer = __sync_fetch_and_add(&ctx->buffered_callbacks_idx, 1);
  while (1) {
    volatile size_t pending = next_buffer - ctx->processed_callbacks;
    if (pending >= ctx->max_pending_callbacks) {  // Queue is full, wait
      NOP10();
      if (!PINNING) usleep(2);
    } else {
      break;
    }
  }
  return next_buffer % ctx->max_pending_callbacks;
}

/* Once we get a slot, we fill it, and then submit it */
static size_t submit_slab_buffer(struct slab_context *ctx, int buffer_idx) {
  while (1) {
    if (ctx->sent_callbacks % ctx->max_pending_callbacks !=
        buffer_idx) {  // Somebody else is enqueuing a request, wait!
      NOP10();
    } else {
      break;
    }
  }
  return __sync_fetch_and_add(&ctx->sent_callbacks, 1);
}

/* Requests are statically attributed to workers using this function */
struct slab_context *get_slab_context(void *item) {
    /* thread-local counter */
    static __thread size_t rr_counter = 0;
    size_t nb = get_nb_distributors();
    /* post-increment 후 모듈로 처리 */
    size_t idx = (rr_counter++) % nb;
    return &slab_contexts[idx];
}

struct slab_context *get_slab_context_uidx(uint64_t items_per_page, uint64_t idx) {
  return &slab_contexts[((idx / items_per_page) % get_nb_workers()) + get_nb_distributors()];
}

size_t get_item_size(char *item) {
  struct item_metadata *meta = (struct item_metadata *)item;
  return item_stored_size(meta);
}

static struct slab *get_slab(struct slab_context *ctx, void *item,
                             uint64_t *sidx, index_entry_t *old_e) {
  struct item_metadata *meta = (struct item_metadata *)item;
  char *item_key = &item[sizeof(*meta)];
  uint64_t key = *(uint64_t *)item_key;
  uint64_t idx;
  struct tree_entry *tree = tnt_subtree_get((void *)key, &idx, old_e);

  if (!tree) die("Item is too big\n");

  *sidx = idx;
  return tree->slab;
}

static void enqueue_slab_callback(struct slab_context *ctx,
                                  enum slab_action action,
                                  struct slab_callback *callback) {
  size_t buffer_idx = get_slab_buffer(ctx);
  callback->action = action;
  ctx->callbacks[buffer_idx] = callback;
  add_time_in_payload(callback, TIMING_STAGE_REQUEST_ENQUEUED);
  submit_slab_buffer(ctx, buffer_idx);
}

/*
 * KVell API - These functions are called from user context
 */
void *kv_read_sync(void *item) {
  struct slab_context *ctx = get_slab_context(item);
  uint64_t i;
  struct slab *s = get_slab(ctx, item, &i, NULL);
  // Warning, this is very unsafe, the lookup might not be performed in the
  // worker context => race! We only use that during init.
  R_LOCK(&s->tree_lock);
  index_entry_t *e = tnt_index_lookup_utree(s->subtree, item);
  R_UNLOCK(&s->tree_lock);
  if (e)
    return read_item(s, GET_SIDX(e->slab_idx));
  else
    return NULL;
}

void kv_read_async(struct slab_callback *callback) {
  struct slab_context *ctx = get_slab_context(callback->item);
  callback->ctx = ctx;
  return enqueue_slab_callback(ctx, READ, callback);
}

void kv_read_async_no_lookup(struct slab_callback *callback, struct slab *s,
                             size_t slab_idx, size_t count) {
  struct slab_context *ctx = get_slab_context_uidx((PAGE_SIZE/s->item_size), slab_idx);
  // struct slab_context *ctx = get_slab_context(callback->item);
  callback->ctx = ctx;
  callback->slab = s;
  callback->slab_idx = slab_idx;
  return enqueue_slab_callback(ctx, READ_NO_LOOKUP, callback);
}

/*
 * A client may hand back a record it just read, and READ returns a pointer
 * into the page, so a reinserted copy's shy flag could ride into a real write.
 * Client writes are never shy: strip it at the entry points.
 */
void kv_add_async(struct slab_callback *callback) {
  struct slab_context *ctx = get_slab_context(callback->item);
  item_clear_shy((struct item_metadata *)callback->item);
  callback->ctx = ctx;
  enqueue_slab_callback(ctx, ADD, callback);
}

void kv_upsert_async(struct slab_callback *callback) {
  struct slab_context *ctx = get_slab_context(callback->item);
  item_clear_shy((struct item_metadata *)callback->item);
  callback->ctx = ctx;
  return enqueue_slab_callback(ctx, UPSERT, callback);
}

void kv_remove_async(struct slab_callback *callback) {
  struct slab_context *ctx = get_slab_context(callback->item);
  item_clear_shy((struct item_metadata *)callback->item);
  callback->ctx = ctx;
  return enqueue_slab_callback(ctx, DELETE, callback);
}

void kv_add_async_no_lookup(struct slab_callback *callback, struct slab *s,
                            size_t slab_idx) {
  struct slab_context *ctx = get_slab_context_uidx((PAGE_SIZE/s->item_size), slab_idx);
  callback->ctx = ctx;
  callback->slab = s;
  callback->slab_idx = slab_idx;
  return enqueue_slab_callback(ctx, ADD_NO_LOOKUP, callback);
}

void kv_upsert_async_no_lookup(struct slab_callback *callback, struct slab *s,
                               size_t slab_idx) {
  struct slab_context *ctx = get_slab_context_uidx((PAGE_SIZE/s->item_size), slab_idx);
  callback->ctx = ctx;
  callback->slab = s;
  callback->slab_idx = slab_idx;
  return enqueue_slab_callback(ctx, UPSERT_NO_LOOKUP, callback);
}
void kv_fsst_async_no_lookup(struct slab_callback *callback, struct slab *s,
                             size_t slab_idx) {
  struct slab_context *ctx = get_slab_context_uidx((PAGE_SIZE/s->item_size), slab_idx);
  callback->ctx = ctx;
  callback->slab = s;
  callback->slab_idx = slab_idx;
  return enqueue_slab_callback(ctx, UPSERT_NO_LOOKUP, callback);
}

static void complete_read_miss(struct slab_callback *callback) {
  add_time_in_payload(callback, TIMING_STAGE_IO_COMPLETE);
  if (callback->cb) callback->cb(callback, NULL);
}

/*
 * Worker context
 */

/* Dequeue enqueued callbacks */
static void worker_dequeue_requests(struct slab_context *ctx) {
  size_t retries = 0;
  size_t sent_callbacks = ctx->sent_callbacks;
  size_t pending = sent_callbacks - ctx->processed_callbacks;
  if (pending == 0) return;
again:
  for (size_t i = 0; i < pending; i++) {
    struct slab_callback *callback =
        ctx->callbacks[ctx->processed_callbacks % ctx->max_pending_callbacks];
    enum slab_action action = callback->action;
    add_time_in_payload(callback, TIMING_STAGE_REQUEST_DEQUEUED);

    index_entry_t *e = NULL;
    struct tree_entry *tree = NULL;

    switch (action) {
      case ADD_NO_LOOKUP:
      case UPSERT_NO_LOOKUP:
        upsert_item_async(callback);
        break;
      case READ_NO_LOOKUP: {
        // slab idx에 카운트 담아옴
	if (cfg.with_reins) {
          struct slab *s = callback->slab;
          uint64_t curr_epoch = atomic_load_explicit(&epoch, memory_order_acquire);
          uint64_t slab_epoch = atomic_load_explicit(&s->cur_ep, memory_order_acquire);
          uint64_t cnt = 0;

          if (slab_epoch == curr_epoch) {
             // 같은 에포크: epcount만 증가
             cnt = atomic_fetch_add_explicit(&s->epcnt, 1, memory_order_relaxed);
	   } else {
             // 에포크가 바뀌었으므로, 
             // cur_ep를 최신으로 바꾸고 epcount→prev_epcount 교환 후 epcount=1
             uint64_t old_count = atomic_exchange_explicit(
               &s->epcnt, 0, memory_order_relaxed);
             atomic_store_explicit(&s->cur_ep, curr_epoch, memory_order_release);
             atomic_store_explicit(&s->prev_epcnt, old_count, memory_order_relaxed);
             atomic_store_explicit(&s->epcnt, 1, memory_order_relaxed);
             if (curr_epoch % 10 == 0) {
	       size_t num_words = (((s->size_on_disk + PAGE_SIZE - 1) / PAGE_SIZE) + 63) / 64;
               for (size_t i = 0; i < num_words; i++)
                 __atomic_exchange_n(&s->hot_bits[i], 0ULL, __ATOMIC_RELAXED);
             }
           }

          mark_page_hot(s, item_page_num(s, callback->slab_idx));
          uint64_t tree_depth = tnt_get_depth();

          if (cnt == (size_t)(cfg.epoch/20)
            &&  s->upward_maxlen >= (tree_depth/3)) {
            int expected = 0;

            /* CAS, so two I/O workers cannot both enqueue the slab. */
            if (atomic_compare_exchange_strong_explicit(
                    &s->queued, &expected, 1, memory_order_acq_rel,
                    memory_order_acquire)) {
              printf("Reinsert: %lu\n", s->seq);
              bgq_enqueue(GC, s);
            }
          }
	}
        read_item_async(callback);
        break;
      }
      case READ:
        e = tnt_index_lookup(callback, callback->item);
        if (!e) {  // Item is not in DB
          __sync_add_and_fetch(&try_fsst, 1);
          complete_read_miss(callback);
          break;
        } else {
          struct slab *s = e->slab;
          callback->slab = s;
          callback->slab_idx = GET_SIDX(e->slab_idx);
          /* tnt_index_lookup() already holds the read reference. */
          kv_read_async_no_lookup(callback, callback->slab, callback->slab_idx,
                                  0);
        }
        break;
      case DELETE:
        if (item_is_legacy(callback->item))
          die("Attempted to delete with legacy item metadata\n");
        item_encode_tombstone(callback->item);

        /* fall through */
      case ADD:
        /*
         * ADD is an alias of UPSERT. The KVell ADD died on a duplicate key;
         * for a new key the two paths were identical, and the duplicate check
         * made the load phase abort on a deleted-then-re-added key.
         */
      case UPSERT:
        tree = centree_lookup_and_reserve(callback->item, 
                          &callback->slab_idx, &e);
        if (!e) {
          __sync_add_and_fetch(&try_fsst, 1);
          callback->slab = tree->slab;
          add_time_in_payload(callback, TIMING_STAGE_STORAGE_TARGET_READY);
          add_item_async(callback);
          // read_item_async_from_fsst(callback);
          break;
        }

        add_time_in_payload(callback, TIMING_STAGE_STORAGE_TARGET_READY);
	callback->slab = tree->slab;
        //callback->slab = get_slab(ctx, callback->item, &callback->slab_idx, e);

        if (e && callback->fsst_slab == NULL) {
          callback->fsst_slab = e->slab;
          callback->fsst_idx = GET_SIDX(e->slab_idx);
        }

        remove_and_add_item_async(callback);
        break;
      case FSST_NO_LOOKUP:
        break;

      default:
        die("Unknown action\n");
    }
    ctx->processed_callbacks++;
    if (NEVER_EXCEED_QUEUE_DEPTH && io_pending(ctx->io_ctx) >= QUEUE_DEPTH)
      break;
  }

  if (WAIT_A_BIT_FOR_MORE_IOS) {
    while (retries < 5 && io_pending(ctx->io_ctx) < QUEUE_DEPTH) {
      retries++;
      pending = ctx->sent_callbacks - ctx->processed_callbacks;
      if (pending == 0) {
        wait_for(10000);
      } else {
        goto again;
      }
    }
  }
}

static void *worker_slab_init(void *pdata) {
  struct slab_context *ctx = pdata;

  __sync_add_and_fetch(&nb_workers_launched, 1);

  pid_t x = syscall(__NR_gettid);
  printf("[SLAB WORKER %lu] tid %d\n", ctx->worker_id, x);
  pin_me_on(ctx->worker_id);

  /* Create the pagecache for the worker */
  ctx->pagecache = calloc(1, sizeof(*ctx->pagecache));
  page_cache_init(ctx->pagecache);

  /* Initialize the async io for the worker */
  ctx->io_ctx = worker_ioengine_init(ctx->max_pending_callbacks);
  __sync_add_and_fetch(&nb_workers_ready, 1);

  /* Main loop: do IOs and process enqueued requests */
  declare_breakdown;
  while (1) {
    ctx->rdt++;

    while (io_pending(ctx->io_ctx)) {
      worker_ioengine_enqueue_ios(ctx->io_ctx);
      __1 worker_ioengine_get_completed_ios(ctx->io_ctx);
      __2 worker_ioengine_process_completed_ios(ctx->io_ctx);
      __3
    }

    volatile size_t pending = ctx->sent_callbacks - ctx->processed_callbacks;
    while (!pending && !io_pending(ctx->io_ctx)) {
      if (!PINNING) {
        usleep(2);
      } else {
        NOP10();
      }
      pending = ctx->sent_callbacks - ctx->processed_callbacks;
    }
    __4

    worker_dequeue_requests(ctx);
    __5  // Process queue

    show_breakdown_periodic_hook(
        WORKER_UTILIZATION_PERIOD_MS, ctx->processed_callbacks, "io_submit",
        "io_getevents", "io_cb", "wait", "slab_cb",
        publish_utilization_sample(ctx, elapsed, __breakdown.evt4));
  }

  return NULL;
}

static void *worker_distributor_init(void *pdata) {
  struct slab_context *ctx = pdata;

  // ctx->fsst_idx = aligned_alloc(PAGE_SIZE, 64*PAGE_SIZE);
  __sync_add_and_fetch(&nb_workers_launched, 1);

  pid_t x = syscall(__NR_gettid);
  printf("[SLAB WORKER %lu] tid %d\n", ctx->worker_id, x);
  pin_me_on(ctx->worker_id);
  if (ctx->worker_id == 0)
    atomic_init(&epoch, 1);

  ctx->io_ctx = worker_ioengine_init(ctx->max_pending_callbacks);
  __sync_add_and_fetch(&nb_workers_ready, 1);

  declare_breakdown;
  while (1) {
    ctx->rdt++;
    if (ctx->rdt % cfg.epoch == 0 && ctx->worker_id == 0) {
      atomic_fetch_add_explicit(&epoch, 1, memory_order_seq_cst);
    }
    volatile size_t pending = ctx->sent_callbacks - ctx->processed_callbacks;
    while (!pending) {
      if (!PINNING) {
        usleep(2);
      } else {
        NOP10();
      }
      pending = ctx->sent_callbacks - ctx->processed_callbacks;
    }
    __4

    worker_dequeue_requests(ctx);
    __5  // Process queue

    show_breakdown_periodic_hook(
        WORKER_UTILIZATION_PERIOD_MS, ctx->processed_callbacks, "io_submit",
        "io_getevents", "io_cb", "wait", "slab_cb",
        publish_utilization_sample(ctx, elapsed, __breakdown.evt4));
    //if (ctx->worker_id == 0)
    //    check_and_handle_tnt(__breakdown.real_start, __breakdown.evt5);
  }

  return NULL;
}

static void *worker_restructuring_init(void *pdata) {
  (void)pdata;

  while (1) {
    pthread_mutex_lock(&restructuring_lock);
    if (cfg.prune_auto) {
      /*
       * -C: wake at least once per period to measure the stale ratio, on top
       * of the utilization-driven signals.
       */
      struct timespec deadline;

      clock_gettime(CLOCK_REALTIME, &deadline);
      deadline.tv_sec += cfg.prune_period_ms / 1000;
      deadline.tv_nsec += (cfg.prune_period_ms % 1000) * 1000000L;
      if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
      }
      while (!restructuring_requested &&
             pthread_cond_timedwait(&restructuring_cond, &restructuring_lock,
                                    &deadline) == 0)
        ;
    } else {
      while (!restructuring_requested)
        pthread_cond_wait(&restructuring_cond, &restructuring_lock);
    }
    restructuring_requested = 0;
    pthread_mutex_unlock(&restructuring_lock);

    while (cfg.with_rebal && restructuring_utilization_thresholds_met() &&
           tnt_rebalancing_needed()) {
      int status = tnt_rebalancing();

      if (status >= 0)
        break;
      fprintf(stderr, "Background rebalancing failed: %s; retrying\n",
              strerror(-status));
      sleep(1);
    }

    /*
     * Both maintenance operations run on this thread, which is what keeps them
     * exclusive; the mutex is there for the callers in main.c and the tests.
     *
     * -C: prune while the stale-slot ratio is at or above the threshold and a
     * candidate exists, whatever the CPU looks like. A dropped candidate
     * (-EAGAIN/-EBUSY/-ENOSPC) ends the burst: the scan would hand back the
     * same triple, and the next period retries. Without -C: one prune per
     * wake-up, behind the utilization gate, as before.
     */
    if (cfg.prune_auto) {
      struct prune_stale m;
      size_t done = 0;
      int status = TNT_PRUNE_NOOP;

      prune_stale_measure(&m);
      while (prune_stale_ratio(&m) >= cfg.prune_stale_ratio) {
        status = tnt_prune_once();
        if (status != TNT_PRUNE_DONE)
          break;
        done++;
        prune_stale_measure(&m);
      }
      if (done)
        printf("Prune trigger: %zu prunes, stale %zu/%zu (%.1f%%)\n", done,
               m.stale, m.reserved, 100.0 * prune_stale_ratio(&m));
      if (status < 0 && status != -EAGAIN && status != -EBUSY &&
          status != -ENOSPC)
        fprintf(stderr, "Background pruning failed: %s\n", strerror(-status));
    } else if (cfg.with_prune && restructuring_utilization_thresholds_met()) {
      int status = tnt_prune_once();

      if (status < 0 && status != -EAGAIN && status != -EBUSY &&
          status != -ENOSPC)
        fprintf(stderr, "Background pruning failed: %s\n", strerror(-status));
    }
  }

  return NULL;
}

int restructuring_worker_init(void) {
  int expected = 0;
  pthread_t thread;

  if (!atomic_compare_exchange_strong_explicit(
          &restructuring_started, &expected, 1,
          memory_order_acq_rel, memory_order_acquire))
    return 0;

  int error = pthread_create(&thread, NULL, worker_restructuring_init, NULL);
  if (error != 0) {
    atomic_store_explicit(&restructuring_started, 0, memory_order_release);
    return -error;
  }
  pthread_detach(thread);
  maybe_wake_restructuring_worker();
  return 0;
}

/*
 * When first loading a slab from disk we need to rebuild the in memory tree,
 * these functions do that.
 */
int add_existing_item(struct slab *s, size_t idx, void *item,
                      struct slab_callback *cb) {
  struct item_metadata *meta = item;

  if (item_is_legacy(meta))
    die("Legacy item metadata found while rebuilding slab %lu\n", s->seq);
  /*
   * An empty slot is skipped, not a stop sign: a crash between a slot's
   * reservation and its page write leaves a hole in the middle of a live
   * leaf, and everything after it is still real data.
   */
  if (item_is_empty(meta))
    return 0;

  char *item_key = &item[sizeof(*meta)];
  uint64_t key = *(uint64_t *)item_key;

#if WITH_FILTER
if ((already = filter_contain(s->filter, (unsigned char *)&key))) {
  #endif
  {
    index_entry_t *prev = tnt_index_lookup_utree(s->subtree, item);

    if (prev != NULL) {
      /*
       * The same key twice in one slab. The runtime rule: a shy record (a
       * copy made by reinsertion) never beats a client record, whatever the
       * slot order; otherwise the later slot wins. This is also what makes an
       * abandoned reinsertion slot harmless after a crash: it always sits in
       * the same slab as, or above, a client record for its key.
       */
      if (item_is_shy(meta) && !sidx_is_shy(prev->slab_idx))
        return 1; /* occupied, but the existing record stays indexed */
      tnt_index_delete(s->subtree, item);
      s->nb_items--;
      __sync_sub_and_fetch(&nb_totals, 1);
    }
  }
#if WITH_FILTER
}
#endif

  meta->rdt = 0;
  __sync_fetch_and_add(&s->nb_items, 1);
  /* last_item is set by the caller from the highest occupied slot, not by count. */
  cb->slab_idx = idx;

  __sync_add_and_fetch(&nb_totals, 1);
  if (item_is_shy(meta))
    tnt_index_add_shy(cb, item); /* the completions' rule keeps applying */
  else
    tnt_index_add(cb, item);

  slab_widen_range(s, key);

#if WITH_FILTER
  if (!already && filter_add((filter_t *)s->filter, (unsigned char *)&key) == 0) {
    printf("Fail adding to filter %p %lu seq/idx %lu/%lu, kvsize: %lu/%lu\n",
           s->filter, key, cb->slab->seq, cb->slab_idx,
           item_key_size(meta), meta->value_size);
    return 0;

  } else if (!filter_contain(s->filter, (unsigned char *)&key)) {
    printf("Error about filter in Rebuilding\n");
  }
#endif

  return 1;
}

/* Returns one past the highest occupied slot in the chunk, or 0 if none. */
size_t process_existing_chunk(struct slab *s, char *data, size_t start,
                              size_t length, struct slab_callback *cb) {
  static __thread declare_periodic_count;
  size_t nb_items_per_page = PAGE_SIZE / s->item_size;
  size_t nb_pages = length / PAGE_SIZE;
  size_t highest = 0;

  for (size_t p = 0; p < nb_pages; p++) {
    size_t page_num =
        ((start + p * PAGE_SIZE) / PAGE_SIZE);  // Physical page to virtual page
    size_t base_idx = page_num * nb_items_per_page;
    size_t current = p * PAGE_SIZE;
    for (size_t i = 0; i < nb_items_per_page; i++) {
      if (add_existing_item(s, base_idx, &data[current], cb) != 0)
        highest = base_idx + 1;
      base_idx++;
      current += s->item_size;
      periodic_count(
          1000, "[REBUILD WORKER] Init - Recovered %lu items", s->nb_items);
    }
  }
  return highest;
}

#define GRANULARITY_REBUILD (2 * 1024 * 1024)  // We rebuild 2MB by 2MB
void rebuild_index(struct slab *s, uint64_t key, char *buf) {
  int fd = s->fd;
  size_t start = 0, end;
  struct slab_callback *callback;

  if (s->size_on_disk == 0) {
    atomic_store_explicit(&s->last_item, 
	s->nb_max_items, memory_order_release);
    atomic_store_explicit(&s->full, 1, memory_order_release);
    subtree_free(s->subtree);
#if WITH_FILTER
    filter_delete(s->filter);
#endif
    s->subtree = NULL;
    return;
  }

  callback = malloc(sizeof(*callback));
  callback->slab = s;

  size_t highest = 0;

  while (1) {
    size_t h;

    end = start + GRANULARITY_REBUILD;
    if (end > slab_data_size(s)) end = slab_data_size(s); /* not the header */
    if (((end - start) % PAGE_SIZE) != 0) end = end - (end % PAGE_SIZE);
    if (((end - start) % PAGE_SIZE) != 0)
      die("File size is wrong (%%PAGE_SIZE!=0)\n");
    if (end == start) break;
    int r = pread(fd, buf, end - start, start);
    if (r != end - start)
      perr("pread failed! Read %d instead of %lu (offset %lu)\n", r,
           end - start, start);
    h = process_existing_chunk(s, buf, start, end - start, callback);
    if (h > highest) highest = h;
    start = end;
  }

  /*
   * Slots are handed out past the highest occupied one, so a hole left by a
   * crash stays empty for good instead of being reused over live data.
   */
  atomic_store_explicit(&s->last_item, highest, memory_order_release);
  if (highest == s->nb_max_items)
    atomic_store_explicit(&s->full, 1, memory_order_release);

  free(callback);
  return;
}

void get_all_keys(uint64_t h, int n, void *data) {
  uint64_t *ks = (uint64_t*)data;
  ks[n] = h;
  return;
}

int compare_uint64(const void *a, const void *b) {
    uint64_t num1 = *(const uint64_t *)a;
    uint64_t num2 = *(const uint64_t *)b;

    if (num1 < num2) {
        return -1;  // num1 is less than num2
    } else if (num1 > num2) {
        return 1;   // num1 is greater than num2
    } else {
        return 0;   // num1 is equal to num2
    }
}

void invalid_indexes(struct slab **sa, int snum, uint64_t *keys) {
  struct slab *s;
  int nks, nkeys;
  centree_node p;
  uint64_t *ks = malloc((cfg.max_file_size/cfg.kv_size) * sizeof(uint64_t));
  char *item = create_unique_item(cfg.kv_size, 0);
  struct item_metadata *meta = (struct item_metadata *)item;
  char *item_key = &item[sizeof(*meta)];

  for (int i = 0; i < snum; i++) {
    s = sa[i];

    // Since everything in sa is a leaf, 
    // just put all keys of it in keys variable unconditionally.
    nkeys = subtree_forall_keys(s->subtree, get_all_keys, keys);
    
    /*
     * History, not routing: after recovery the routing tree is rebuilt
     * balanced and no longer mirrors the history chain.
     */
    p = centree_lu_parent((centree_node)s->centree_node);
    s = p ? p->value.slab : NULL;

    while (s != NULL) {

      if (s->subtree == NULL)
        goto next;

      // First, put all the keys of the moved sub-tree into ks.
      nks = subtree_forall_keys(s->subtree, get_all_keys, ks);

      qsort(keys, nkeys, sizeof(uint64_t), compare_uint64);
      qsort(ks, nks, sizeof(uint64_t), compare_uint64);

      int j = 0, k = 0;
      while (j < nkeys && k < nks) {
        if (keys[j] == ks[k]) {
          *(uint64_t *)item_key = ks[k];
          int r = tnt_index_invalid_utree(s->subtree, item);
          __sync_fetch_and_sub(&s->nb_items, r);
          __sync_sub_and_fetch(&nb_totals, r);
          ks[k] = -1;
          k++;
        } else if (keys[j] < ks[k]) {
          j++;
        } else {
          k++;
        }
      }

      for (k=0; k < nks; k++)
        if (ks[k] != -1)
          keys[nkeys++] = ks[k];

    next:
      // Move to parent
      p = centree_lu_parent((centree_node)s->centree_node);
      s = p ? p->value.slab : NULL;
    }
  }
  free(item);
  free(ks);
}

struct dirent **rebuild_list;
static pthread_lock_t rebuild_lock;
static struct slab **leaf_slab_list;
static int leaf_slab_totals = 0;
static int rebuild_totals = 0;
static int rebuild_ready = 0;
static struct slab **live_slabs;
static size_t nb_live_slabs, next_live_slab;

struct slab *close_and_create_slab(struct slab *s);

static int numeric_sort(const struct dirent **a, const struct dirent **b) {
    int num_a = 0, num_b = 0;
    
    // "slab-" 이후의 숫자 부분 추출
    sscanf((*a)->d_name, "slab-%d", &num_a);
    sscanf((*b)->d_name, "slab-%d", &num_b);
    
    return (num_a - num_b);
}

static void *worker_rebuild_init(void *pdata) {
  struct slab_context *ctx = pdata;
  char *cached_data;
  uint64_t *cached_key;
  int last_insert;
  if (ctx->worker_id == 0) {
    DIR *dir;
    char *path = "/scratch0/kvell";
    if ((dir = opendir(path)) != NULL) {
      // Sort the entries by name
      rebuild_totals = scandir(path, &rebuild_list, NULL, numeric_sort);
      if (rebuild_totals < 0) {
        perror("scandir");
      } else {
        // both trees from the headers; unreachable files are deleted here
        rebuild_slabs(rebuild_totals, rebuild_list);
      }
      nb_live_slabs = slab_recovered(&live_slabs);
      next_live_slab = 0;
      leaf_slab_list = malloc((nb_live_slabs + 1) * sizeof(struct slab*));
    } else {
      perror("Could not open directory");
    }
    INIT_LOCK(&rebuild_lock, NULL);
    __sync_add_and_fetch(&rebuild_ready, 1);
  } else {
    while (__sync_fetch_and_or(&rebuild_ready, 0) == 0) {
      NOP10();
    }
  }

  cached_data = aligned_alloc(PAGE_SIZE, GRANULARITY_REBUILD);

  while (1) {
    struct slab *s = NULL;

    // Each pthread takes the next live slab.
    W_LOCK(&rebuild_lock);
    if (next_live_slab < nb_live_slabs)
      s = live_slabs[next_live_slab++];
    W_UNLOCK(&rebuild_lock);
    if (s == NULL)
      break;

    // Fill the B+-Tree of the subtree. Internal slabs were marked full when
    // the trees were built, whatever their slot count says.
    rebuild_index(s, s->key, cached_data);

    if (atomic_load_explicit(&s->full, memory_order_acquire) == 0) {
      W_LOCK(&rebuild_lock);
      leaf_slab_list[leaf_slab_totals++] = s;
      W_UNLOCK(&rebuild_lock);
    }
  }

  free(cached_data);
  __sync_add_and_fetch(&rebuild_ready, 1);

  while (__sync_fetch_and_or(&rebuild_ready, 0) < (nb_workers + nb_distributors + 1)) {
    NOP10();
  }

  cached_key = aligned_alloc(PAGE_SIZE, GRANULARITY_REBUILD * 3);

  last_insert = 0;
  while (1) {
    int i;
    struct slab *s[10];
    int snum = 0;
    W_LOCK(&rebuild_lock);
    // Each pthread selects one slab to work on.
    for (i = last_insert; i < leaf_slab_totals; i++) {
      if (leaf_slab_list[i] != NULL) {
        s[snum++] = leaf_slab_list[i];
        leaf_slab_list[i] = NULL;
        last_insert = i;
        if (snum == 10)
          break;
      }
    }
    W_UNLOCK(&rebuild_lock);

    invalid_indexes((struct slab **)&s, snum, cached_key);
    if (i == leaf_slab_totals)
      break;
  }


  free(cached_key);
  __sync_add_and_fetch(&rebuild_ready, 1);

  while (__sync_fetch_and_or(&rebuild_ready, 0) < ((nb_workers + nb_distributors) * 2 + 1)) {
    NOP10();
  }

  if (ctx->worker_id == 0) {
    /*
     * A leaf that is physically full but has no children was cut off between
     * taking its final slot and committing its split (children are created
     * before the parent's header names them). Writers to its range would wait
     * forever, so give it its children now.
     */
    for (size_t i = 0; i < nb_live_slabs; i++) {
      struct slab *s = live_slabs[i];
      centree_node node = (centree_node)s->centree_node;

      if (atomic_load_explicit(&s->full, memory_order_acquire) &&
          atomic_load_explicit(&node->child_flag, memory_order_acquire) == 0) {
        fprintf(stderr, "Recovery: slab %lu is a full leaf, splitting it\n",
                s->seq);
        close_and_create_slab(s);
      }
    }
    free(leaf_slab_list);
    for (int i = 0; i < rebuild_totals; i++)
      free(rebuild_list[i]);
    free(rebuild_list);
  }

  pthread_exit(NULL);

  return NULL;
}

void slab_workers_init(int _nb_disks, int nb_workers_per_disk,
                       int nb_distributors_per_disk) {
  size_t max_pending_callbacks = MAX_NB_PENDING_CALLBACKS_PER_WORKER;
  nb_disks = _nb_disks;
  nb_workers = nb_disks * nb_workers_per_disk;
  nb_distributors = nb_disks * nb_distributors_per_disk;
  nb_totals = 0;

  slab_contexts = calloc(nb_workers + nb_distributors, sizeof(*slab_contexts));
  for (size_t w = 0; w < (size_t)(nb_workers + nb_distributors); w++) {
    atomic_init(&slab_contexts[w].utilization, 0);
    atomic_init(&slab_contexts[w].utilization_sample_ms, 0);
  }
  if (!create_root_slab()) {
    pthread_t *t = malloc((nb_distributors + nb_workers)*sizeof(pthread_t));
    for (size_t w = 0; w < nb_distributors + nb_workers; w++) {
      struct slab_context *ctx = &slab_contexts[w];
      ctx->worker_id = w;
      pthread_create(&t[w], NULL, worker_rebuild_init, ctx);
    }
    for (size_t w = 0; w < nb_distributors + nb_workers; w++) {
      pthread_join(t[w], NULL);
    }
    free(t);
    printf("nb_totals: %lu\n", nb_totals);
    /*tnt_print();*/
    /*exit(1);*/
  }

  pthread_t t;
  // pagecaches = calloc(nb_workers, sizeof(*pagecaches));
  for (size_t w = 0; w < nb_distributors; w++) {
    struct slab_context *ctx = &slab_contexts[w];
    ctx->worker_id = w;
    ctx->max_pending_callbacks = max_pending_callbacks;
    ctx->callbacks =
      calloc(ctx->max_pending_callbacks, sizeof(*ctx->callbacks));
    pthread_create(&t, NULL, worker_distributor_init, ctx);
  }

  for (size_t w = nb_distributors; w < nb_distributors + nb_workers; w++) {
    struct slab_context *ctx = &slab_contexts[w];
    ctx->worker_id = w;
    ctx->max_pending_callbacks = max_pending_callbacks;
    ctx->callbacks =
      calloc(ctx->max_pending_callbacks, sizeof(*ctx->callbacks));
    pthread_create(&t, NULL, worker_slab_init, ctx);
  }

  while (*(volatile int *)&nb_workers_ready != nb_workers + nb_distributors) {
    NOP10();
  }
}

size_t get_database_size(void) {
  return nb_totals;
}

void flush_batched_load(void) {
  tree_entry_t *victim = NULL;
  struct slab *s = NULL;
  do {
    victim = pick_garbage_node();
    // select not full
    while (victim && 
      atomic_load_explicit(&victim->slab->full, memory_order_acquire) == 1)
      victim = pick_garbage_node();
    if (!victim)
      break;
    s = victim->slab;
    if (s->nb_batched != 0) {
      for (int i=0; i < s->nb_batched; i++) {
        struct slab_callback *cb = s->batched_callbacks[i];
        kv_add_async_no_lookup(cb, cb->slab, cb->slab_idx);
        s->batched_callbacks[i] = NULL;
      }
      s->nb_batched = 0;
    }
    if (!s->batched_callbacks)
      free(s->batched_callbacks);
  } while (victim);
}
