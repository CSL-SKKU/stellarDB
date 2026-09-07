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
 * On-disk header, the last page of every slab file. It carries what recovery
 * needs and nothing else: the slab's id (its file is named slab-<id>), its
 * routing key (the pivot for an internal node, the creation key for a leaf),
 * and the ids of its two history children. lu_parent is not stored -- a node's
 * parent is whichever node lists it as a child. The rest of the page is
 * reserved. The history root's id lives in the ROOT file, replaced atomically
 * by rename.
 */
#define SLAB_HEADER_MAGIC 0x3152414c4c455453ULL /* "STELLAR1" */
struct slab_header {
  uint64_t magic;
  uint64_t id;
  uint64_t key;
  uint64_t level;       /* advisory */
  uint64_t lu_child[2]; /* 0 = none */
};

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
  _Atomic size_t nb_tombstones; // tombstones published into this slab (diagnostic, never decremented)
  size_t nb_max_items;
  _Atomic size_t last_item;  // Total number of items, including freed
  /* Client writes completed into this slab; the scheduler's mark gives the
   * writes since its last pass (the cold-leaf priority of the ILI pruner). */
  _Atomic uint64_t nb_writes, nb_writes_mark;

  // For Reinsertion
  _Atomic int queued;
  _Atomic size_t cur_ep;
  uint64_t *hot_bits;

  int fd;
  size_t size_on_disk;
  uint64_t update_ref;
  uint64_t read_ref;
  /* Set once when a retired slab's file is closed and unlinked. */
  _Atomic int released;
  /*
   * Set once when this slab's contents were rebuilt into a fresh slab that the
   * same centree node now points at (compaction / migration). Readers that
   * loaded the old pointer reload node->value.slab; the old subtree is freed
   * by slab_retire() and the file released when the references drain.
   */
  _Atomic int superseded;

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
  uint64_t user_start;  /* client use: start cycle of a chained request */
  uint32_t upward_len;  /* READ: levels walked from the leaf to find the record (1 = at leaf) */
  uint32_t page_was_hot; /* READ_NO_LOOKUP: the page's hot bit was already set */
  /*
   * Stage stamps (rdtsc) at DEBUG=0, filled by add_time_in_payload():
   * [0] first dequeue (the distributor starts), [1] second enqueue (hand-off
   * to the I/O worker: routing done), [2] second dequeue (the I/O worker
   * starts), [3] routing descent done (LEAF_FOUND: the leaf is known, the
   * history walk starts). payload holds the client's enqueue; the completion
   * computes queue wait, distributor service (split into descent and history
   * walk) and I/O service from them.
   */
  uint64_t t_stage[4];
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
 * Completion of a reinsertion's append. Publishes the entry as shy only if the
 * key is still absent from the destination and the source record is still the
 * authoritative one; otherwise the slot is abandoned. Never frees the callback.
 */
void add_in_tree_for_reinsertion(struct slab_callback *cb, void *item);

/*
 * Make a slab immutable so a maintenance pass can copy it. Returns the number
 * of slots writers ever reserved, or -EBUSY / -ENOSPC (see slab.c). Only the
 * single pruner may call it.
 */
long slab_freeze(struct slab *s, size_t budget);

/*
 * Wait until no write holds a reference on the slab.
 * This is to ensure the slab becomes stable, since the internal nodes
 * may under publish for a brief moment. (not fully immutable yet)
 * This check is neccessary for immutable-ness, because splits happen
 * proactively at the moment of the last slot gets reserved, not when the slab
 * is full with published slots.
 * Anyone about to treat it as immutable (the pruner, before snapshotting inner/outer)
 * waits here first.
 * Should not wait for a slab that is not internal, otherwise it may wait quite a long time,
 * until that slab also becomes internal.
 */
void slab_drain_updates(struct slab *s);

/*
 * Give up a retired slab's memory: its node must already be marked removed.
 * Frees the local index (and filter) and makes the legacy range checks reject
 * every key. The descriptor, the node and hot_bits are deliberately kept --
 * they are held raw across async I/O and by the reinsertion queue.
 */
void slab_retire(struct slab *s);

/*
 * Close and unlink a retired slab's file once nobody is using it. Idempotent
 * and cheap for live slabs, which is why every last-reference dropper can
 * call it.
 */
void slab_release_if_idle(struct slab *s);
/* create_slab() with an explicit data size in pages (0 = cfg.max_file_size). */
struct slab *create_slab_sized(struct slab_context *ctx, uint64_t level,
                               uint64_t key, int rebuild, char *name,
                               size_t data_pages);

uint64_t slab_create_sequence(void);

/*
 * A fresh slab file (header written, id assigned), or (rebuild != 0) a
 * descriptor over an existing one -- then the caller sets seq from the header
 * and no id is consumed.
 */
struct slab *create_slab(struct slab_context *ctx, uint64_t level, uint64_t key,
                         int rebuild, char *name);

/* Bytes of the file that hold slots; the header page sits after them. */
static inline size_t slab_data_size(const struct slab *s) {
  return s->size_on_disk - PAGE_SIZE;
}

/*
 * Write the header from the slab's in-memory state (its node's pivot, level and
 * history children). One aligned page write: the durable commit of a split
 * (parent names its children) or of a prune (D names N). Returns 0 or -errno.
 */
int slab_write_header(struct slab *s);
/* Same, from explicit values; used before the slab has a center-tree node. */
int slab_write_header_raw(struct slab *s, uint64_t key, uint64_t level,
                          uint64_t left_id, uint64_t right_id);
int slab_read_header(int fd, size_t size_on_disk, struct slab_header *out);
/* ROOT file: the history root's id. write = temp + rename. */
int slab_root_write(uint64_t id);
int slab_root_read(uint64_t *id);
void slab_set_create_sequence(uint64_t next);

/* Test-only: _exit() at a chosen point of a split or a prune. */
enum slab_crash_point {
  CRASH_NONE = 0,
  CRASH_SPLIT_BEFORE_COMMIT,  /* children created, parent header not written */
  CRASH_PRUNE_AFTER_N_HEADER, /* N complete and named, D not yet updated */
  CRASH_PRUNE_AFTER_COMMIT,   /* D (or ROOT) names N, old files still there */
};
void slab_set_crash_point(enum slab_crash_point point);
void slab_maybe_crash(enum slab_crash_point point);

struct slab *resize_slab(struct slab *s);

void *read_item(struct slab *s, size_t idx);
void read_item_async(struct slab_callback *callback);
void scan_item_async(struct slab_callback *callback);
void add_item_async(struct slab_callback *callback);
/* First stage of an append; reinsertion drives it with its own completion. */
void add_item_async_cb1(struct slab_callback *callback);
void upsert_item_async(struct slab_callback *callback);
void remove_item_async(struct slab_callback *callback);
void remove_and_add_item_async(struct slab_callback *callback);

off_t item_page_num(struct slab *s, size_t idx);
void mark_page_hot(struct slab *s, size_t page_idx);
/* Same, returning whether the bit was already set. */
int mark_page_hot_test(struct slab *s, size_t page_idx);
/* Reinsertion on read: decide and issue a copy for the record just read. */
void reins_on_read_consider(struct slab_callback *callback,
                            struct item_metadata *meta);

/*
 * Recovery: read every slab header, rebuild both trees, delete the files that
 * are not reachable from ROOT. Returns the number of live slabs, exposed
 * through slab_recovered() for the per-slab index scans.
 */
int rebuild_slabs(int filenum, struct dirent **file_list);
size_t slab_recovered(struct slab ***out);
int create_root_slab(void);
#endif
