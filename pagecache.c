#include "headers.h"

void page_cache_init(struct pagecache *p) {
  declare_timer;
  start_timer {
    printf("#Reserving memory for page cache...\n");
    p->cached_data =
        aligned_alloc(PAGE_SIZE, cfg.page_cache_size / get_nb_workers());
    assert(p->cached_data);  // If it fails here, it's probably because page
                             // cache size is bigger than RAM -- see options.h
    memset(p->cached_data, 0, cfg.page_cache_size / get_nb_workers());
  }
  stop_timer("Page cache initialization");

  p->hash_to_page = tree_create();
  p->used_pages =
      calloc(MAX_PAGE_CACHE / get_nb_workers(), sizeof(*p->used_pages));
  p->used_page_size = 0;
  p->oldest_page = NULL;
  p->newest_page = NULL;
}

struct lru *add_page_in_lru(struct pagecache *p, void *page, uint64_t hash) {
  struct lru *me = &p->used_pages[p->used_page_size];
  me->hash = hash;
  me->page = page;
  if (!p->oldest_page) p->oldest_page = me;
  me->prev = NULL;
  me->next = p->newest_page;
  if (p->newest_page) p->newest_page->prev = me;
  p->newest_page = me;
  return me;
}

void bump_page_in_lru(struct pagecache *p, struct lru *me, uint64_t hash) {
  assert(me->hash == hash);
  if (me == p->newest_page) return;
  if (p->oldest_page == me && me->prev) p->oldest_page = me->prev;
  if (me->prev) me->prev->next = me->next;
  if (me->next) me->next->prev = me->prev;
  me->prev = NULL;
  me->next = p->newest_page;
  p->newest_page->prev = me;
  p->newest_page = me;
}

/*
 * Get a page from the page cache.
 * *page will be set to the address in the page cache
 * @return 1 if the page already contains the right data, 0 otherwise.
 */
int get_page(struct pagecache *p, uint64_t hash, void **page,
             struct lru **lru) {
  void *dst;
  struct lru *lru_entry;
  maybe_unused pagecache_entry_t tmp_entry;
  maybe_unused pagecache_entry_t *old_entry = NULL;

  // Is the page already cached?
  pagecache_entry_t *e = tree_lookup(p->hash_to_page, hash);
  if (e) {
    dst = e->page;
    lru_entry = e->lru;
    if (lru_entry->hash != hash)
      die("LRU wierdness %lu vs %lu\n", lru_entry->hash, hash);
    bump_page_in_lru(p, lru_entry, hash);
    *page = dst;
    *lru = lru_entry;
    return 1;
  }

  // Otherwise allocate a new page, either a free one, or reuse the oldest
  if (p->used_page_size < MAX_PAGE_CACHE / get_nb_workers()) {
    dst = &p->cached_data[PAGE_SIZE * p->used_page_size];
    lru_entry = add_page_in_lru(p, dst, hash);
    p->used_page_size++;
  } else {
    lru_entry = p->oldest_page;
    dst = p->oldest_page->page;
    // printf("REPLACE PAGE %lu %lu\n", lru_entry->hash, hash);

    tree_delete(p->hash_to_page, p->oldest_page->hash, &old_entry);

    lru_entry->hash = hash;
    lru_entry->page = dst;
    bump_page_in_lru(p, lru_entry, hash);
  }

  // Remember that the page cache now stores this hash
  tree_insert(p->hash_to_page, hash, old_entry, dst, lru_entry);

  lru_entry->contains_data = 0;
  lru_entry->dirty = 0;  // should already be equal to 0, but we never know
  *page = dst;
  *lru = lru_entry;

  return 0;
}

//static uint64_t extract_page_num_from_hash(uint64_t hash) {
//  uint64_t mask = (1ULL << 40) - 1;  // Create a mask for the lower 40 bits
//  return hash & mask;                // Extract the lower 40 bits
//}

int get_page_with_slab(struct pagecache *p, uint64_t hash, void **page,
                       struct lru **lru, struct slab *s) {
  void *dst;
  struct lru *lru_entry;
  maybe_unused pagecache_entry_t tmp_entry;
  maybe_unused pagecache_entry_t *old_entry = NULL;
  //uint64_t page_num = extract_page_num_from_hash(hash);

  // Is the page already cached?
  pagecache_entry_t *e = tree_lookup(p->hash_to_page, hash);
  if (e) {
    dst = e->page;
    lru_entry = e->lru;
    if (lru_entry->hash != hash)
      die("LRU wierdness %lu vs %lu\n", lru_entry->hash, hash);
    bump_page_in_lru(p, lru_entry, hash);
    //if (lru_entry->hot_page_checked == 0) {
    //  __sync_add_and_fetch(&s->hot_pages, 1);
    //  lru_entry->hot_page_checked = 1;
    //}
    *page = dst;
    *lru = lru_entry;
    return 1;
  }

  // Otherwise allocate a new page, either a free one, or reuse the oldest
  if (p->used_page_size < MAX_PAGE_CACHE / get_nb_workers()) {
    dst = &p->cached_data[PAGE_SIZE * p->used_page_size];
    lru_entry = add_page_in_lru(p, dst, hash);
    p->used_page_size++;
  } else {
    lru_entry = p->oldest_page;
    dst = p->oldest_page->page;
    // printf("REPLACE PAGE %lu %lu\n", lru_entry->hash, hash);

    //if (lru_entry->hot_page_checked) {
    //  __sync_fetch_and_sub(&((struct slab *)lru_entry->slab)->hot_pages, 1);
    //}
    tree_delete(p->hash_to_page, p->oldest_page->hash, &old_entry);

    lru_entry->hash = hash;
    lru_entry->page = dst;
    bump_page_in_lru(p, lru_entry, hash);
  }

  //lru_entry->hot_page_checked = 0;
  lru_entry->slab = s;

  // Remember that the page cache now stores this hash
  tree_insert(p->hash_to_page, hash, old_entry, dst, lru_entry);

  lru_entry->contains_data = 0;
  lru_entry->dirty = 0;  // should already be equal to 0, but we never know
  *page = dst;
  *lru = lru_entry;

  return 0;
}

int get_page_for_file(struct pagecache *p, uint64_t hash, uint64_t size,
                      void **page, struct lru **lru) {
  void *dst;
  struct lru *lru_entry;
  maybe_unused pagecache_entry_t tmp_entry;
  maybe_unused pagecache_entry_t *old_entry = NULL;

  // Is the page already cached?
  pagecache_entry_t *e = tree_lookup(p->hash_to_page, hash);
  if (e) {
    dst = e->page;
    lru_entry = e->lru;
    if (lru_entry->hash != hash)
      die("LRU wierdness %lu vs %lu\n", lru_entry->hash, hash);
    bump_page_in_lru(p, lru_entry, hash);
    *page = dst;
    *lru = lru_entry;
    return 1;
  }

  // Otherwise allocate a new page, either a free one, or reuse the oldest
  if (p->used_page_size < MAX_PAGE_CACHE / get_nb_workers()) {
    dst = &p->cached_data[PAGE_SIZE * p->used_page_size];
    lru_entry = add_page_in_lru(p, dst, hash);
    p->used_page_size += 16384;
  } else {
    lru_entry = p->oldest_page;
    dst = p->oldest_page->page;
    // printf("REPLACE FILE %lu %lu\n", lru_entry->hash, hash);

    tree_delete(p->hash_to_page, p->oldest_page->hash, &old_entry);

    lru_entry->hash = hash;
    lru_entry->page = dst;
    bump_page_in_lru(p, lru_entry, hash);
  }

  // Remember that the page cache now stores this hash
  tree_insert(p->hash_to_page, hash, old_entry, dst, lru_entry);

  lru_entry->contains_data = 0;
  lru_entry->dirty = 0;  // should already be equal to 0, but we never know
  *page = dst;
  *lru = lru_entry;

  return 0;
}
