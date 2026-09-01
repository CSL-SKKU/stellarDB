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
  uint64_t cur_seq = -1;
  int flag = O_RDWR | O_DIRECT;

  // not rebuild
  if (!rebuild)
    flag = flag | O_CREAT;

  //size_t disk = slab_worker_id / (get_nb_workers() / get_nb_disks());
  cur_seq = __sync_add_and_fetch(&create_sequence, 1);
  if (rebuild)
    sprintf(path, "/scratch0/kvell/%s", name);
  else
    sprintf(path, PATH, 0LU, cur_seq, level, key);

  s->fd = open(path, flag, 0777);

  if (s->fd == -1) {
      perr("Cannot allocate slab %s", path);
  } 

  fstat(s->fd, &sb);
  s->size_on_disk = sb.st_size;
  if (!rebuild && s->size_on_disk < cfg.max_file_size) {
    fallocate(s->fd, 0, 0, cfg.max_file_size);
    s->size_on_disk = cfg.max_file_size;
  }


  size_t nb_items_per_page = PAGE_SIZE / cfg.kv_size;
  s->nb_max_items = s->size_on_disk / PAGE_SIZE * nb_items_per_page;
  s->item_size = cfg.kv_size;

  s->min = -1;
  s->key = key;
  s->seq = cur_seq;

  atomic_init(&s->full, 0);
  atomic_init(&s->last_item, 0);

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

  return s;
}

/*
 * Double the size of a slab on disk
 */
struct slab *resize_slab(struct slab *s) {
  if (s->size_on_disk < 10000000000LU) {
    s->size_on_disk *= 2;
    if (fallocate(s->fd, 0, 0, s->size_on_disk))
      perr("Cannot resize slab (item size %lu) new size %lu\n", s->item_size,
           s->size_on_disk);
    s->nb_max_items *= 2;
  } else {
    size_t nb_items_per_page = PAGE_SIZE / s->item_size;
    s->size_on_disk += 10000000000LU;
    if (fallocate(s->fd, 0, 0, s->size_on_disk))
      perr("Cannot resize slab (item size %lu) new size %lu\n", s->item_size,
           s->size_on_disk);
    s->nb_max_items = s->size_on_disk / PAGE_SIZE * nb_items_per_page;
  }
  return s;
}

int rebuild_slabs(int filenum, struct dirent **file_list) {
  int ret = 0;
  for (int i = 0; i < filenum; i++) {
    if (file_list[i]->d_type == DT_REG) {  // If it's a regular file
      char *filename = file_list[i]->d_name;
      uint64_t level, key, old_key, seq;
      int keynum;
      if ((keynum = sscanf(filename, "slab-%lu-%lu-%lu-%lu", &seq, &level, &old_key, &key)) >= 2) {
        struct slab *s;
        s = create_slab(NULL, level, old_key, 1, filename);
        // Process the slab file
        //process_slab_file(level, key);
#if WITH_FILTER
        tnt_subtree_add(s, tnt_subtree_create(), 
                  (void *)filter_create(200000), old_key);
#else
        tnt_subtree_add(s, tnt_subtree_create(), 
                  NULL, old_key);
#endif
        if (keynum == 4) {
          tnt_subtree_update_key(s->key, key);
          s->key = key;
        }
        ret++;
      } else {
        free(file_list[i]);
        file_list[i] = NULL;
      }
    }
    else {
      free(file_list[i]);
      file_list[i] = NULL;
    }
    //free(file_list[i]);
  }
  return ret;
}

static int root_exists() {
    const char *dir_path = "/scratch0/kvell";
    const char *prefix = "slab-1-0-";
    
    DIR *dir = opendir(dir_path);
    if (dir == NULL) {
        perror("Unable to open directory");
        return -1;  // 디렉터리를 열 수 없는 경우
    }
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        // 파일 이름이 "slab-0-"로 시작하는지 확인
        if (strncmp(entry->d_name, prefix, strlen(prefix)) == 0) {
            closedir(dir);
            return 1;  // 해당 파일을 찾은 경우
        }
    }
    
    closedir(dir);
    return 0;  // 해당 파일을 찾지 못한 경우
}

int create_root_slab() {
  struct slab *s;
  centree_init();

  // need to rebuild
  if (root_exists())
    return 0;

  s = create_slab(NULL, 0, 0, 0, NULL);

#if WITH_FILTER
  tnt_subtree_add(s, tnt_subtree_create(), filter_create(200000), 0);
#else
  tnt_subtree_add(s, tnt_subtree_create(), NULL, 0);
#endif
  return 1;
}

static void create_and_add_split_child(uint64_t level, uint64_t key) {
  struct slab *child = create_slab(NULL, level, key, 0, NULL);
  void *filter = NULL;

#if WITH_FILTER
  filter = filter_create(200000);
#endif
  tnt_subtree_add(child, tnt_subtree_create(), filter, key);
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

  tnt_subtree_update_key(s->key, new_key);

  W_LOCK(&s->tree_lock);
  s->key = new_key;
  W_UNLOCK(&s->tree_lock);
  char path[128], spath[128];
  int len;
  sprintf(path, "/proc/self/fd/%d", s->fd);
  if ((len = readlink(path, spath, sizeof(spath) - 1)) < 0) {
    tnt_split_phase_exit();
    perr("Can't find file\n");
  }
  spath[len] = 0;
  strncpy(path, spath, len);
  int suffix_len = snprintf(path + len, sizeof(path) - len, "-%lu", s->key);
  if (suffix_len < 0 || (size_t)suffix_len >= sizeof(path) - (size_t)len) {
    tnt_split_phase_exit();
    die("Cannot append pivot to slab %lu filename", s->seq);
  }
  if (rename(spath, path) != 0) {
    tnt_split_phase_exit();
    perr("Cannot rename slab %lu for pivot %lu", s->seq, new_key);
  }

  create_and_add_split_child(new_level, new_key - 1);
  run_split_midpoint_test_hook(s);
  create_and_add_split_child(new_level, new_key + 1);
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

  R_LOCK(&s->tree_lock);
  if (s->min == -1 && 
    !__sync_fetch_and_or(&s->update_ref, 0)
    && cur == 0) {
    char path[128], spath[128];
    int len;
    sprintf(path, "/proc/self/fd/%d", s->fd);
    if ((len = readlink(path, spath, 512)) < 0) {
	    printf("already removed\n");
	    goto skip;
    }
    spath[len] = 0;
    close(s->fd);
    truncate(spath, 0);
  }
skip:
  R_UNLOCK(&s->tree_lock);
  
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

void add_in_tree_for_upsert(struct slab_callback *cb, void *item) {
  struct slab *s = cb->slab;
  struct slab *old_s = cb->fsst_slab;
  //uint64_t old_idx = cb->fsst_idx;
  struct item_metadata *meta = (struct item_metadata *)item;
  char *item_key = &item[sizeof(*meta)];
  uint64_t key = *(uint64_t *)item_key;
  int removed = 0, alrdy = 0;
  uint64_t cur;
  index_entry_t *e = NULL;

  add_time_in_payload(cb, TIMING_STAGE_IO_COMPLETE);
  W_LOCK(&s->tree_lock);
    // CASE 2에서 여러 쓰레드가 여기 도달 가능.
    // 두 쓰레드들 중 가장 최신의 애가 먼저 lock 잡고 추가했다면
    // 그 다음 쓰레드는 스킵해야함.
  e = tnt_index_lookup_utree(s->subtree, item);
  if (e) {
    if (e->slab_idx > cb->slab_idx) {
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

  R_LOCK(&s->tree_lock);

  cur = __sync_fetch_and_add(&s->read_ref, 0);

  if (s->min == -1 && 
    __sync_fetch_and_or(&s->update_ref, 0) == 0 
    && cur == 0) {
    char path[128], spath[128];
    int len;
    sprintf(path, "/proc/self/fd/%d", s->fd);
    if ((len = readlink(path, spath, 512)) < 0) die("READLINK\n");
    spath[len] = 0;
    close(s->fd);
    truncate(spath, 0);
  }
  R_UNLOCK(&s->tree_lock);

  if (cb->cb_cb == add_in_tree_for_upsert) {
    free(cb->item);
    free(cb);
  }
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
