#include "headers.h"
#include "utils.h"
#include "items.h"
#include "slab.h"
#include "ioengine.h"
#include "pagecache.h"
#include "slabworker.h"
#include <errno.h>

extern int print;
extern int load;
extern uint64_t nb_totals;


static int create_sequence = 0;
static _Atomic(slab_split_test_hook_t) split_midpoint_test_hook;

void slab_set_split_midpoint_test_hook(slab_split_test_hook_t hook) {
  atomic_store_explicit(&split_midpoint_test_hook, hook,
                        memory_order_release);
}

static void run_split_midpoint_test_hook(struct slab *parent) {
  slab_split_test_hook_t hook = atomic_load_explicit(
      &split_midpoint_test_hook, memory_order_acquire);

  if (hook != NULL)
    hook(parent);
}

/*
 * Where is my item in the slab?
 */
off_t item_page_num(struct slab *s, size_t idx) {
  size_t items_per_page = PAGE_SIZE / s->item_size;
  return idx / items_per_page;
}
static off_t item_in_page_offset(struct slab *s, size_t idx) {
  size_t items_per_page = PAGE_SIZE / s->item_size;
  return (idx % items_per_page) * s->item_size;
}

void mark_page_hot(struct slab *s, size_t page_idx) {
    // 1) 몇 번째 워드(word)에 해당하는지, 몇 번째 비트(bit)에 해당하는지 계산
    size_t word_idx = page_idx / 64;
    size_t bit_pos  = page_idx % 64;

    // 2) 워드당 1ULL<<bit_pos를 OR하면 해당 비트가 켜짐
    uint64_t mask = (1ULL << bit_pos);
    if (word_idx > 255) {
	printf("s: %lu, page: %lu, word: %lu, bit: %lu\n", s->seq, page_idx, word_idx, bit_pos);
	die("WHAT");
    }

    // 3) __sync_fetch_and_or를 사용해서 “원자적으로” 비트 세팅
    __sync_fetch_and_or(&s->hot_bits[word_idx], mask);
    //   → 기존 hot_bits[word_idx]에 mask 비트를 OR하고, 
    //     연산 전(old value)을 반환. (반환 값이 필요 없으면 쓰지 않아도 됨)
}

/* How many slabs have ever been created; slab->seq is a stamp from this. */
uint64_t slab_create_sequence(void) {
  return (uint64_t)__sync_fetch_and_or(&create_sequence, 0);
}

/*
 * Create a slab: a file that only contains items of a given size.
 * @callback is a callback that will be called on all previously existing items
 * of the slab if it is restored from disk.
 */
struct slab *create_slab(struct slab_context *ctx, uint64_t level,
                         uint64_t key, int rebuild, char *name) {
  struct stat sb;
  char path[512];
  struct slab *s = calloc(1, sizeof(*s));
  uint64_t cur_seq = 0;
  int flag = O_RDWR | O_DIRECT;

  // not rebuild
  if (!rebuild)
    flag = flag | O_CREAT;

  /*
   * A fresh slab consumes an id and is named after it. A rebuilt one keeps
   * the id its header carries; the caller stores it in seq, so no id is
   * consumed here.
   */
  if (rebuild) {
    sprintf(path, "/scratch0/kvell/%s", name);
  } else {
    cur_seq = __sync_add_and_fetch(&create_sequence, 1);
    sprintf(path, PATH, 0LU, cur_seq);
  }

  s->fd = open(path, flag, 0777);

  if (s->fd == -1) {
      perr("Cannot allocate slab %s", path);
  } 

  /*
   * cfg.max_file_size is the data size; the header page comes on top of it,
   * so the slot count a configuration implies is unchanged by the header.
   */
  fstat(s->fd, &sb);
  s->size_on_disk = sb.st_size;
  if (!rebuild && s->size_on_disk < cfg.max_file_size + PAGE_SIZE) {
    fallocate(s->fd, 0, 0, cfg.max_file_size + PAGE_SIZE);
    s->size_on_disk = cfg.max_file_size + PAGE_SIZE;
  }
  if (s->size_on_disk < 2 * PAGE_SIZE || s->size_on_disk % PAGE_SIZE != 0)
    die("Slab %s has size %lu; need at least a data page and the header page\n",
        path, s->size_on_disk);

  /* The last page is the header, not slots. */
  size_t nb_items_per_page = PAGE_SIZE / cfg.kv_size;
  s->nb_max_items = slab_data_size(s) / PAGE_SIZE * nb_items_per_page;
  s->item_size = cfg.kv_size;

  s->min = -1;
  s->key = key;
  s->seq = cur_seq;

  atomic_init(&s->full, 0);
  atomic_init(&s->last_item, 0);
  atomic_init(&s->released, 0);

  if (cfg.with_reins) {
    atomic_init(&s->queued, 0);
    atomic_init(&s->upward_maxlen, 0);
    atomic_init(&s->cur_ep, 1);
    atomic_init(&s->epcnt, 0);
    atomic_init(&s->prev_epcnt, 0);
    size_t num_words = (((s->size_on_disk + PAGE_SIZE - 1) / PAGE_SIZE) + 63) / 64;
    s->hot_bits = calloc(num_words, sizeof(uint64_t));
  }

   if (load)
    s->batched_callbacks = calloc(NUM_LOAD_BATCH, sizeof(struct slab_callback *));
   else
    s->batched_callbacks = NULL;
   s->nb_batched = 0;

  INIT_LOCK(&s->tree_lock, NULL);

  /*
   * A fresh slab is on disk with a valid header before anything can refer to
   * it. It has no children yet; the header is rewritten when it splits.
   */
  if (!rebuild && slab_write_header_raw(s, key, level, 0, 0) != 0)
    perr("Cannot write the header of slab %s", path);

  return s;
}

/* ------------------------------------------------------------------ */
/* Header page, ROOT file                                              */

static __thread char *header_page;

static char *header_page_buffer(void) {
  if (header_page == NULL)
    header_page = aligned_alloc(PAGE_SIZE, PAGE_SIZE);
  return header_page;
}

int slab_write_header_raw(struct slab *s, uint64_t key, uint64_t level,
                          uint64_t left_id, uint64_t right_id) {
  char *page = header_page_buffer();
  struct slab_header *h = (struct slab_header *)page;
  ssize_t written;

  if (page == NULL)
    return -ENOMEM;
  memset(page, 0, PAGE_SIZE);
  h->magic = SLAB_HEADER_MAGIC;
  h->id = s->seq;
  h->key = key;
  h->level = level;
  h->lu_child[0] = left_id;
  h->lu_child[1] = right_id;
  written = pwrite(s->fd, page, PAGE_SIZE, (off_t)slab_data_size(s));
  return written == (ssize_t)PAGE_SIZE ? 0 : -EIO;
}

int slab_write_header(struct slab *s) {
  centree_node n = (centree_node)s->centree_node;
  uint64_t child[2] = {0, 0};

  if (n == NULL)
    return -EINVAL;
  for (int i = 0; i < 2; i++)
    if (n->lu_child[i] != NULL)
      child[i] = n->lu_child[i]->value.slab->seq;
  return slab_write_header_raw(
      s, centree_pivot_load(n),
      atomic_load_explicit(&n->value.level, memory_order_acquire), child[0],
      child[1]);
}

int slab_read_header(int fd, size_t size_on_disk, struct slab_header *out) {
  char *page = header_page_buffer();

  if (page == NULL)
    return -ENOMEM;
  if (size_on_disk < 2 * PAGE_SIZE || size_on_disk % PAGE_SIZE != 0)
    return -EINVAL;
  if (pread(fd, page, PAGE_SIZE, (off_t)(size_on_disk - PAGE_SIZE)) !=
      (ssize_t)PAGE_SIZE)
    return -EIO;
  memcpy(out, page, sizeof(*out));
  return out->magic == SLAB_HEADER_MAGIC ? 0 : -EBADMSG;
}

/*
 * Every operation on the database directory goes through a descriptor opened
 * with open(): that is the one call tests interpose to redirect
 * /scratch0/kvell, and rename/unlink on absolute paths would escape it.
 */
static int kvell_dirfd(void) {
  char path[128];

  snprintf(path, sizeof(path), "/scratch%lu/kvell", 0LU);
  return open(path, O_RDONLY | O_DIRECTORY);
}

int slab_root_write(uint64_t id) {
  char text[32];
  int dirfd, fd, len, error = 0;

  dirfd = kvell_dirfd();
  if (dirfd < 0)
    return -errno;
  len = snprintf(text, sizeof(text), "%lu\n", id);
  fd = openat(dirfd, "ROOT.tmp", O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    error = -errno;
    goto out;
  }
  if (write(fd, text, len) != len || fsync(fd) != 0) {
    close(fd);
    error = -EIO;
    goto out;
  }
  close(fd);
  /* rename is atomic: readers see the old id or the new one, never neither. */
  if (renameat(dirfd, "ROOT.tmp", dirfd, "ROOT") != 0)
    error = -errno;
out:
  close(dirfd);
  return error;
}

int slab_root_read(uint64_t *id) {
  char text[32];
  int dirfd, fd, len;

  dirfd = kvell_dirfd();
  if (dirfd < 0)
    return -errno;
  fd = openat(dirfd, "ROOT", O_RDONLY);
  close(dirfd);
  if (fd < 0)
    return -errno;
  len = read(fd, text, sizeof(text) - 1);
  close(fd);
  if (len <= 0)
    return -EIO;
  text[len] = 0;
  *id = strtoull(text, NULL, 10);
  return *id == 0 ? -EBADMSG : 0;
}

void slab_set_create_sequence(uint64_t next) {
  __sync_lock_test_and_set(&create_sequence, (int)next);
}

static enum slab_crash_point crash_point;

void slab_set_crash_point(enum slab_crash_point point) { crash_point = point; }

void slab_maybe_crash(enum slab_crash_point point) {
  if (crash_point == point) {
    fprintf(stderr, "test crash point %d reached, exiting hard\n", point);
    _exit(42);
  }
}

/*
 * Slabs are never resized: the header sits in the last page, reinsertion's
 * buffer is sized once, and nb_max_items is baked into the freeze/split CAS.
 */
struct slab *resize_slab(struct slab *s) {
  die("Slab %lu: resizing is not supported\n", s->seq);
  return s;
}

static struct slab **recovered;
static size_t nb_recovered;

size_t slab_recovered(struct slab ***out) {
  *out = recovered;
  return nb_recovered;
}

static void discard_recovered(struct slab *s, const char *name,
                              const char *why) {
  int dirfd;

  fprintf(stderr, "Recovery: deleting %s (%s)\n", name, why);
  if (s != NULL) {
    close(s->fd);
    if (s->subtree != NULL)
      subtree_free(s->subtree);
    free(s->centree_node); /* never published, so it may be freed */
    free(s->hot_bits);
    free(s);
  }
  dirfd = kvell_dirfd();
  if (dirfd >= 0) {
    unlinkat(dirfd, name, 0);
    close(dirfd);
  }
}

int rebuild_slabs(int filenum, struct dirent **file_list) {
  struct slab_header *hdrs = calloc(filenum > 0 ? filenum : 1, sizeof(*hdrs));
  char **names = calloc(filenum > 0 ? filenum : 1, sizeof(*names));
  unsigned char *reachable;
  uint64_t root_id, max_id = 0;
  size_t live = 0, kept = 0;

  recovered = calloc(filenum > 0 ? filenum : 1, sizeof(*recovered));
  if (hdrs == NULL || names == NULL || recovered == NULL)
    die("Recovery: out of memory\n");

  /* 1. One descriptor per file with a valid header; drop everything else. */
  for (int i = 0; i < filenum; i++) {
    char *name = file_list[i]->d_name;
    struct slab_header hdr;
    struct stat sb;
    char path[512];
    int fd, dup = 0;

    if (file_list[i]->d_type != DT_REG || strncmp(name, "slab-", 5) != 0)
      continue;
    snprintf(path, sizeof(path), "/scratch0/kvell/%s", name);
    fd = open(path, O_RDONLY); /* absolute: open() is the redirected call */
    if (fd < 0 || fstat(fd, &sb) != 0 ||
        slab_read_header(fd, (size_t)sb.st_size, &hdr) != 0) {
      if (fd >= 0)
        close(fd);
      discard_recovered(NULL, name, "no valid header");
      continue;
    }
    close(fd);
    for (size_t j = 0; j < live && !dup; j++)
      dup = hdrs[j].id == hdr.id;
    if (dup) {
      discard_recovered(NULL, name, "duplicate id");
      continue;
    }

    recovered[live] = create_slab(NULL, hdr.level, hdr.key, 1, name);
    recovered[live]->seq = hdr.id;
    recovered[live]->subtree = tnt_subtree_create();
    subtree_set_slab(recovered[live]->subtree, recovered[live]);
#if WITH_FILTER
    recovered[live]->filter = filter_create(200000);
#endif
    hdrs[live] = hdr;
    names[live] = name;
    if (hdr.id > max_id)
      max_id = hdr.id;
    live++;
  }
  if (live == 0)
    die("Recovery: ROOT exists but no slab file has a valid header\n");
  if (slab_root_read(&root_id) != 0)
    die("Recovery: cannot read ROOT\n");

  /* 2. Both trees from the headers. */
  reachable = calloc(live, sizeof(*reachable));
  tnt_recover_tree(recovered, hdrs, live, root_id, reachable);

  /* 3. Garbage: leftovers of an interrupted split or prune. */
  for (size_t i = 0; i < live; i++) {
    if (reachable[i])
      recovered[kept++] = recovered[i];
    else
      discard_recovered(recovered[i], names[i], "not reachable from ROOT");
  }
  nb_recovered = kept;
  slab_set_create_sequence(max_id);

  free(reachable);
  free(hdrs);
  free(names);
  return (int)kept;
}

/* ROOT present: recover. Slab files without ROOT: unrecoverable layout. */
static int root_exists(void) {
  uint64_t id;
  DIR *dir;
  struct dirent *entry;

  if (slab_root_read(&id) == 0)
    return 1;
  dir = opendir("/scratch0/kvell");
  if (dir == NULL) {
    perror("Unable to open directory");
    return -1;
  }
  while ((entry = readdir(dir)) != NULL) {
    if (strncmp(entry->d_name, "slab-", 5) == 0) {
      closedir(dir);
      die("Slab files present in /scratch0/kvell but no ROOT file: the "
          "layout predates headers or ROOT was lost; not recoverable\n");
    }
  }
  closedir(dir);
  return 0;
}

int create_root_slab() {
  struct slab *s;
  centree_init();

  // need to rebuild
  if (root_exists())
    return 0;

  s = create_slab(NULL, 0, 0, 0, NULL);
  /* The first slab is the history root until a prune replaces it. */
  if (slab_root_write(s->seq) != 0)
    perr("Cannot write the ROOT file");

#if WITH_FILTER
  tnt_subtree_add(s, tnt_subtree_create(), filter_create(200000), 0);
#else
  tnt_subtree_add(s, tnt_subtree_create(), NULL, 0);
#endif
  return 1;
}

static void add_split_child(struct slab *child, uint64_t key) {
  void *filter = NULL;

#if WITH_FILTER
  filter = filter_create(200000);
#endif
  tnt_subtree_add_split(child, tnt_subtree_create(), filter, key);
}

struct slab *close_and_create_slab(struct slab *s) {
  uint64_t new_key;
  uint64_t new_level;
  uint64_t range_min;
  uint64_t range_max;

  tnt_split_phase_enter();
  R_LOCK(&s->tree_lock);
  range_min = __atomic_load_n(&s->min, __ATOMIC_ACQUIRE);
  range_max = __atomic_load_n(&s->max, __ATOMIC_ACQUIRE);
  if (range_min == (uint64_t)-1 || range_min > range_max) {
    R_UNLOCK(&s->tree_lock);
    tnt_split_phase_exit();
    die("Cannot split slab %lu with invalid range [%lu, %lu]", s->seq,
        range_min, range_max);
  }
  new_key = range_min + (range_max - range_min) / 2;
  if (new_key == 0 || new_key == UINT64_MAX) {
    R_UNLOCK(&s->tree_lock);
    tnt_split_phase_exit();
    die("Cannot split slab %lu with overflowing pivot %lu", s->seq, new_key);
  }
  new_level = tnt_get_centree_level(s->centree_node) + 1;
  R_UNLOCK(&s->tree_lock);

  centree_pivot_store(s->centree_node, new_key);

  W_LOCK(&s->tree_lock);
  s->key = new_key;
  W_UNLOCK(&s->tree_lock);

  /*
   * Durable order: both children exist on disk (headers, no data) before the
   * parent's header names them and records the pivot -- that write is the
   * commit -- and only then are the children published to readers and
   * writers. A crash before the commit leaves two unreachable, empty files
   * that recovery deletes; a crash after it leaves a valid split.
   */
  struct slab *left = create_slab(NULL, new_level, new_key - 1, 0, NULL);
  struct slab *right = create_slab(NULL, new_level, new_key + 1, 0, NULL);

  slab_maybe_crash(CRASH_SPLIT_BEFORE_COMMIT);
  if (slab_write_header_raw(s, new_key,
                            tnt_get_centree_level(s->centree_node), left->seq,
                            right->seq) != 0) {
    tnt_split_phase_exit();
    perr("Cannot commit the split of slab %lu", s->seq);
  }

  add_split_child(left, new_key - 1);
  run_split_midpoint_test_hook(s);
  add_split_child(right, new_key + 1);
  wakeup_subtree_get(s->centree_node);
  tnt_split_phase_exit();

  return s;
}

/*
 * Synchronous read item
 */
void *read_item(struct slab *s, size_t idx) {
  size_t page_num = item_page_num(s, idx);
  char *disk_data = safe_pread(s->fd, page_num * PAGE_SIZE);
  return &disk_data[item_in_page_offset(s, idx)];
}

/*
 * Asynchronous read
 * - read_item_async creates a callback for the ioengine and queues the io
 * request
 * - read_item_async_cb is called when io is completed (might be synchronous if
 * page is cached) If nothing happen it is because do_io is not called.
 */
void read_item_async_cb(struct slab_callback *callback) {
  char *disk_page = callback->lru_entry->page;
  off_t in_page_offset =
      item_in_page_offset(callback->slab, callback->slab_idx);
  struct slab *s = callback->slab;
  uint64_t cur;

  cur = __sync_sub_and_fetch(&s->read_ref, 1);
  (void)cur;
  slab_release_if_idle(s);
  
  add_time_in_payload(callback, TIMING_STAGE_IO_COMPLETE);
  struct item_metadata *meta =
      (struct item_metadata *)&disk_page[in_page_offset];
  if (item_is_legacy(meta))
    die("Read encountered legacy item metadata\n");
  if (callback->cb)
    callback->cb(callback, item_is_tombstone(meta) ? NULL : meta);
}

void read_item_async(struct slab_callback *callback) {
  callback->io_cb = read_item_async_cb;
  read_page_async(callback);
}

void scan_item_async(struct slab_callback *callback) {
  callback->io_cb = read_item_async_cb;
}

/*
 * Asynchronous upsert item:
 * - First read the page where the item is staying
 * - Once the page is in page cache, write it
 * - Then send the order to flush it.
 */
void upsert_item_async_cb2(struct slab_callback *callback) {
  char *disk_page = callback->lru_entry->page;
  off_t in_page_offset =
      item_in_page_offset(callback->slab, callback->slab_idx);
  unsigned char cbcb = callback->cb_cb ? 1 : 0;

  struct item_metadata *meta = (struct item_metadata *)callback->item;
  char *item_key = &callback->item[sizeof(*meta)];
  uint64_t key = *(uint64_t *)item_key;

  void *item = &disk_page[in_page_offset];
  struct item_metadata *meta2 = (struct item_metadata *)item;
  char *item_key2 = &item[sizeof(*meta2)];
  uint64_t key2 = *(uint64_t *)item_key2;
  if (key != key2)
    die("DIFF key1: %lu, key2: %lu, key/idx: %lu/%lu\n", key, key2,
        callback->slab->seq, callback->slab_idx);

  if(callback->cb != add_in_tree_for_upsert
    && callback->cb != add_in_tree_for_reinsertion
    && callback->cb != add_in_tree)
    __sync_fetch_and_sub(&callback->slab->update_ref, 1);

  //if (load == 0)
  //  printf("A,%lu,%lu,%lu\n", key, callback->slab->seq, callback->slab_idx/4096);

  if (callback->cb) callback->cb(callback, &disk_page[in_page_offset]);
  if (cbcb) callback->cb_cb(callback, &disk_page[in_page_offset]);
}

void upsert_item_async_cb1(struct slab_callback *callback) {
  char *disk_page = callback->lru_entry->page;

  struct slab *s = callback->slab;
  size_t idx = callback->slab_idx;
  void *item = callback->item;
  struct item_metadata *meta = item;
  off_t offset_in_page = item_in_page_offset(s, idx);
  struct slab_context *ctx = callback->ctx;

  meta->rdt = get_rdt(ctx);
  if (item_is_legacy(meta))
    die("Attempted to write legacy item metadata\n");
  if (item_stored_size(meta) > s->item_size)
    die("Trying to write an item that is too big for its slab\n");
  else
    memcpy(&disk_page[offset_in_page], item, item_stored_size(meta));

#if DEBUG
  /*
    char *item_key = &callback->item[sizeof(*meta)];
    uint64_t key = *(uint64_t*)item_key;
    off_t in_page_offset = item_in_page_offset(callback->slab,
    callback->slab_idx);

    void *item2 = &disk_page[in_page_offset];
    struct item_metadata *meta2 = (struct item_metadata *)item;
    char *item_key2 = &item2[sizeof(*meta)];
    uint64_t key2 = *(uint64_t*)item_key2;
    if (key != key2)
     printf("DIFFFF key1: %lu, key2: %lu, %lu/%lu\n", key, key2, s->seq, idx);
     */
#endif

  callback->io_cb = upsert_item_async_cb2;
  write_page_async(callback);
}

void upsert_item_async(struct slab_callback *callback) {
  callback->io_cb = upsert_item_async_cb1;
  read_page_async(callback);
}

/*
 * Add an item is just like updating, but we need to find a suitable page first!
 * get_free_item_idx returns lru_entry == NULL if no page with empty spot exist.
 * If a page with an empty spot exists, we have to scan it to find a suitable
 * spot.
 */
void add_item_async_cb1(struct slab_callback *callback) {
  struct slab *s = callback->slab;
  struct lru *lru_entry = callback->lru_entry;

  R_LOCK(&s->tree_lock);
  if (lru_entry == NULL) {  // no free page, append
    if (callback->slab_idx + 1 == s->nb_max_items &&
        callback->slab_idx != callback->fsst_idx) {
      assert(atomic_load_explicit(&s->full, memory_order_acquire) == 1);
      assert(atomic_load_explicit(&s->last_item, 
	     memory_order_acquire) == s->nb_max_items);
      // s->imm = 1;
      R_UNLOCK(&s->tree_lock);
      s = close_and_create_slab(s);
      R_LOCK(&s->tree_lock);
    }
  } else {  // reuse a free spot. Don't forget to add the linked tombstone in
            // the freelist.
    die("LRU entry != NULL\n");
  }

  R_UNLOCK(&s->tree_lock);
  if (load) {
    W_LOCK(&s->tree_lock);
    s->batched_callbacks[s->nb_batched++] = callback;
    if (s->nb_batched == NUM_LOAD_BATCH) {
      struct slab_callback *batched_callbacks_copy[NUM_LOAD_BATCH];
      memcpy(batched_callbacks_copy, s->batched_callbacks, NUM_LOAD_BATCH * sizeof(struct slab_callback *));
      s->nb_batched = 0;
      if (atomic_load_explicit(&s->full, memory_order_acquire) == 1)
        free(s->batched_callbacks);
      W_UNLOCK(&s->tree_lock);

      for (int i=0; i<NUM_LOAD_BATCH; i++) {
        struct slab_callback *cb = batched_callbacks_copy[i];

        if (cb == NULL)
          continue;

        kv_add_async_no_lookup(cb, cb->slab, cb->slab_idx);
      }
    } else {
      W_UNLOCK(&s->tree_lock);
    }
    return;
  }

  kv_add_async_no_lookup(callback, callback->slab, callback->slab_idx);
}

void add_item_async(struct slab_callback *callback) {
  if (callback->cb != add_in_tree) {
    if (callback->cb == NULL) {
      printf("testestest WHAT?\n");
      callback->cb = add_in_tree;
    } else {
      callback->cb_cb = callback->cb;  // computes_stat
      callback->cb = add_in_tree;
    }
  }
  callback->io_cb = add_item_async_cb1;
  callback->lru_entry = NULL;
  callback->io_cb(callback);
}

/*
 * Freeze a slab: no writer will ever reserve another slot in it, and every
 * write to a slot reserved before the freeze has completed.
 *
 * Returns the number of slots writers ever reserved, or
 *   -EBUSY   a writer already took the final slot, so it owns this slab's
 *            split; the slab is about to grow children.
 *   -ENOSPC  those slots do not fit in the caller's budget.
 * Both failures leave the slab untouched.
 *
 * The CAS to nb_max_items is what makes freeze and split mutually exclusive:
 * reserve_slot() hands out the final slot with the same CAS on last_item, so
 * whichever side wins it decides. After the CAS, reserve_slot() always takes
 * its "old >= nb_max_items" exit.
 *
 * full is then published with an exchange, i.e. a full barrier, and the
 * caller waits for update_ref to drain. Writers increment update_ref before
 * they read full (centree_lookup_and_reserve/tnt_subtree_get), so the pair is
 * a Dekker handshake: either the drain sees the writer's reference and waits
 * for it, or the writer sees full and gives up on this slab. Draining also
 * waits for in-flight page writes to slots reserved before the freeze.
 */
long slab_freeze(struct slab *s, size_t budget) {
  for (;;) {
    size_t old = atomic_load_explicit(&s->last_item, memory_order_acquire);

    if (old >= s->nb_max_items)
      return -EBUSY;
    if (old > budget)
      return -ENOSPC;
    if (atomic_compare_exchange_strong_explicit(
            &s->last_item, &old, s->nb_max_items, memory_order_acq_rel,
            memory_order_acquire)) {
      atomic_exchange_explicit(&s->full, 1, memory_order_seq_cst);
      while (__sync_fetch_and_or(&s->update_ref, 0) != 0)
        NOP10();
      return (long)old;
    }
  }
}

void slab_retire(struct slab *s) {
  /*
   * The node is marked removed before this runs (the routing splice does it),
   * and every reader of s->subtree checks that flag under tree_lock, so the
   * local index can go here (I-5).
   *
   * min/max are reset because the older range checks in the writer descent and
   * in add_in_tree_for_upsert() consult them without looking at `removed`:
   * min = -1, max = 0 rejects every key. It is not how retirement is
   * detected -- an empty live slab looks the same (I-6).
   */
  W_LOCK(&s->tree_lock);
  s->min = (uint64_t)-1;
  s->max = 0;
  if (s->subtree != NULL) {
    subtree_free(s->subtree);
    s->subtree = NULL;
  }
#if WITH_FILTER
  if (s->filter != NULL) {
    filter_delete(s->filter);
    s->filter = NULL;
  }
#endif
  W_UNLOCK(&s->tree_lock);

  slab_release_if_idle(s);
}

void slab_release_if_idle(struct slab *s) {
  centree_node node = (centree_node)s->centree_node;
  char proc[64], path[512];
  int expected = 0;
  int len;

  if (node == NULL ||
      !atomic_load_explicit(&node->removed, memory_order_acquire))
    return;
  /*
   * No new reference can appear on a retired slab: readers and the
   * reinsertion worker check `removed` under tree_lock before taking one, and
   * the writer descent's transient update reference never touches the file or
   * the local index. So once both counts are zero they stay zero, and the
   * last dropper gets here.
   */
  if (__sync_fetch_and_or(&s->update_ref, 0) != 0 ||
      __sync_fetch_and_or(&s->read_ref, 0) != 0)
    return;
  if (!atomic_compare_exchange_strong(&s->released, &expected, 1))
    return;

  snprintf(proc, sizeof(proc), "/proc/self/fd/%d", s->fd);
  len = readlink(proc, path, sizeof(path) - 1);
  /* fd = -1 makes a straggling read fail instead of hitting a reused fd. */
  close(s->fd);
  s->fd = -1;
  if (len > 0) {
    path[len] = 0;
    unlink(path);
  }
}

void add_in_tree_for_upsert(struct slab_callback *cb, void *item) {
  struct slab *s = cb->slab;
  struct slab *old_s = cb->fsst_slab;
  //uint64_t old_idx = cb->fsst_idx;
  struct item_metadata *meta = (struct item_metadata *)item;
  char *item_key = &item[sizeof(*meta)];
  uint64_t key = *(uint64_t *)item_key;
  int removed = 0, alrdy = 0;
  index_entry_t *e = NULL;

  add_time_in_payload(cb, TIMING_STAGE_IO_COMPLETE);
  W_LOCK(&s->tree_lock);
    // CASE 2에서 여러 쓰레드가 여기 도달 가능.
    // 두 쓰레드들 중 가장 최신의 애가 먼저 lock 잡고 추가했다면
    // 그 다음 쓰레드는 스킵해야함.
  e = tnt_index_lookup_utree(s->subtree, item);
  if (e) {
    /*
     * Two client writes of one key in one slab: the higher slot wins (there is
     * no request ordering, so either is acceptable). A shy entry -- put there
     * by reinsertion, a copy of an older value -- never wins against a client
     * write, whatever its slot: that is the rule that stops a copy-forward
     * from dropping or shadowing a real write.
     */
    if (!sidx_is_shy(e->slab_idx) && GET_SIDX(e->slab_idx) > cb->slab_idx) {
      __sync_fetch_and_sub(&s->nb_items, 1);
      W_UNLOCK(&s->tree_lock);
      goto skip;
    }
    alrdy = tnt_index_delete(s->subtree, item);
  }

  tnt_index_add(cb, item);

  if (!alrdy) {
    __sync_fetch_and_add(&nb_totals, 1);
#if WITH_FILTER
    if (filter_add((filter_t *)s->filter, (unsigned char *)&key) == 0) {
      printf("Fail adding to filter %p %lu seq/idx %lu/%lu fsst %lu/%lu\n",
             s->filter, key, cb->slab->seq, cb->slab_idx, cb->fsst_slab->seq,
             cb->fsst_idx);
    } else if (!filter_contain(s->filter, (unsigned char *)&key)) {
      printf("FIFIFIFIF UPUP\n");
    }
#endif
  } else {
    __sync_fetch_and_sub(&s->nb_items, 1);
    W_UNLOCK(&s->tree_lock);
    goto skip;
  }

  slab_widen_range(s, key);

  W_UNLOCK(&s->tree_lock);

  add_time_in_payload(cb, TIMING_STAGE_NEW_INDEX_PUBLISHED);

  R_LOCK(&old_s->tree_lock);
  if (old_s->min == -1)
    removed = 1;
  else {
    removed = tnt_index_invalid_utree(old_s->subtree, cb->item);
    if (removed)
      __sync_fetch_and_sub(&old_s->nb_items, 1);
  }
  R_UNLOCK(&old_s->tree_lock);

  add_time_in_payload(cb, TIMING_STAGE_OLD_INDEX_INVALIDATED);


  if (!removed) {
      printf("UPCASE: Edge\n");
  }

skip:
  __sync_fetch_and_sub(&s->update_ref, 1);
  slab_release_if_idle(s);

  if (cb->cb_cb == add_in_tree_for_upsert) {
    free(cb->item);
    free(cb);
  }
}

void add_in_tree_for_reinsertion(struct slab_callback *cb, void *item) {
  struct slab *s = cb->slab;
  struct slab *old_s = cb->fsst_slab;
  struct item_metadata *meta = (struct item_metadata *)item;
  char *item_key = &item[sizeof(*meta)];
  uint64_t key = *(uint64_t *)item_key;
  index_entry_t *e;
  int source_ok = 0, removed = 0;

  add_time_in_payload(cb, TIMING_STAGE_IO_COMPLETE);

  /*
   * The copy is only worth publishing if the record it was taken from is still
   * the authoritative one. A client write that completed since invalidated
   * it; a prune that retired the source dropped its index. Either way the
   * slot just reserved is abandoned: it holds a shy record on disk, so
   * recovery knows to let any client record for the key beat it.
   */
  R_LOCK(&old_s->tree_lock);
  if (old_s->min != (uint64_t)-1 && old_s->subtree != NULL) {
    e = tnt_index_lookup_utree(old_s->subtree, item);
    source_ok = e != NULL && !sidx_is_invalid(e->slab_idx) &&
                GET_SIDX(e->slab_idx) == cb->fsst_idx;
  }
  R_UNLOCK(&old_s->tree_lock);
  if (!source_ok) {
    __sync_fetch_and_sub(&s->nb_items, 1);
    goto skip;
  }

  W_LOCK(&s->tree_lock);
  /*
   * Any entry for the key in the destination -- a client's, or another
   * copy's -- means somebody got here first. A client write that lands later
   * replaces a shy entry regardless of slot order (see add_in_tree_for_upsert),
   * so publishing here is safe against writes still in flight.
   */
  if (tnt_index_lookup_utree(s->subtree, item) != NULL) {
    __sync_fetch_and_sub(&s->nb_items, 1);
    W_UNLOCK(&s->tree_lock);
    goto skip;
  }
  tnt_index_add_shy(cb, item);
  __sync_fetch_and_add(&nb_totals, 1);
  slab_widen_range(s, key);
  W_UNLOCK(&s->tree_lock);

  add_time_in_payload(cb, TIMING_STAGE_NEW_INDEX_PUBLISHED);

  /* Destination published: the source copy is now the older one. */
  R_LOCK(&old_s->tree_lock);
  if (old_s->min == (uint64_t)-1 || old_s->subtree == NULL)
    removed = 1;
  else {
    removed = tnt_index_invalid_utree(old_s->subtree, item);
    if (removed)
      __sync_fetch_and_sub(&old_s->nb_items, 1);
  }
  R_UNLOCK(&old_s->tree_lock);
  (void)removed;

skip:
  __sync_fetch_and_sub(&s->update_ref, 1);
  slab_release_if_idle(s);
}

void remove_and_add_item_async(struct slab_callback *callback) {
  callback->io_cb = add_item_async_cb1;
  if (callback->slab_idx != -1) {
    if (!callback->cb){  // making fsst by using cb_cb
      callback->cb = add_in_tree_for_upsert;
    }
    else {
      callback->cb_cb = callback->cb;  // computes_stat
      callback->cb = add_in_tree_for_upsert;
    }
  } else {
    // FSST과정에서는 불가능 해야 하는(호출되면 안되는) 상황
    //  In-place update라 트리 수정이 불필요.
    //  그냥 원래 위치에 데이터만 업데이트
    callback->slab_idx = callback->fsst_idx;
  }
  callback->lru_entry = NULL;
  callback->io_cb(callback);
}
