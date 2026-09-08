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
    for (int j = 0; j < HOT_BATCH; j++) {
      struct slab *s = (struct slab*)bgq_dequeue(GC);

      if (!s) goto fsst_sleep;

      /* Slot pages only: the file's last page is the header. */
      size_t num_words = ((slab_data_size(s) / PAGE_SIZE) + 63) / 64;
      struct slab_callback *cb;
      centree_node node = (centree_node)s->centree_node;

      /*
       * Pin the source for the whole batch: the read reference keeps its file
       * and subtree alive while this pass reads them. A slab that is already
       * retired is dropped -- its records live in the replacement node now.
       * So is a slab that is not full: its bytes can still change under the
       * buffer read below, and its records are already at the leaf anyway.
       */
      R_LOCK(&s->tree_lock);
      if (atomic_load_explicit(&node->removed, memory_order_acquire) ||
          atomic_load_explicit(&s->superseded, memory_order_acquire) ||
          !atomic_load_explicit(&s->full, memory_order_acquire)) {
        R_UNLOCK(&s->tree_lock);
        atomic_store_explicit(&s->queued, 0, memory_order_relaxed);
        continue;
      }
      __sync_fetch_and_add(&s->read_ref, 1);
      R_UNLOCK(&s->tree_lock);

      size_t nread = pread(s->fd, gc_buf, slab_data_size(s), 0);
      if (nread < 0) perror("pread GC");

      for (size_t w = 0; w < num_words; w++) {
        uint64_t word = __atomic_load_n(&s->hot_bits[w], __ATOMIC_RELAXED);
        if (word == 0) {
          // 이 워드에 세트된 비트가 하나도 없으므로, 다음 워드로
          continue;
        }

        __atomic_store_n(&s->hot_bits[w], 0ULL, __ATOMIC_RELAXED);

        while (word != 0) {
          struct tree_entry *tree;
          index_entry_t *c, *e;
          struct item_metadata *meta;
          uint32_t src_idx;

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
            size_t slot_idx = page_idx * num_kvs + kv_i;
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
            cb->slab_idx = slot_idx;
            cb->fsst_slab = NULL;
            cb->fsst_idx = -1;

            // (3) KV 크기만큼 메모리 할당한 뒤, 페이지에서 해당 KV를 복사
            cb->item = malloc(s->item_size);
            if (!cb->item) {
              perror("item malloc 실패");
              exit(1);
            }
            memcpy(cb->item, &gc_buf[offset + kv_i * s->item_size],
                   s->item_size);

            meta = (struct item_metadata *)cb->item;
            if (item_is_legacy(meta))
              die("Reinsertion encountered legacy item metadata\n");
            
            /* empty slot; no need for reinsertion */
            if (item_is_empty(meta)) goto skip;
            TEST_STAT_INC(reins_examined);
            report_event(REPORT_REINSERTION);

            /*
             * The source slab's subtree is freed under its write lock when it
             * is retired, so consult it under the read lock and only while the
             * node is live. Retirement mid-batch ends the pass: the records
             * were already copied into the replacement node.
             */
            R_LOCK(&s->tree_lock);
            if (atomic_load_explicit(&node->removed, memory_order_acquire) ||
                atomic_load_explicit(&s->superseded, memory_order_acquire)) {
              R_UNLOCK(&s->tree_lock);
              free(cb->item);
              free(cb);
              goto slab_done;
            }
            c = tnt_index_lookup_utree(s->subtree, cb->item);
            src_idx = c ? (uint32_t)c->slab_idx : 0;
            R_UNLOCK(&s->tree_lock);

            /* check if c is still indexed */
            if (!c) goto skip;

            /* inspect invalid/stale marker */
            {
              unsigned char v;
              const uint32_t p = src_idx;
              asm("btl %2, %1; setc %0" : "=qm"(v) : "m"(p), "Ir"(31));
              if (v == 1) goto skip;
            }

            /* c should be valid, check once more */
            {
              size_t size = item_stored_size(meta);
              char *item_key = &cb->item[sizeof(*meta)];
              uint64_t key = *(uint64_t *)item_key;
              /* Tombstones can have: size < s->item_size, so we only check
              * for oversized items */
              if (size > s->item_size)
                die("Oversized item during reinsertion: key=%lu size=%lu slot=%lu\n",
                    key, size, s->item_size);
              if (key > 100000000) {
                printf("key: %lu, pgoff: %lu, size: %lu\n", key, offset, size);
                printf("page_idx: %lu, kv_idx: %lu\n", page_idx, kv_i);
              }
            }
            
            /*
             * Authority check: the record we hold must be the one a READ
             * would return, i.e. the first copy of the key walking up from the
             * current leaf must be this very slot. This does not trust the
             * invalid hint, which can be missed.
             */
            {
              index_entry_t *cur = tnt_index_lookup(cb, cb->item);
              int authoritative = cur != NULL && cur->slab == s &&
                                  GET_SIDX(cur->slab_idx) == slot_idx;

              tnt_index_lookup_unref(cur);
              if (!authoritative) goto skip;
            }
            /* The source this copy came from, for the split test and the completion. */
            cb->fsst_slab = s;
            cb->fsst_idx = slot_idx;

            // (4) 비동기 업데이트 호출: append only, as a shy record
            e = NULL;
            tree = centree_lookup_and_reserve(cb->item, &cb->slab_idx, &e);
            cb->slab = tree->slab;
            if (cb->slab_idx == (uint64_t)-1) {
              /*
               * In place means the key is already in the leaf: the
               * authoritative copy is where it belongs, nothing to move. The
               * reservation path took an update reference for the in-place
               * write; give it back.
               */
              __sync_fetch_and_sub(&cb->slab->update_ref, 1);
              slab_release_if_idle(cb->slab);
              goto skip;
            }
            item_mark_shy(meta);
            cb->cb = add_in_tree_for_reinsertion; /* decides at completion */
            cb->cb_cb = cb_gc;                    /* frees item and cb */
            cb->io_cb = add_item_async_cb1;
            cb->lru_entry = NULL;
            cb->io_cb(cb);
            TEST_STAT_INC(reins_issued);
            continue;
          skip:
            free(cb->item);
            free(cb);
          }

          // (라) 처리한 비트를 워드에서 지운다.
          word &= ~(1ULL << bit_pos);
        }
	//}
      }
slab_done:
      __sync_fetch_and_sub(&s->read_ref, 1);
      slab_release_if_idle(s);
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
