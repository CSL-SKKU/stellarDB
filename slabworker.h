#ifndef SLAB_WORKER_H
#define SLAB_WORKER_H 1

#include "pagecache.h"

struct slab_callback;
struct slab_context;

void kv_read_async(struct slab_callback *callback);
void kv_add_async(struct slab_callback *callback);
void kv_update_async(struct slab_callback *callback);
void kv_add_or_update_async(struct slab_callback *callback);
void kv_remove_async(struct slab_callback *callback);

void kv_update_async_no_lookup(struct slab_callback *callback, struct slab *s,
                               size_t slab_idx);
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
#endif
