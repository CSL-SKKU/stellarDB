#include "cpp-btree/btree_map.h"
#include <vector>
#include <random>
#include "tnt_subtree.h"

using namespace std;
using namespace btree;

extern "C" {
static uint64_t marked_total;

uint64_t subtree_marked_total(void) {
  return __atomic_load_n(&marked_total, __ATOMIC_RELAXED);
}

static void marked_add(subtree_t *t) {
  __atomic_fetch_add(&t->marked_count, 1, __ATOMIC_RELAXED);
  __atomic_fetch_add(&marked_total, 1, __ATOMIC_RELAXED);
}

static inline void set_inval(uint32_t *addr) {
  asm("btsl %1, %0" : "+m"(*addr) : "Ir"(31));
}

static inline int tas_inval(uint32_t *addr) {
  int oldbit;
  asm("lock; btsl %2, %1\n\tsbbl %0, %0" : "=r"(oldbit), "=m"(*addr) : "r"(31));
  return oldbit;
}

static inline unsigned char test_inval(uint32_t *addr) {
  unsigned char v;
  const uint32_t *p = addr;
  asm("btl %2, %1; setc %0" : "=qm"(v) : "m"(*p), "Ir"(31));
  return v;
}
subtree_t *subtree_create() {
  subtree_t *t = (subtree_t*) malloc(sizeof(subtree_t));
  btree_map<uint64_t, uint32_t> *b =
      new btree_map<uint64_t, uint32_t>();
  t->tree = b;
  t->marked_count = 0;
  return t;
}

void subtree_set_slab(subtree_t *t, void *slab) {
  t->slab = slab;
  return;
}

int subtree_find(subtree_t *t, unsigned char *k, size_t len,
                 struct index_entry *e) {
  // printf("# \tLookup Debug hash 2: %hhu\n", *k);
  uint64_t hash = *(uint64_t *)k;
  // printf("# \tLookup Debug hash 3: %lu\n", hash);
  btree_map<uint64_t, uint32_t> *b =
      static_cast<btree_map<uint64_t, uint32_t> *>(t->tree);
  auto i = b->find(hash);
  if (i != b->end()) {
    index_entry_t cur = {{(struct slab*)t->slab}, {(uint64_t)i->second}};
    *e = cur;
    return 1;
  } else {
    return 0;
  }
}
int subtree_set_invalid(subtree_t *t, unsigned char *k, size_t len) {
  uint64_t hash = *(uint64_t *)k;
  btree_map<uint64_t, uint32_t> *b =
      static_cast<btree_map<uint64_t, uint32_t> *>(t->tree);
  auto i = b->find(hash);
  if (i != b->end()) {
    if (!tas_inval(&i->second)) {
      // printf("SET INVAL %lu (s, %lu)\n", hash, i->second.slab_idx);
      set_inval(&i->second);
      marked_add(t);
      return 1;
    }
    // printf("Already INVAL %lu(s, %lu)\n", hash, i->second.slab_idx);
    return 0;
  } else {
    return 0;
  }
}

int subtree_delete(subtree_t *t, unsigned char *k, size_t len) {
  uint64_t hash = *(uint64_t *)k;
  btree_map<uint64_t, uint32_t> *b =
      static_cast<btree_map<uint64_t, uint32_t> *>(t->tree);
  auto i = b->find(hash);
  if (i == b->end()) return 0;
  if (sidx_is_invalid(i->second)) {
    __atomic_fetch_sub(&t->marked_count, 1, __ATOMIC_RELAXED);
    __atomic_fetch_sub(&marked_total, 1, __ATOMIC_RELAXED);
  }
  b->erase(i);
  return 1;
}

void subtree_insert(subtree_t *t, unsigned char *k, size_t len,
                    struct index_entry *e) {
  uint64_t hash = *(uint64_t *)k;
  btree_map<uint64_t, uint32_t> *b =
      static_cast<btree_map<uint64_t, uint32_t> *>(t->tree);
  auto inserted = b->insert(make_pair(hash, (uint32_t)e->slab_idx));
  if (inserted.second && sidx_is_invalid(e->slab_idx)) marked_add(t);
}

void subtree_insert_shy(subtree_t *t, unsigned char *k, size_t len,
                        struct index_entry *e) {
  uint64_t hash = *(uint64_t *)k;
  btree_map<uint64_t, uint32_t> *b =
      static_cast<btree_map<uint64_t, uint32_t> *>(t->tree);
  auto inserted = b->insert(make_pair(hash, (uint32_t)e->slab_idx | SIDX_SHY_BIT));
  if (inserted.second && sidx_is_invalid(e->slab_idx)) marked_add(t);
}

int subtree_clear_shy(subtree_t *t, unsigned char *k, size_t len) {
  uint64_t hash = *(uint64_t *)k;
  btree_map<uint64_t, uint32_t> *b =
      static_cast<btree_map<uint64_t, uint32_t> *>(t->tree);
  auto i = b->find(hash);
  if (i == b->end())
    return 0;
  __atomic_and_fetch(&i->second, ~SIDX_SHY_BIT, __ATOMIC_RELAXED);
  return 1;
}

int subtree_sample_percent(subtree_t *t,
                           uint64_t *out_keys,
                           size_t sample_cnt) {
  auto b = static_cast<btree_map<uint64_t,uint32_t>*>(t->tree);
  size_t N = b->size();
  if (N == 0 || sample_cnt == 0) return 0;

  // 리저버 샘플링 준비
  std::vector<uint64_t> reservoir;
  reservoir.reserve(sample_cnt);

  std::mt19937_64 rng{std::random_device{}()};
  size_t idx = 0;

  for (auto it = b->begin(); it != b->end(); ++it, ++idx) {
    uint64_t key = it->first;

    if (idx < sample_cnt) {
      // 초기 sample_cnt개는 곧바로 reservoir에 담고
      reservoir.push_back(key);
    } else {
      // 이후엔 [0..idx] 사이에서 랜덤 인덱스 j를 뽑아
      // j < sample_cnt 이면 reservoir[j]를 대체
      std::uniform_int_distribution<size_t> dist(0, idx);
      size_t j = dist(rng);
      if (j < sample_cnt) {
        reservoir[j] = key;
      }
    }
    if (idx + 1 >= N) break;
  }

  // 결과를 out_keys에 복사
  for (size_t i = 0; i < sample_cnt; i++) {
    out_keys[i] = reservoir[i];
  }
  return (int)sample_cnt;
}

int subtree_forall_keys(subtree_t *t, void (*cb)(uint64_t h, int n, void *data),
                         void *data) {
  btree_map<uint64_t, uint32_t> *b =
      static_cast<btree_map<uint64_t, uint32_t> *>(t->tree);
  int n = 0;
  auto i = b->begin();
  while (i != b->end()) {
    cb(i->first, n++, data);
    i++;
  }
  return n;
}
int subtree_forall_entries(subtree_t *t,
                           void (*cb)(uint64_t key, uint32_t slot, void *data),
                           void *data) {
  btree_map<uint64_t, uint32_t> *b =
      static_cast<btree_map<uint64_t, uint32_t> *>(t->tree);
  int n = 0;
  auto i = b->begin();
  while (i != b->end()) {
    cb(i->first, i->second, data);
    n++;
    i++;
  }
  return n;
}

int subtree_forall_invalid(subtree_t *t, void *data, void (*cb)(void *slab, uint64_t slab_idx)) {
  btree_map<uint64_t, uint32_t> *b =
      static_cast<btree_map<uint64_t, uint32_t> *>(t->tree);
  int count = 0;
  auto i = b->begin();
  while (i != b->end()) {
    if (!test_inval(&i->second)) {
      // 여기서 inval을 하면 더 빨리 tree free 가능
      // 문제가 생기느냐? tree가 free되는 순간이 온다면
      // 트리가 더이상 필요하지 않을 것 따라서 문제 없음
      // set_inval(&i->second.slab_idx);
      // printf("FSST: %lu ", i->first);
      cb(data, i->second);
      count++;
    }
    // else
    // printf("INVAL FSST: %lu\n", i->first);
    i++;
  }
  return count;
}


void subtree_free(subtree_t *t) {
  __atomic_fetch_sub(&marked_total, t->marked_count, __ATOMIC_RELAXED);
  btree_map<uint64_t, uint32_t> *b =
      static_cast<btree_map<uint64_t, uint32_t> *>(t->tree);
  delete b;
  free(t);
}
void subtree_all_free(subtree_t *t) {
  __atomic_fetch_sub(&marked_total, t->marked_count, __ATOMIC_RELAXED);
  btree_map<uint64_t, uint32_t> *b =
      static_cast<btree_map<uint64_t, uint32_t> *>(t->tree);
  b->erase(b->begin(), b->end());
  delete b;
  free(t);
}
}
