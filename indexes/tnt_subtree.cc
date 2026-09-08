#include "cpp-btree/btree_map.h"
#include <vector>
#include <cerrno>
#include <new>
#include <stdexcept>
#include "tnt_subtree.h"
#include "../report.h"

using namespace std;
using namespace btree;

extern "C" {
#ifdef STELLAR_TESTING
static uint64_t marked_total;

uint64_t subtree_marked_total(void) {
  return __atomic_load_n(&marked_total, __ATOMIC_RELAXED);
}
#endif

static int track_marked(void) {
#ifdef STELLAR_TESTING
  return 1;
#else
  return report_marked_entries();
#endif
}
static void marked_add(subtree_t *t) {
  if (track_marked()) __atomic_fetch_add(&t->marked_count, 1, __ATOMIC_RELAXED);
#ifdef STELLAR_TESTING
  __atomic_fetch_add(&marked_total, 1, __ATOMIC_RELAXED);
#endif
}
static bool tombstone_at(subtree_t *t, uint32_t slot) {
  auto bits = static_cast<vector<uint64_t> *>(t->report_tombstones);
  size_t word = GET_SIDX(slot) / 64;
  return bits && word < bits->size() && ((*bits)[word] & (UINT64_C(1) << (GET_SIDX(slot) % 64)));
}
void subtree_report_record(subtree_t *t, uint64_t key, uint64_t slot, int tombstone) {
  if (!report_entry_classes() || (!tombstone && !t->report_tombstones)) return;
  auto b = static_cast<btree_map<uint64_t, uint32_t> *>(t->tree);
  auto i = b->find(key);
  if (i == b->end() || GET_SIDX(i->second) != slot) return;
  bool old = tombstone_at(t, i->second);
  if (old == (bool)tombstone) return;
  auto bits = static_cast<vector<uint64_t> *>(t->report_tombstones);
  if (!bits) t->report_tombstones = bits = new vector<uint64_t>();
  size_t word = slot / 64;
  if (word >= bits->size()) bits->resize(word + 1, 0);
  (*bits)[word] ^= UINT64_C(1) << (slot % 64);
  if (!sidx_is_invalid(i->second)) {
    if (tombstone) __atomic_fetch_add(&t->live_tombstones, 1, __ATOMIC_RELAXED);
    else __atomic_fetch_sub(&t->live_tombstones, 1, __ATOMIC_RELAXED);
  }
}
void subtree_report_counts(subtree_t *t, uint64_t *total, uint64_t *stale, uint64_t *tombstones) {
  auto b = static_cast<btree_map<uint64_t, uint32_t> *>(t->tree);
  *total += b->size();
  *stale += t->marked_count;
  *tombstones += t->live_tombstones;
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
  t->slab = NULL;
  t->tree = b;
  t->report_tombstones = NULL;
  t->live_tombstones = 0;
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
      if (report_entry_classes() && tombstone_at(t, i->second))
        __atomic_fetch_sub(&t->live_tombstones, 1, __ATOMIC_RELAXED);
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
    if (track_marked()) __atomic_fetch_sub(&t->marked_count, 1, __ATOMIC_RELAXED);
#ifdef STELLAR_TESTING
    __atomic_fetch_sub(&marked_total, 1, __ATOMIC_RELAXED);
#endif
  }
  if (report_entry_classes() && tombstone_at(t, i->second)) {
    if (!sidx_is_invalid(i->second))
      __atomic_fetch_sub(&t->live_tombstones, 1, __ATOMIC_RELAXED);
    auto bits = static_cast<vector<uint64_t> *>(t->report_tombstones);
    (*bits)[GET_SIDX(i->second) / 64] &= ~(UINT64_C(1) << (GET_SIDX(i->second) % 64));
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

int subtree_invalidate_idle(struct subtree_idle_node *nodes, size_t count,
                            void (*progress)(void *), void *context,
                            uint64_t *invalidated) {
  using tree_type = btree_map<uint64_t, uint32_t>;
  struct entry {
    uint64_t key;
    uint32_t *word;             // stable: the indexes cannot change structure
    subtree_idle_node *owner;
    size_t previous;            // former ancestor, encoded as index + 1
  };
  struct frame { size_t node, begin; };
  *invalidated = 0;
  try {
    // Allocate everything before marking. Unlike a node-based hash map, the
    // table/path storage is reused without allocating for every indexed key.
    vector<size_t> lengths(count);
    size_t peak = 0;
    for (size_t i = 0; i < count; i++) {
      auto &n = nodes[i];
      if (!n.index || !n.valid || (i == 0 ? n.parent != SIZE_MAX : n.parent >= i))
        return -EINVAL;
      size_t length = i ? lengths[n.parent] : 0;
      bool internal = i + 1 < count && nodes[i + 1].parent == i;
      if (internal) {
        size_t items = static_cast<tree_type *>(n.index->tree)->size();
        if (items > SIZE_MAX - length) return -EOVERFLOW;
        length += items;
      }
      lengths[i] = length;
      if (length > peak) peak = length;
    }
    size_t capacity = 8;
    if (peak > SIZE_MAX / 2) return -EOVERFLOW;
    while (capacity / 2 < peak) {
      if (capacity > SIZE_MAX / 2) return -EOVERFLOW;
      capacity *= 2;
    }
    vector<size_t> table(capacity, 0);
    vector<entry> path;
    vector<frame> frames;
    path.reserve(peak);
    frames.reserve(count);
    const size_t mask = capacity - 1;
    auto home = [mask](uint64_t key) {
      key ^= key >> 30; key *= UINT64_C(0xbf58476d1ce4e5b9);
      key ^= key >> 27; key *= UINT64_C(0x94d049bb133111eb);
      return (size_t)(key ^ (key >> 31)) & mask;
    };
    auto find = [&](uint64_t key) {
      size_t at = home(key);
      while (table[at] && path[table[at] - 1].key != key) at = (at + 1) & mask;
      return at;
    };
    size_t visited = 0;
    auto pop = [&]() {
      size_t begin = frames.back().begin;
      while (path.size() > begin) {
        auto &e = path.back();
        size_t at = find(e.key);
        table[at] = e.previous;
        if (!e.previous) {
          // Backshift deletion avoids accumulating tombstones over a long
          // traversal. Ancestor stack indices remain valid when cells move.
          size_t hole = at, next = (at + 1) & mask;
          while (table[next]) {
            size_t start = home(path[table[next] - 1].key);
            if (((hole - start) & mask) < ((next - start) & mask)) {
              table[hole] = table[next]; hole = next;
            }
            next = (next + 1) & mask;
          }
          table[hole] = 0;
        }
        path.pop_back();
        if (++visited % 16384 == 0 && progress) progress(context);
      }
      frames.pop_back();
      if (progress) progress(context);
    };
    for (size_t i = 0; i < count; i++) {
      while (!frames.empty() && frames.back().node != nodes[i].parent) pop();
      if (nodes[i].parent != SIZE_MAX && frames.empty()) return -EINVAL;
      bool internal = i + 1 < count && nodes[i + 1].parent == i;
      size_t begin = path.size();
      auto tree = static_cast<tree_type *>(nodes[i].index->tree);
      for (auto it = tree->begin(); it != tree->end(); ++it) {
        size_t at = find(it->first), previous = table[at];
        if (previous) {
          auto &older = path[previous - 1];
          uint32_t word = *older.word;
          if (!sidx_is_invalid(word)) {
            // Exclusive idle ownership permits updating the actual value word
            // without a second lookup or a per-entry lock/atomic bit operation.
            *older.word = word | SIDX_INVALID_BIT;
            --*older.owner->valid;
            marked_add(older.owner->index);
            if (report_entry_classes() && tombstone_at(older.owner->index, word))
              --older.owner->index->live_tombstones;
            ++*invalidated;
          }
        }
        // Marked entries and tombstones still shadow older copies. A leaf
        // only probes: it has no descendants that need a path-table entry.
        if (internal) {
          path.push_back({it->first, &it->second, &nodes[i], previous});
          table[at] = path.size();
        }
        if (++visited % 16384 == 0 && progress) progress(context);
      }
      if (internal) frames.push_back({i, begin});
      if (progress) progress(context);
    }
    while (!frames.empty()) pop();
  } catch (const bad_alloc &) {
    return -ENOMEM;
  } catch (const length_error &) {
    return -EOVERFLOW;
  }
  return 0;
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
#ifdef STELLAR_TESTING
  __atomic_fetch_sub(&marked_total, t->marked_count, __ATOMIC_RELAXED);
#endif
  delete static_cast<vector<uint64_t> *>(t->report_tombstones);
  btree_map<uint64_t, uint32_t> *b =
      static_cast<btree_map<uint64_t, uint32_t> *>(t->tree);
  delete b;
  free(t);
}
void subtree_all_free(subtree_t *t) {
#ifdef STELLAR_TESTING
  __atomic_fetch_sub(&marked_total, t->marked_count, __ATOMIC_RELAXED);
#endif
  delete static_cast<vector<uint64_t> *>(t->report_tombstones);
  btree_map<uint64_t, uint32_t> *b =
      static_cast<btree_map<uint64_t, uint32_t> *>(t->tree);
  b->erase(b->begin(), b->end());
  delete b;
  free(t);
}
}
