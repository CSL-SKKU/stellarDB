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

/* Header of a slab -- shouldn't contain any pointer as it is persisted on disk.
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
  UPDATE,
  DELETE,
  READ,
  READ_NO_LOOKUP,
  ADD_OR_UPDATE,
  ADD_NO_LOOKUP,
  UPDATE_NO_LOOKUP,
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

void add_in_tree_for_update(struct slab_callback *cb, void *item);

struct slab *resize_slab(struct slab *s);

void *read_item(struct slab *s, size_t idx);
void read_item_async(struct slab_callback *callback);
void scan_item_async(struct slab_callback *callback);
void add_item_async(struct slab_callback *callback);
void update_item_async(struct slab_callback *callback);
void remove_item_async(struct slab_callback *callback);
void remove_and_add_item_async(struct slab_callback *callback);

off_t item_page_num(struct slab *s, size_t idx);
void mark_page_hot(struct slab *s, size_t page_idx);

int rebuild_slabs(int filenum, struct dirent **file_list);
int create_root_slab(void);
#endif
