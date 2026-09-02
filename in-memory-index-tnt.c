#include "headers.h"
#include "indexes/tnt_centree.h"
#include "indexes/tnt_subtree.h"

#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <math.h>
#include <sys/syscall.h>

extern int print;
extern int load;

static background_queue *gc_queue, *fsst_queue;
static pthread_lock_t gc_lock;
static pthread_lock_t fsst_lock;

static int futex_wait(atomic_int *addr, int expected) {
  return syscall(SYS_futex, addr, FUTEX_WAIT, expected, NULL, NULL, 0);
}

static int futex_wake(atomic_int *addr, int num) {
  return syscall(SYS_futex, addr, FUTEX_WAKE, num, NULL, NULL, 0);
}

static __thread index_entry_t tmp_entry;

static uint64_t get_prefix_for_item(char *item) {
  struct item_metadata *meta = (struct item_metadata *)item;
  char *item_key = &item[sizeof(*meta)];
  return *(uint64_t *)item_key;
}

background_queue *bgq_get(enum fsst_mode m) {
  background_queue *queue = m == GC ? gc_queue : fsst_queue;
  return queue;
}

int bgq_is_empty(enum fsst_mode m) {
  background_queue *queue = m == GC ? gc_queue : fsst_queue;
  pthread_lock_t *lock = m == GC ? &gc_lock : &fsst_lock;
  int count;
  R_LOCK(lock);
  count = queue->count;
  R_UNLOCK(lock);
  return count == 0;
}

int bgq_count(enum fsst_mode m) {
  background_queue *queue = m == GC ? gc_queue : fsst_queue;
  pthread_lock_t *lock = m == GC ? &gc_lock : &fsst_lock;
  int count;

  R_LOCK(lock);
  count = queue->count;
  R_UNLOCK(lock);
  return count;
}

void bgq_enqueue(enum fsst_mode m, void *n) {
  background_queue *queue = m == GC ? gc_queue : fsst_queue;
  pthread_lock_t *lock = m == GC ? &gc_lock : &fsst_lock;
  bgq_node *new = (bgq_node *)malloc(sizeof(bgq_node));
  if (new == NULL) die("Fail Allocation\n");
  if (m == GC) 
    new->item = n;
  else
    new->data = n;
  new->next = NULL;
  //printf("mode: %d, enqueu: %lu\n", m, n->value.slab->seq);

  W_LOCK(lock);
  if (is_empty(queue)) {
    queue->front = new;
  } else {
    queue->rear->next = new;
  }
  queue->rear = new;
  queue->count++;
  W_UNLOCK(lock);
}

void *bgq_dequeue(enum fsst_mode m) {
  background_queue *queue = m == GC ? gc_queue : fsst_queue;
  pthread_lock_t *lock = m == GC ? &gc_lock : &fsst_lock;
  centree_node data;
  char *item;
  bgq_node *ptr;
  W_LOCK(lock);
  if (is_empty(queue)) {
    W_UNLOCK(lock);
    return NULL;
  }
  ptr = queue->front;
  if (ptr == NULL)
    printf("WHAT?\n");
  if (m == GC)
    item = ptr->item;
  else
    data = ptr->data;
  queue->front = ptr->next;

  // Check if the queue becomes empty after dequeue
  if (queue->front == NULL) {
    queue->rear = NULL;  // Set rear to NULL when the queue is empty
  }

  queue->count--;
  W_UNLOCK(lock);
  free(ptr);

  if (m == GC)
    return item;

  return &data->value;
}

void *bgq_front(enum fsst_mode m) {
  background_queue *queue = m == GC ? gc_queue : fsst_queue;
  pthread_lock_t *lock = m == GC ? &gc_lock : &fsst_lock;
  centree_node data;
  char *item;
  bgq_node *ptr;
  R_LOCK(lock);
  if (is_empty(queue)) {
    R_UNLOCK(lock);
    return 0;
  }
  ptr = queue->front;
  if (m == GC)
    item = ptr->item;
  else
    data = ptr->data;
  R_UNLOCK(lock);

  if (m == GC)
    return item;

  return &data->value;
}

void *bgq_front_node(enum fsst_mode m) {
  background_queue *queue = m == GC ? gc_queue : fsst_queue;
  pthread_lock_t *lock = m == GC ? &gc_lock : &fsst_lock;
  centree_node data;
  char *item;
  bgq_node *ptr;
  R_LOCK(lock);
  if (is_empty(queue)) {
    R_UNLOCK(lock);
    return 0;
  }
  ptr = queue->front;
  if (m == GC)
    item = ptr->item;
  else
    data = ptr->data;
  queue->front = ptr->next;
  R_UNLOCK(lock);

  if (m == GC)
    return item;

  return data;
}

centree_node dequeue_specific_node(background_queue *queue,
                                   centree_node target) {
  if (queue->front == NULL) return NULL;

  bgq_node *current = queue->front;
  bgq_node *prev = NULL;

  while (current != NULL) {
    centree_node ret;
    if (current->data == target) {
      if (prev == NULL) {
        // target node is the front node
        queue->front = current->next;
        if (queue->front == NULL) {
          queue->rear = NULL;
        }
      } else {
        prev->next = current->next;
        if (current->next == NULL) {
          queue->rear = prev;
        }
      }
      if (current->next == NULL)
        ret = NULL;
      else
        ret = current->next->data;
      free(current);
      queue->count--;
      return ret;
    }
    prev = current;
    current = current->next;
  }

  return NULL;
}

tree_entry_t *get_next_node_entry(background_queue *queue,
                                  centree_node target) {
  bgq_node *current = queue->front;

  while (current != NULL) {
    if (current->data == target) {
      if (current->next != NULL) {
        return &current->next->data->value;
      } else {
        return NULL;
      }
    }
    current = current->next;
  }

  return NULL;
}

centree_node get_next_node(background_queue *queue, centree_node target) {
  bgq_node *current = queue->front;

  while (current != NULL) {
    if (current->data == target) {
      if (current->next != NULL) {
        return current->next->data;
      } else {
        return NULL;
      }
    }
    current = current->next;
  }

  return NULL;
}

/* ========================================= */
// Worker for using indexes

static centree centree_root;
static pthread_lock_t centree_root_lock;
/*
 * Statically initialized: tnt_rebalancing() is reachable (and returns
 * -EINVAL) before centree_init() runs.
 */
static pthread_mutex_t maintenance_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_int centree_phase_state;
static void (*rebalance_precommit_test_hook)(void);
static void (*rebalance_postpublish_test_hook)(void);
static void (*index_lookup_step_test_hook)(centree_node n);

#define CENTREE_RESTRUCTURING (1U << 30)
#define CENTREE_SPLIT_COUNT_MASK (CENTREE_RESTRUCTURING - 1)

void tnt_split_phase_enter(void) {
  for (;;) {
    int state = atomic_load_explicit(&centree_phase_state,
                                     memory_order_acquire);

    if (state & CENTREE_RESTRUCTURING) {
      futex_wait(&centree_phase_state, state);
      continue;
    }
    assert((state & CENTREE_SPLIT_COUNT_MASK) <
           CENTREE_SPLIT_COUNT_MASK);

    if (atomic_compare_exchange_weak_explicit(
            &centree_phase_state, &state, state + 1,
            memory_order_acq_rel, memory_order_acquire))
      return;
  }
}

void tnt_split_phase_exit(void) {
  int old = atomic_fetch_sub_explicit(&centree_phase_state, 1,
                                      memory_order_acq_rel);

  assert((old & CENTREE_SPLIT_COUNT_MASK) > 0);
  if ((old & CENTREE_SPLIT_COUNT_MASK) == 1)
    futex_wake(&centree_phase_state, INT_MAX);
}

static void restructuring_phase_enter(void) {
  for (;;) {
    int state = atomic_load_explicit(&centree_phase_state,
                                     memory_order_acquire);

    if (state & CENTREE_RESTRUCTURING) {
      futex_wait(&centree_phase_state, state);
      continue;
    }
    if (atomic_compare_exchange_weak_explicit(
            &centree_phase_state, &state,
            state | CENTREE_RESTRUCTURING, memory_order_acq_rel,
            memory_order_acquire))
      break;
  }

  for (;;) {
    int state = atomic_load_explicit(&centree_phase_state,
                                     memory_order_acquire);

    if ((state & CENTREE_SPLIT_COUNT_MASK) == 0)
      return;
    futex_wait(&centree_phase_state, state);
  }
}

static void restructuring_phase_exit(void) {
  int old = atomic_exchange_explicit(&centree_phase_state, 0,
                                     memory_order_acq_rel);

  assert(old == CENTREE_RESTRUCTURING);
  futex_wake(&centree_phase_state, INT_MAX);
}

void tnt_maintenance_lock(void) {
  pthread_mutex_lock(&maintenance_lock);
}

void tnt_maintenance_unlock(void) {
  pthread_mutex_unlock(&maintenance_lock);
}

void tnt_set_rebalance_precommit_test_hook(void (*hook)(void)) {
  rebalance_precommit_test_hook = hook;
}

void tnt_set_rebalance_postpublish_test_hook(void (*hook)(void)) {
  rebalance_postpublish_test_hook = hook;
}

void tnt_set_index_lookup_step_test_hook(void (*hook)(centree_node n)) {
  index_lookup_step_test_hook = hook;
}

void swizzle_by_slab(size_t *arr, size_t nb_items, double x_percent) {
  // 1) Initialize arr[i] = i
  for (size_t i = 0; i < nb_items; i++) {
    arr[i] = i;
  }

  // 2) Prepare BFS queue (use GC queue)
  //    Clear any existing content
  while (!bgq_is_empty(GC)) {
    bgq_dequeue(GC);
  }
  //    Enqueue root
  centree_read_in(centree_root);
  centree_node root = centree_read_root(centree_root);
  if (root) {
    bgq_enqueue(GC, root);
  }

  uint64_t *sample_arr = malloc(sizeof(uint64_t) * root->value.slab->nb_max_items);

  // 3) Traverse, sample X% of each slab’s keys, and swap
  size_t write_idx = 0;
  srand((unsigned)time(NULL));

  while (!bgq_is_empty(GC) && write_idx < nb_items) {
    // dequeue one node
    centree_node node = (centree_node)bgq_dequeue(GC);
    if (!node) continue;

    // sample X% of this slab’s items
    struct slab *s = node->value.slab;
    size_t item_count = s->nb_items;  
    size_t sample_cnt = (size_t)(item_count * x_percent / 100.0);
    subtree_sample_percent(s->subtree, sample_arr, sample_cnt);

    for (size_t j = 0; j < sample_cnt && write_idx < nb_items; j++) {
      // pick a random key in [s->min, s->max]
      // note: if keys are not perfectly dense, you may adjust this
      size_t key = sample_arr[j];
      if (key >= nb_items) continue;  // safety check

      // swap arr[write_idx] <-> arr[key]
      size_t tmp   = arr[write_idx];
      arr[write_idx] = arr[key];
      arr[key]       = tmp;

      write_idx++;
    }

    // enqueue children
    centree_node left = centree_read_left(centree_root, node);
    centree_node right = centree_read_right(centree_root, node);
    if (left) bgq_enqueue(GC, left);
    if (right) bgq_enqueue(GC, right);
  }
  centree_read_out(centree_root);

  while (!bgq_is_empty(GC)) {
    bgq_dequeue(GC);
  }
}

centree tnt_centree(void) { return centree_root; }

/* ------------------------------------------------------------------ */
/* Recovery                                                            */

struct recover_ref {
  uint64_t id;
  size_t index;
};

static int compare_recover_ref(const void *a, const void *b) {
  const struct recover_ref *x = a, *y = b;

  return x->id < y->id ? -1 : x->id > y->id;
}

static size_t recover_lookup(const struct recover_ref *refs, size_t n,
                             uint64_t id) {
  size_t lo = 0, hi = n;

  while (lo < hi) {
    size_t mid = (lo + hi) / 2;

    if (refs[mid].id == id)
      return refs[mid].index;
    if (refs[mid].id < id)
      lo = mid + 1;
    else
      hi = mid;
  }
  return (size_t)-1;
}

static void recover_inorder(centree_node n, centree_node *out, size_t *count,
                            size_t capacity) {
  if (n == NULL)
    return;
  recover_inorder(n->lu_child[CENTREE_LU_LEFT], out, count, capacity);
  if (*count < capacity)
    out[*count] = n;
  (*count)++;
  recover_inorder(n->lu_child[CENTREE_LU_RIGHT], out, count, capacity);
}

size_t tnt_recover_tree(struct slab **slabs, const struct slab_header *hdrs,
                        size_t n, uint64_t root_id, unsigned char *reachable) {
  struct recover_ref *refs = calloc(n, sizeof(*refs));
  centree_node *nodes = calloc(n, sizeof(*nodes));
  centree_node *order = calloc(n, sizeof(*order));
  size_t root_index, nb_order = 0;
  int error;

  if (refs == NULL || nodes == NULL || order == NULL)
    die("Recovery: out of memory for %zu slabs\n", n);

  /* One node per slab, no links yet. */
  for (size_t i = 0; i < n; i++) {
    tree_entry_t value = {0};

    value.key = hdrs[i].key;
    value.seq = slabs[i]->seq;
    value.slab = slabs[i];
    nodes[i] = centree_node_new((void *)(uintptr_t)hdrs[i].key, &value);
    atomic_store_explicit(&nodes[i]->value.level, hdrs[i].level,
                          memory_order_release);
    slabs[i]->centree_node = nodes[i];
    refs[i].id = slabs[i]->seq;
    refs[i].index = i;
    reachable[i] = 0;
  }
  qsort(refs, n, sizeof(*refs), compare_recover_ref);

  /* History links from the headers; a parent is whoever names you. */
  for (size_t i = 0; i < n; i++) {
    for (int side = 0; side < 2; side++) {
      uint64_t id = hdrs[i].lu_child[side];
      size_t j;

      if (id == 0)
        continue;
      j = recover_lookup(refs, n, id);
      if (j == (size_t)-1)
        die("Recovery: slab %lu names child %lu, which is not on disk\n",
            slabs[i]->seq, id);
      if (centree_lu_parent(nodes[j]) != NULL)
        die("Recovery: slab %lu has two parents (%lu and %lu)\n", id,
            centree_lu_parent(nodes[j])->value.slab->seq, slabs[i]->seq);
      nodes[i]->lu_child[side] = nodes[j];
      centree_lu_parent_store(nodes[j], nodes[i]);
    }
    if ((hdrs[i].lu_child[0] == 0) != (hdrs[i].lu_child[1] == 0))
      die("Recovery: slab %lu has exactly one history child\n",
          slabs[i]->seq);
  }

  root_index = recover_lookup(refs, n, root_id);
  if (root_index == (size_t)-1)
    die("Recovery: ROOT names slab %lu, which is not on disk\n", root_id);
  if (centree_lu_parent(nodes[root_index]) != NULL)
    die("Recovery: the ROOT slab %lu has a parent\n", root_id);

  /*
   * In-order over the history tree is the routing in-order. Everything not
   * reached from ROOT is a leftover of an interrupted split or prune.
   */
  recover_inorder(nodes[root_index], order, &nb_order, n);
  if (nb_order > n)
    die("Recovery: history tree visits %zu nodes, %zu exist\n", nb_order, n);
  for (size_t i = 0; i < nb_order; i++) {
    centree_node node = order[i];
    int internal = node->lu_child[0] != NULL;

    if (internal != (int)(i % 2))
      die("Recovery: in-order position %zu is %s\n", i,
          internal ? "internal" : "a leaf");
  }
  for (size_t i = 0; i < nb_order; i++) {
    centree_node node = order[i];
    struct slab *s = node->value.slab;
    size_t idx = recover_lookup(refs, n, s->seq);

    reachable[idx] = 1;
    if (node->lu_child[0] != NULL) {
      /*
       * Internal nodes never take writes, whether or not they are physically
       * full -- a merged node produced by pruning usually is not.
       */
      atomic_store_explicit(&node->child_flag, 1, memory_order_release);
      atomic_store_explicit(&s->full, 1, memory_order_release);
    }
  }

  error = centree_build_from_inorder(centree_root, order, nb_order);
  if (error != 0)
    die("Recovery: cannot build the routing tree: %s\n", strerror(error));

  free(refs);
  free(nodes);
  free(order);
  return nb_order;
}

/*
 * The publication lock. Held for write only while an epoch change is made
 * visible, which is what excludes the callers that walk the current
 * generation without registering as RCU readers.
 */
void tnt_root_wlock(void) { W_LOCK(&centree_root_lock); }
void tnt_root_wunlock(void) { W_UNLOCK(&centree_root_lock); }

tree_entry_t *centree_worker_lookup(void *key) {
  return centree_lookup(centree_root, key, tnt_pointer_cmp);
}

centree_node tnt_routing_left(centree_node n) {
  centree_read_in(centree_root);
  centree_node left = centree_read_left(centree_root, n);
  centree_read_out(centree_root);
  return left;
}

centree_node tnt_routing_right(centree_node n) {
  centree_read_in(centree_root);
  centree_node right = centree_read_right(centree_root, n);
  centree_read_out(centree_root);
  return right;
}

centree_node tnt_routing_parent(centree_node n) {
  centree_read_in(centree_root);
  centree_node parent = centree_read_parent(centree_root, n);
  centree_read_out(centree_root);
  return parent;
}

uint64_t tnt_get_depth(void) {
  if (centree_root == NULL)
    return 0;
  return centree_get_depth(centree_root);
}

uint64_t tnt_get_node_count(void) {
  if (centree_root == NULL)
    return 0;
  return atomic_load_explicit(&centree_root->node_count,
                              memory_order_acquire);
}

bool tnt_rebalancing_needed(void) {
  uint64_t node_count = tnt_get_node_count();
  uint64_t depth = tnt_get_depth();

  if (node_count <= 1)
    return false;
  return (double)depth > log2((double)node_count) * REBALANCE_THRESHOLD;
}

static void centree_worker_insert(int worker_id, void *item, tree_entry_t *e) {
  struct rcu_writer writer;
  centree_node n;

  writer = rcu_writer_in(&centree_root->topology_rcu);
  W_LOCK(&centree_root_lock);
  n = centree_insert(centree_root, &writer, (void *)e->key, e,
                     tnt_pointer_cmp);
  e->slab->centree_node = (void *)n;
  rcu_writer_publish_deferred(&centree_root->topology_rcu, &writer,
                              NULL, NULL);
  W_UNLOCK(&centree_root_lock);
  rcu_writer_finish_deferred(&centree_root->topology_rcu, &writer);
}

void wakeup_subtree_get(void *n) {
  centree_node p = (centree_node)n;
  if (p) {
    atomic_store(&p->child_flag, 1);
    //printf("wake: %lu, %d\n", n->parent->key, atomic_load(&n->parent->child_flag));
    futex_wake(&p->child_flag, INT_MAX);
  }
}

// void tnt_subtree_delete(int worker_id, void *item) {
//    W_LOCK(&centree_root_lock);
//    centree_delete(centree_root, (void*)get_prefix_for_item(item),
//    tnt_pointer_cmp); W_UNLOCK(&centree_root_lock);
// }
//
index_entry_t *subtree_worker_lookup_utree(subtree_t *tree, void *item) {
  uint64_t hash = get_prefix_for_item(item);
  // printf("# \tLookup Debug hash 1: %lu\n", hash);
  int res =
    subtree_find(tree, (unsigned char *)&hash, sizeof(hash), &tmp_entry);
  if (res)
    return &tmp_entry;
  else
    return NULL;
}

index_entry_t *subtree_worker_lookup_ukey(subtree_t *tree, uint64_t key) {
  int res = subtree_find(tree, (unsigned char *)&key, sizeof(key), &tmp_entry);
  if (res)
    return &tmp_entry;
  else
    return NULL;
}

int tnt_centree_node_is_child (centree_node n) {
  centree_read_in(centree_root);
  centree_node left = centree_read_left(centree_root, n);
  centree_node right = centree_read_right(centree_root, n);
  centree_read_out(centree_root);
  return !right && !left;
}

uint64_t tnt_get_centree_level(void *n) {
  return atomic_load_explicit(&((centree_node)n)->value.level,
                              memory_order_acquire);
}

static void tnt_subtree_add_common(struct slab *s, void *tree, void *filter,
                                   uint64_t tmp_key,
                                   bool split_phase_held) {
  tree_entry_t e = {
    .seq = s->seq,
    .key = tmp_key,
    .slab = s,
  };

  subtree_set_slab(tree, s);
  s->subtree = tree;
#if WITH_FILTER
  s->filter = filter;
#endif
  if (split_phase_held) {
    int state = atomic_load_explicit(&centree_phase_state,
                                     memory_order_acquire);

    assert((state & CENTREE_SPLIT_COUNT_MASK) > 0);
  } else {
    tnt_split_phase_enter();
  }
  centree_worker_insert(0, NULL, &e);
  if (!split_phase_held)
    tnt_split_phase_exit();
}

void tnt_subtree_add(struct slab *s, void *tree, void *filter,
                     uint64_t tmp_key) {
  tnt_subtree_add_common(s, tree, filter, tmp_key, false);
}

void tnt_subtree_add_split(struct slab *s, void *tree, void *filter,
                           uint64_t tmp_key) {
  tnt_subtree_add_common(s, tree, filter, tmp_key, true);
}

void subtree_worker_insert(subtree_t *tree, void *item, index_entry_t *e) {
  uint64_t hash = get_prefix_for_item(item);
  subtree_insert(tree, (unsigned char *)&hash, sizeof(hash), e);
}

int subtree_worker_delete(subtree_t *tree, void *item) {
  uint64_t hash = get_prefix_for_item(item);
  return subtree_delete(tree, (unsigned char *)&(hash), sizeof(hash));
}

int subtree_worker_invalid_utree(subtree_t *tree, void *item) {
  uint64_t hash = get_prefix_for_item(item);
  return subtree_set_invalid(tree, (unsigned char *)&hash, sizeof(hash));
}

/* ========================================= */
// TNT

/*
 * Lock-free routing descent. One RCU generation covers root and every child
 * edge. The returned node remains allocated after read_out() under the
 * center-tree no-reclamation lifetime contract.
 */
static centree_node centree_find_leaf(void *key) {
  centree_read_in(centree_root);
  centree_node n = centree_read_root(centree_root);
  while (n != NULL) {
    int cmp = tnt_pointer_cmp(
        key, (void *)(uintptr_t)centree_pivot_load(n));
    centree_node next =
        (centree_route_left(cmp) ? centree_read_left(centree_root, n)
                                 : centree_read_right(centree_root, n));
    if (!next) break;
    n = next;
  }
  centree_read_out(centree_root);
  return n;
}

tree_entry_t *tnt_parent_subtree_get(void *centnode) {
  centree_node p = tnt_routing_parent(centnode);
  return p ? &p->value : NULL ;
}

size_t reserve_slot(struct slab *s) {
  size_t old;

  // 1) 먼저 풀 여부 확인
  if (atomic_load_explicit(&s->full, memory_order_acquire)) {
    return (size_t)-1; // 풀 상태
  }

  // 2) CAS 루프: old < max 인 경우에만 +1 시도
  old = atomic_load_explicit(&s->last_item, memory_order_relaxed);
  while (old < s->nb_max_items) {
    if (atomic_compare_exchange_weak_explicit(
      &s->last_item,
      &old,           // expected value
      old + 1,        // desired value
      memory_order_acquire,
      memory_order_relaxed)) {
      // CAS 성공: old 가 예약된 슬롯
      break;
    }
    // CAS 실패 시 old 가 최신 값으로 업데이트되므로,
    // 다시 old < max 검사 후 재시도
  }

  // 3) old >= max → 풀 상태
  if (old >= s->nb_max_items) {
    atomic_store_explicit(&s->full, 1, memory_order_release);
    return (size_t)-1;
  }

  // 4) 예약된 슬롯 리턴
  // (old 는 [0, nb_max_items-1] 범위 보장)
  if (old + 1 == s->nb_max_items) {
    //printf("last one: %lu\n", s->key);
    atomic_store_explicit(&s->full, 1, memory_order_release);
  }
  return old;
}

tree_entry_t *tnt_subtree_get(void *key, uint64_t *idx, index_entry_t *old_e) {
  centree t = centree_root;
  centree_node n, prev;

restart:
  R_LOCK(&centree_root_lock);
  n = centree_current_root(t);
  while (1) {
    struct slab *s = n->value.slab;
    int comp_result;

    /*
     * Taken before full or last_item is consulted: slab_freeze() publishes
     * full and then drains update_ref, so either it waits for this writer or
     * this writer sees full and moves on. Kept on the way out, released by
     * the write completion.
     */
    __sync_fetch_and_add(&s->update_ref, 1);

    // ── 1) full 아닌 slab에서만 in-place 업데이트 허용 ──
    if (!atomic_load_explicit(&s->full, memory_order_acquire)
      && old_e && s == old_e->slab) {
      *idx = (uint64_t)-1;
      break;
    }

    // ── 2) 슬롯 예약 (full 이면 reserve_slot()이 -1 리턴) ──
    size_t slot = reserve_slot(s);
    if (slot != (size_t)-1) {
      __sync_fetch_and_add(&s->nb_items, 1);
      *idx = slot;
      break;
    }

    // ── 3) slab full 이거나 예약 실패 → 트리 아래로 ──
    /* Released before descending or waiting on child_flag. */
    __sync_fetch_and_sub(&s->update_ref, 1);
    /* This can be the last reference on a slab retired a moment ago. */
    slab_release_if_idle(s);

    //R_LOCK(&s->tree_lock);
    //if (s->full == 0) {
    //  R_UNLOCK(&s->tree_lock);
    //  if (old_e && s == old_e->slab) {
    //    __sync_fetch_and_add(&s->update_ref, 1);
    //    // IN-PLACE UPSERT
    //    *idx = -1;
    //    break;
    //  }
    //  W_LOCK(&s->tree_lock);
    //  if (s->last_item >= s->nb_max_items) {
    //    W_UNLOCK(&s->tree_lock);
    //    continue;
    //  }
    //  assert(s->last_item < s->nb_max_items);
    //  __sync_fetch_and_add(&s->update_ref, 1);
    //  *idx = s->last_item++;
    //  s->nb_items++;
    //  if (s->last_item == s->nb_max_items) s->full = 1;
    //  W_UNLOCK(&s->tree_lock);
    //  break;
    //} else {
    //  assert(s->last_item == s->nb_max_items);
    //}
    //R_UNLOCK(&s->tree_lock);

    prev = n;
    do {
      R_LOCK(&s->tree_lock);
      comp_result = tnt_pointer_cmp(
          (void *)key, (void *)(uintptr_t)centree_pivot_load(prev));
      if (centree_route_left(comp_result)) {
        n = centree_current_left(t, prev);
      } else {
        assert(comp_result >= 0);
        n = centree_current_right(t, prev);
      }
      R_UNLOCK(&s->tree_lock);
      if (!n) {
        R_UNLOCK(&centree_root_lock);
        /*
         * Retirement is the other way out of this wait: a pruned leaf never
         * grows children, so the pruner sets removed and wakes the waiters.
         * Testing removed as well as the flag also covers a wake that races
         * with the flag store.
         */
        while (atomic_load(&prev->child_flag) == 0 &&
               !atomic_load(&prev->removed)) {
          futex_wait(&prev->child_flag, 0);
        }
        if (atomic_load(&prev->removed))
          goto restart;
        R_LOCK(&centree_root_lock);
      }
    } while (!n);
  }
  R_UNLOCK(&centree_root_lock);
  if (n)
    return &n->value;
  else
    return NULL;
}

struct tree_entry* centree_lookup_and_reserve(
  void *item,
  uint64_t *out_idx,
  index_entry_t **out_e)
{
  // item -> key 추출
  struct item_metadata *meta = (struct item_metadata *)item;
  char *item_key = &item[sizeof(*meta)];
  uint64_t key = *(uint64_t *)item_key;

  // 4) 최초 하강: 기존 find 함수 재사용
  centree_node n;

  // 5) 이후부터는 root 락을 잡고 slab 할당/하강 로직 수행
  centree_node prev;

restart:
  n = centree_find_leaf((void *)key);

  R_LOCK(&centree_root_lock);
  while (1) {
    struct slab *s = n->value.slab;
    index_entry_t *found = NULL;

    /*
     * Taken before full or last_item is consulted: slab_freeze() publishes
     * full and then drains update_ref, so either it waits for this writer or
     * this writer sees full and moves on. Kept on the way out, released by
     * the write completion.
     */
    __sync_fetch_and_add(&s->update_ref, 1);

    // a) slab 내부 lookup
    R_LOCK(&s->tree_lock);
    if (key <= s->max && key >= s->min)
    	found = subtree_worker_lookup_utree(s->subtree, item);
    R_UNLOCK(&s->tree_lock);

    // b) in-place update 조건
    if (!atomic_load_explicit(&s->full, memory_order_acquire) && found) {
      R_UNLOCK(&centree_root_lock);
      *out_idx = (uint64_t)-1;
      *out_e = found;
      return &n->value;
    }

    // c) 새 슬롯 예약 시도
    size_t slot = reserve_slot(s);
    if (slot != (size_t)-1) {
      __sync_fetch_and_add(&s->nb_items, 1);

      slab_widen_range(s, key);

      *out_idx = slot;
      break;
    }

    // d) slab full -> split된 자식으로 하강 (자식 없으면 기다림)
    /* Released before descending or waiting on child_flag. */
    __sync_fetch_and_sub(&s->update_ref, 1);
    /* This can be the last reference on a slab retired a moment ago. */
    slab_release_if_idle(s);

    prev = n;
    do {
      int comp = tnt_pointer_cmp(
          (void *)key, (void *)(uintptr_t)centree_pivot_load(prev));
      R_LOCK(&s->tree_lock);
      n = (centree_route_left(comp)
               ? centree_current_left(centree_root, prev)
               : centree_current_right(centree_root, prev));
      R_UNLOCK(&s->tree_lock);

      if (!n) {
        R_UNLOCK(&centree_root_lock);
        /*
         * Retirement is the other way out of this wait: a pruned leaf never
         * grows children, so the pruner sets removed and wakes the waiters.
         * Testing removed as well as the flag also covers a wake that races
         * with the flag store. The update reference was already released
         * above, so restarting leaks nothing.
         */
        while (atomic_load_explicit(&prev->child_flag,
                                    memory_order_acquire) == 0 &&
               !atomic_load_explicit(&prev->removed, memory_order_acquire)) {
          futex_wait(&prev->child_flag, 0);
        }
        if (atomic_load_explicit(&prev->removed, memory_order_acquire))
          goto restart;
        R_LOCK(&centree_root_lock);
      }
    } while (!n);
    // 새 노드로 내려갔으니 다시 할당 시도
  }
  R_UNLOCK(&centree_root_lock);

  // 6) upward lookup: 리프 노드 n에서부터 위로 올라가며 이전 entry 찾기
  /*
   * Retired slabs are skipped rather than restarted on: this walk only looks
   * for the older copy to invalidate, and the copy it would have found was
   * already carried into the replacement node. Its subtree is freed under the
   * write lock, so it must not be consulted once removed is set.
   */
  *out_e = NULL;
  for (centree_node cur = n; cur; cur = centree_lu_parent(cur)) {
    struct slab *s2 = cur->value.slab;
    index_entry_t *e2 = NULL;
    R_LOCK(&s2->tree_lock);
    if (!atomic_load_explicit(&cur->removed, memory_order_acquire) &&
        key <= s2->max && key >= s2->min)
    	e2 = subtree_worker_lookup_utree(s2->subtree, item);
    R_UNLOCK(&s2->tree_lock);
    if (e2) {
      *out_e = e2;
      break;
    }
  }

  // 7) 최종 대상 tree_entry 리턴
  return &n->value;
}


tree_entry_t *tnt_traverse_use_seq(int seq) {
  tree_entry_t *t;
  R_LOCK(&centree_root_lock);
  t = centree_traverse_useq(centree_root, seq);
  R_UNLOCK(&centree_root_lock);
  return t;
}

int tnt_get_nodes_at_level(int level, background_queue *q) {
  int current_level = 0;
  int count = 0;
  background_queue queue;

  init_queue(&queue);
  centree_read_in(centree_root);
  enqueue_centnode(&queue, centree_read_root(centree_root));
  while (!is_empty(&queue)) {
    int level_size = queue.count;
    if (current_level == level) {
      while (level_size-- > 0) {
        centree_node node = dequeue_centnode(&queue);
        enqueue_centnode(q, node);
        count++;
      }
      break;
    }

    while (level_size-- > 0) {
      centree_node node = dequeue_centnode(&queue);
      centree_node left = centree_read_left(centree_root, node);
      centree_node right = centree_read_right(centree_root, node);
      if (left != NULL) enqueue_centnode(&queue, left);
      if (right != NULL) enqueue_centnode(&queue, right);
    }
    current_level++;
  }
  centree_read_out(centree_root);

  return count;
}

static __thread int try = 0;
static __thread uint64_t try_key = 0;

index_entry_t *tnt_index_lookup_for_test(struct slab_callback *cb, void *item, int *ttry, uint64_t *tkey) {
  index_entry_t *e = tnt_index_lookup(cb, item);
  *ttry = try; *tkey = try_key;
  return e;
}

/*
 * Drops the read reference tnt_index_lookup() took on the entry's slab. Only
 * for callers that do not go on to read the record: the read completion
 * (read_item_async_cb) drops it otherwise.
 */
void tnt_index_lookup_unref(index_entry_t *e) {
  if (e == NULL)
    return;
  __sync_fetch_and_sub(&e->slab->read_ref, 1);
}

index_entry_t *tnt_index_lookup(struct slab_callback *cb, void *item) {
  struct item_metadata *meta = (struct item_metadata *)item;
  char *item_key = &item[sizeof(*meta)];
  uint64_t key = *(uint64_t *)item_key;
  centree_node n;
  index_entry_t *e = NULL, *tmp = NULL;
  int tmp_try = 0, upward_len = 1;
  int count = 0;
  bool leaf_timed = false;

restart:
  e = NULL;
  tmp_try = 0;
  upward_len = 1;
  count = 0;

  n = centree_find_leaf((void*)key);
  if (!leaf_timed) {
    /* Only the first descent is timed; a restart must not add a stage. */
    add_time_in_payload(cb, TIMING_STAGE_LEAF_FOUND);
    leaf_timed = true;
  }

  // Leaf node에서 upward 탐색
  while (n != NULL) {
    struct slab *s = n->value.slab;

    if (index_lookup_step_test_hook != NULL)
      index_lookup_step_test_hook(n);

    R_LOCK(&s->tree_lock);
    /*
     * A retired node's valid entries were copied into its replacement, which
     * the published topology routes to. Walking past it would miss them, so
     * restart the descent instead. removed is monotone and is only set after
     * the replacement is published, so this cannot spin.
     */
    if (atomic_load_explicit(&n->removed, memory_order_acquire)) {
      R_UNLOCK(&s->tree_lock);
      goto restart;
    }
    tmp_try++;
    tmp = NULL;
    if (s->min != -1) {
#if WITH_FILTER
      if (filter_contain(s->filter, (unsigned char *)&key)) {
#endif
	    if (key <= s->max && key >= s->min) {
	      count++;
          tmp = subtree_worker_lookup_utree(s->subtree, item);
	    }
        if (tmp) {
          //  && !TEST_INVAL(tmp->slab_idx)
          //  이 조건을 지웠는데, update의 lock을 최소화하기 위해서
          //  1. 지운다. 2. 추가한다 이 과정에서 1-2 사이에 있는 중일 수 있음
          /*
           * The reference is taken under the lock that found the entry and
           * after the removed check, so a retired slab can never gain a new
           * reader. The caller reading the record hands it to the read
           * completion; any other caller must tnt_index_lookup_unref().
           */
          assert(tmp->slab == s);
          __sync_fetch_and_add(&s->read_ref, 1);
          e = tmp;
          R_UNLOCK(&s->tree_lock);
	          if (tmp_try > try) {
	            try = tmp_try;
	            try_key = key;
	          }
          //printf("[%lu] try: %d\n", key, try);
          break;
        }
#if WITH_FILTER
      }
#endif
    }
    R_UNLOCK(&s->tree_lock);

    // 부모(history) 노드로 이동
    upward_len++;
    n = centree_lu_parent(n);
  }
  add_time_in_payload(cb, TIMING_STAGE_INDEX_LOOKUP_DONE);
  // printf("%d", try);
  
  if (e){
    add_upward_in_lat_ctx(cb, upward_len);
    add_scount_in_lat_ctx(cb, count);
    if (cfg.with_reins)
      e->slab->upward_maxlen = upward_len;
    return e;
  }
  return NULL;
}

void tnt_index_add(struct slab_callback *cb, void *item) {
  index_entry_t new_entry;
  new_entry.slab = cb->slab;
  new_entry.slab_idx = cb->slab_idx;
  subtree_worker_insert(cb->slab->subtree, item, &new_entry);
}

int tnt_index_invalid(void *item) {
  centree t = centree_root;
  struct item_metadata *meta = (struct item_metadata *)item;
  char *item_key = &item[sizeof(*meta)];
  uint64_t key = *(uint64_t *)item_key;
  centree_node n;
  int count = 0;

  R_LOCK(&centree_root_lock);
  n = centree_current_root(t);
  while (n != NULL) {
    struct slab *s = n->value.slab;
    int comp_result;

    R_LOCK(&s->tree_lock);
    if (s->min != -1) {
#if WITH_FILTER
      if (filter_contain(s->filter, (unsigned char *)&key)) {
        #endif
        if (subtree_worker_invalid_utree(s->subtree, item)) {
          __sync_fetch_and_sub(&s->nb_items, 1);
          count++;
          R_UNLOCK(&s->tree_lock);
          R_UNLOCK(&centree_root_lock);
          return 1;
        }
#if WITH_FILTER
      }
#endif
    }
    comp_result = tnt_pointer_cmp(
        (void *)key, (void *)(uintptr_t)centree_pivot_load(n));

    R_UNLOCK(&s->tree_lock);

    if (centree_route_left(comp_result)) {
      n = centree_current_left(t, n);
    } else {
      assert(comp_result >= 0);
      n = centree_current_right(t, n);
    }
  }
  R_UNLOCK(&centree_root_lock);

  return count;
}

subtree_t *tnt_subtree_create(void) { return subtree_create(); }

void centree_init(void) {
  //centree_root = malloc(sizeof(*centree_root));
  centree_root = centree_create();
  gc_queue = malloc(sizeof(background_queue));
  fsst_queue = malloc(sizeof(background_queue));
  init_queue(gc_queue);
  init_queue(fsst_queue);
  atomic_init(&centree_phase_state, 0);
  INIT_LOCK(&centree_root_lock, NULL);
  INIT_LOCK(&gc_lock, NULL);
  INIT_LOCK(&fsst_lock, NULL);
}

/* ========================================= */
// ETC

void tnt_print(void) {
  R_LOCK(&centree_root_lock);
  centree_print(centree_root);
  R_UNLOCK(&centree_root_lock);
}

int tnt_rebalancing(void) {
  struct centree_balance_plan *plan = NULL;
  int result;

  if (centree_root == NULL)
    return -EINVAL;

  tnt_maintenance_lock();
  restructuring_phase_enter();

  centree_node root = centree_current_root(centree_root);
  if (root == NULL) {
    result = TNT_REBALANCE_NOOP;
  } else {
    bool topology_noop =
        centree_current_left(centree_root, root) == NULL &&
        centree_current_right(centree_root, root) == NULL;
    int error = centree_balance_prepare(centree_root, &plan);

    if (error != 0) {
      result = -error;
    } else {
      if (rebalance_precommit_test_hook != NULL)
        rebalance_precommit_test_hook();

      W_LOCK(&centree_root_lock);
      centree_balance_publish(plan);
      W_UNLOCK(&centree_root_lock);
      if (rebalance_postpublish_test_hook != NULL)
        rebalance_postpublish_test_hook();
      centree_balance_complete(plan);
      result = topology_noop ? TNT_REBALANCE_NOOP : TNT_REBALANCE_SUCCESS;
    }
  }

  restructuring_phase_exit();
  tnt_maintenance_unlock();
  return result;
}
