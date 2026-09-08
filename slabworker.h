#ifndef SLAB_WORKER_H
#define SLAB_WORKER_H 1

#include "pagecache.h"

struct slab_callback;
struct slab_context;

void kv_read_async(struct slab_callback *callback);
void kv_add_async(struct slab_callback *callback);
void kv_upsert_async(struct slab_callback *callback);
void kv_remove_async(struct slab_callback *callback);

void kv_upsert_async_no_lookup(struct slab_callback *callback, struct slab *s,
                               size_t slab_idx);
/*
 * Non-blocking variant for I/O-worker-originated writes (reinsertion on
 * read): returns 1 if enqueued, 0 if the destination queue is full. A worker
 * must never spin on another worker's queue.
 */
int kv_add_async_no_lookup_try(struct slab_callback *callback, struct slab *s,
                               size_t slab_idx);
/* Park a copy whose enqueue failed; retried by this worker each loop. */
void reins_defer(struct slab_callback *callback);
void kv_add_async_no_lookup(struct slab_callback *callback, struct slab *s,
                            size_t slab_idx);

typedef struct index_scan tree_scan_res_t;
void kv_read_async_no_lookup(struct slab_callback *callback, struct slab *s,
                             size_t slab_idx, uint64_t count);

size_t get_database_size(void);

void slab_workers_init(int nb_disks, int nb_workers_per_disk,
                       int nb_distributors_per_disk);
int get_nb_workers(void);
int get_nb_distributors(void);
/*
 * One-second average non-wait time for each worker pool, in [0, 100].
 * Workers contribute zero before their first sample and after an idle sample
 * has aged out.
 */
unsigned int get_distributor_utilization(void);
unsigned int get_io_worker_utilization(void);
int restructuring_worker_init(void);
/* Bounded, best-effort hints; enqueue never waits for queue space or maintenance.
 * Descriptors are retained for the process lifetime. No reference is pinned. */
void stale_invalidation_enqueue(struct slab *destination, uint64_t key);
#ifdef STELLAR_TESTING
uint64_t stale_invalidation_pending(void);
#endif
/* Process at most one batch, serialized with pruning/migration. Call without
 * holding maintenance or slab locks. Also usable by tests that drive
 * publication directly without starting the worker threads. */
size_t stale_invalidation_drain(void);
void *kv_read_sync(void *item);  // Unsafe
struct pagecache *get_pagecache(struct slab_context *ctx);
struct pagecache *get_scancache(struct slab_context *ctx);
struct io_context *get_io_context(struct slab_context *ctx);
uint64_t get_rdt(struct slab_context *ctx);
void set_rdt(struct slab_context *ctx, uint64_t val);
int get_nb_disks(void);
struct slab *get_item_slab(int worker_id, void *item);
size_t get_item_size(char *item);
struct slab_context *get_slab_context(void *item);

void increase_processed(struct slab_context *ctx);
struct slab_context *get_slab_context_uidx(uint64_t items_per_page, uint64_t idx);
int get_worker_ucb(struct slab_callback *cb);
void flush_batched_load(void);
void slab_workers_drain_distributors(void);
#endif
