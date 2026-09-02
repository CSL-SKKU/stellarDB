#ifndef SLAB_H
#define SLAB_H 1

#include "ioengine.h"
#define RW_LOCK

#ifdef RW_LOCK
#define INIT_LOCK(l, attr) pthread_rwlock_init(l, attr)
#define R_LOCK(l) pthread_rwlock_rdlock(l)
#define W_LOCK(l) pthread_rwlock_wrlock(l)
#define R_UNLOCK(l) pthread_rwlock_unlock(l)
#define W_UNLOCK(l) pthread_rwlock_unlock(l)
#define pthread_lock_t pthread_rwlock_t
#else
#define INIT_LOCK(l, attr) pthread_spin_init(l, attr)
#define R_LOCK(l) pthread_spin_lock(l)
#define W_LOCK(l) pthread_spin_lock(l)
#define R_UNLOCK(l) pthread_spin_unlock(l)
#define W_UNLOCK(l) pthread_spin_unlock(l)
#define pthread_lock_t pthread_spinlock_t
#endif

struct slab;
struct slab_callback;

typedef void (*slab_split_test_hook_t)(struct slab *parent);

/* Test-only synchronization hook; production leaves it unset. */
void slab_set_split_midpoint_test_hook(slab_split_test_hook_t hook);

/*
 * Slab descriptor lifetime follows its center-tree node: once published, the
 * descriptor's address remains valid for the database process lifetime.
 * Individual resources may be closed during shutdown/maintenance, but the
 * descriptor itself must not be freed while raw node/tree_entry pointers can
 * still refer to it.
 *
 * Header of a slab -- shouldn't contain any pointer as it is persisted on disk.
 */
#define NUM_LOAD_BATCH 64
struct slab {
  struct slab_context *ctx;

  uint64_t key;
  uint64_t min;
  uint64_t max;
  uint64_t seq;

  void *subtree;
  void *centree_node;
#if WITH_FILTER
  void *filter;
#endif
  _Atomic int full;
  pthread_lock_t tree_lock;

  size_t item_size;
  size_t nb_items;   // Number of non freed items
  size_t nb_max_items;
  _Atomic size_t last_item;  // Total number of items, including freed

  // For Reinsertion
  _Atomic int queued;
  _Atomic int upward_maxlen;
  _Atomic size_t cur_ep;
  _Atomic size_t epcnt;
  _Atomic size_t prev_epcnt;
  uint64_t *hot_bits;

  int fd;
  size_t size_on_disk;
  uint64_t update_ref;
  uint64_t read_ref;
  /* Set once when a retired slab's file is closed and unlinked. */
  _Atomic int released;

  unsigned char nb_batched;
  struct slab_callback **batched_callbacks;
};

/* This is the callback enqueued in the engine.
 * slab_callback->item = item looked for (that needs to be freed)
 * item = stored item, or NULL when a READ does not find the key
 */
typedef void(slab_cb_t)(struct slab_callback *, void *item);
enum slab_action {
  ADD,
  UPSERT,
  DELETE,
  READ,
  READ_NO_LOOKUP,
  ADD_NO_LOOKUP,
  UPSERT_NO_LOOKUP,
  FSST_NO_LOOKUP
};
struct slab_callback {
  slab_cb_t *cb;
  slab_cb_t *cb_cb;
  void *payload;
  void *item;

  // Private
  enum slab_action action;
  struct slab *slab;
  union {
    uint64_t slab_idx;
    uint64_t tmp_page_number;  // when we add a new item we don't always know
                               // it's idx directly, sometimes we just know
                               // which page it will be placed on
  };
  struct lru *lru_entry;
  io_cb_t *io_cb;

  struct slab *fsst_slab;
  union {
    uint64_t fsst_idx;
    uint64_t item_nums;
  };
  struct slab_context *ctx;
};

/*
 * Widen [min, max] so it covers key. The range is monotone -- min only ever
 * decreases and max only ever increases, and nothing resets them once the slab
 * is published -- so a CAS loop needs no lock.
 *
 * A reader can catch a widening half-applied, but not in a way that breaks
 * min <= max: whoever reads has already completed its own widening, so max is
 * at least its key and min is at most its key, and a concurrent partial
 * widening can only push min further down, below that key.
 */
static inline void slab_widen_range(struct slab *s, uint64_t key) {
  uint64_t old = s->min;
  while (key < old && !__sync_bool_compare_and_swap(&s->min, old, key))
    old = s->min;
  old = s->max;
  while (key > old && !__sync_bool_compare_and_swap(&s->max, old, key))
    old = s->max;
}

void add_in_tree_for_upsert(struct slab_callback *cb, void *item);

/*
 * Make a slab immutable so a maintenance pass can copy it. Returns the number
 * of slots writers ever reserved, or -EBUSY / -ENOSPC (see slab.c). Only the
 * single pruner may call it.
 */
long slab_freeze(struct slab *s, size_t budget);

struct slab *resize_slab(struct slab *s);

void *read_item(struct slab *s, size_t idx);
void read_item_async(struct slab_callback *callback);
void scan_item_async(struct slab_callback *callback);
void add_item_async(struct slab_callback *callback);
void upsert_item_async(struct slab_callback *callback);
void remove_item_async(struct slab_callback *callback);
void remove_and_add_item_async(struct slab_callback *callback);

off_t item_page_num(struct slab *s, size_t idx);
void mark_page_hot(struct slab *s, size_t page_idx);

int rebuild_slabs(int filenum, struct dirent **file_list);
int create_root_slab(void);
#endif
