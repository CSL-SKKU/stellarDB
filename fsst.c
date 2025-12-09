#include "headers.h"
#include <errno.h>

static char *gc_buf;
int cur = 0;

void cb_gc(struct slab_callback *cb, void *item){
  free(cb->item);
  free(cb);
}

tree_entry_t *pick_garbage_node() { return tnt_traverse_use_seq(cur++); }

#define HOT_BATCH 128

static void *fsst_worker(void *pdata) {
  //tnt_rebalancing();

  while (1) {
    if (bgq_is_empty(GC)) {
      goto fsst_sleep;
    }

  if (!bgq_is_empty(GC)) {
    int updated = 0;
    for (int j = 0; j < HOT_BATCH; j++) {
      struct slab *s = (struct slab*)bgq_dequeue(GC);

      if (!s) goto fsst_sleep;

      size_t num_words = (((s->size_on_disk + PAGE_SIZE - 1) / PAGE_SIZE) + 63) / 64;
      struct slab_callback *cb;

      printf("GC: %lu\n", s->seq);
      size_t nread = pread(s->fd, gc_buf, s->size_on_disk, 0);
      if (nread < 0) perror("pread GC");

      for (size_t w = 0; w < num_words; w++) {
        uint64_t word = __atomic_load_n(&s->hot_bits[w], __ATOMIC_RELAXED);
        if (word == 0) {
          // 이 워드에 세트된 비트가 하나도 없으므로, 다음 워드로
          continue;
        }

        __atomic_store_n(&s->hot_bits[w], 0ULL, __ATOMIC_RELAXED);

        while (word != 0) {
          // (가) 워드 내 최하위 세트 비트(0~63)를 찾는다.
          int bit_pos = __builtin_ctzll(word);
          // (나) 슬랩 내 실제 페이지 인덱스로 변환
          size_t page_idx = w * 64 + (size_t)bit_pos;
          off_t offset = (off_t)page_idx * PAGE_SIZE;

          // (다) 콜백 호출
          // 페이지 안에 들어 있는 KV 개수
          size_t num_kvs = PAGE_SIZE / s->item_size;

          // 페이지 내 모든 KV를 순회
          for (size_t kv_i = 0; kv_i < num_kvs; kv_i++) {
          // (1) 콜백 구조체 할당
            cb = malloc(sizeof(*cb));
            if (!cb) {
              perror("slab_callback malloc 실패");
              exit(1);
            }

            // (2) cb 필드 초기화 (필요한 멤버만 예시로 채웠습니다)
            cb->cb       = cb_gc;        // 실제 실행할 콜백 함수 포인터
            cb->cb_cb    = NULL;
            cb->payload  = NULL;
            cb->slab     = s;
            cb->slab_idx  = page_idx * (PAGE_SIZE / cfg.kv_size) + kv_i;  
            // “페이지 안 몇 번째 KV”인지도 기록하고 싶으면
            cb->fsst_slab = s;
            cb->fsst_idx  = page_idx * (PAGE_SIZE / cfg.kv_size) + kv_i;  

            // (3) KV 크기만큼 메모리 할당한 뒤, 페이지에서 해당 KV를 복사
            cb->item = malloc(s->item_size);
            if (!cb->item) {
              perror("item malloc 실패");
              exit(1);
            }
            memcpy(
              cb->item,
	      &gc_buf[offset + kv_i * s->item_size],
              s->item_size
            );

	    index_entry_t *c = tnt_index_lookup_utree(s->subtree, cb->item);
	    if (c) {
		    unsigned char v;
		    const uint32_t p = c->slab_idx;
		    asm("btl %2, %1; setc %0" : "=qm"(v) : "m"(p), "Ir"(31));
		    if (v == 1) {
			    free(cb->item);
			    free(cb);
			    continue;
		    }
	    }
	    
            struct item_metadata *meta = (struct item_metadata *)cb->item;
            size_t size = sizeof(*meta) + meta->key_size + meta->value_size;
            char *item_key = &cb->item[sizeof(*meta)];
            uint64_t key = *(uint64_t *)item_key;
            if (size != s->item_size) {
              printf("key: %lu, pgoff: %lu, size: %lu\n", key, offset, size);
              printf("page_idx: %lu, kv_idx: %lu\n", page_idx, kv_i);
            }
            if (key > 100000000) {
              printf("key: %lu, pgoff: %lu, size: %lu\n", key, offset, size);
              printf("page_idx: %lu, kv_idx: %lu\n", page_idx, kv_i);
            }
            // (4) 비동기 업데이트 호출
    	    index_entry_t *e = NULL;
    	    struct tree_entry *tree = NULL;
            tree = centree_lookup_and_reserve(cb->item, 
                          &cb->slab_idx, &e);
	    cb->slab = tree->slab;

            //kv_update_async(cb);
            remove_and_add_item_async(cb);
	    //free(cb->item);
	    //free(cb);
	    updated++;
          }

          // (라) 처리한 비트를 워드에서 지운다.
          word &= ~(1ULL << bit_pos);
        }
	//if (updated >= 512) {
	//  sleep(1);
	//  updated = 0;
	//}
      }
      atomic_store_explicit(&s->queued, 0, memory_order_relaxed);
    }
  }
fsst_sleep:
    sleep(1);
  }
}

void fsst_worker_init(void) {
  pthread_t t;
  gc_buf = aligned_alloc(PAGE_SIZE, cfg.max_file_size);
  pthread_create(&t, NULL, fsst_worker, NULL);
}


#if 0
void check_and_remove_tree(struct slab_callback *cb, void *item) {
  struct slab *s = cb->fsst_slab;
  free(cb->item);
  free(cb);

  W_LOCK(&s->tree_lock);
  if (s->nb_items || (s->max == 0 && s->min == -1)) {
    // printf("fsst %lu %lu %lu\n", s->nb_items, s->min, s->max);
    W_UNLOCK(&s->tree_lock);
    return;
  }

  // TODO::JS Remove file
  printf("free %lu %lu %lu %lu\n", s->min, s->max, s->seq, s->nb_items);
  s->min = -1;
  s->max = 0;
  subtree_free(s->subtree);
  if (cfg.with_reins)
    free(s->hot_bits);

#if WITH_FILTER
  filter_delete(s->filter);
#endif
  s->subtree = NULL;
  /*s->centree_node = NULL;*/

  printf("RM %lu\n", s->seq);

  if (__sync_fetch_and_or(&s->update_ref, 0) == 0 && s->read_ref == 0) {
    char path[128], spath[128];
    int len;
    sprintf(path, "/proc/self/fd/%d", s->fd);
    if ((len = readlink(path, spath, 512)) < 0) die("READLINK\n");
    spath[len] = 0;
    close(s->fd);
    /*strncpy(path, spath, len);*/
    /*snprintf(path + len, 128 - len, "-%lu", s->key);*/
    truncate(spath, 0);
    /*rename(spath, path);*/
    //unlink(spath);
    printf("REMOVED FILE\n");
  }

  W_UNLOCK(&s->tree_lock);
}

void skip_or_invalidate_index_fsst(void *slab, uint64_t slab_idx) {
  struct slab_callback *cb;
  struct slab *s = (struct slab *)slab;
  size_t page_num;
  size_t page_idx;
  char *src;

  cb = malloc(sizeof(*cb));
  cb->cb = NULL;
  cb->cb_cb = check_and_remove_tree;
  cb->slab = s;
  cb->slab_idx = GET_SIDX(slab_idx);
  cb->fsst_slab = s;
  cb->fsst_idx = GET_SIDX(slab_idx);
  R_UNLOCK(&s->tree_lock);
  cb->item = malloc(cb->slab->item_size);

  page_num = item_page_num(cb->slab, cb->slab_idx);
  page_idx =
      (cb->slab_idx % (PAGE_SIZE / cb->slab->item_size)) * cb->slab->item_size;
  src = &vict_file_fsst[(page_num * PAGE_SIZE) + page_idx];
  memcpy(cb->item, src, cb->slab->item_size);
  kv_update_async(cb);
  R_LOCK(&s->tree_lock);
  return;
}


static char *vict_file_fsst = NULL;
vict_file_fsst = aligned_alloc(PAGE_SIZE, cfg.max_file_size);
if (!vict_file_fsst) die("FSST Static Buf Error\n");
if (!bgq_is_empty(FSST)) {
    doing = 1;
    for (size_t i = 0; i < NODE_BATCH; i++) {
      tree_entry_t *victim = (tree_entry_t *)bgq_dequeue(FSST);
      if (!victim) goto fsst_sleep;
      printf("FSST %lu\n", i);
      pread(victim->slab->fd, vict_file_fsst, victim->slab->size_on_disk, 0);

      R_LOCK(&victim->slab->tree_lock);
      subtree_forall_invalid(victim->slab->subtree, victim->slab, skip_or_invalidate_index_fsst);
      R_UNLOCK(&victim->slab->tree_lock);
    }
  }
#endif
