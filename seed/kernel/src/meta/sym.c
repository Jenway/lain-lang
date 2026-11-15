/* lain/meta/sym.h 的实现：固定容量、不复用、不扩容的符号驻留表。 */
#include "lain/meta/sym.h"

#include <stdlib.h>
#include <string.h>

struct LainMetaSymTab {
  unsigned char *pool;
  uint32_t *offsets; /* 按 sym 下标；下标 0 恒为 0 */
  uint32_t *lengths;
  uint32_t *buckets; /* 开放寻址，0 = 空 */
  uint32_t bucket_count;
  uint32_t count;
  uint32_t bytes_used;
  uint32_t capacity;
  uint32_t bytes_capacity;
  LainVmQuota *quota;
  uint64_t charged;
};

static uint32_t hash_bytes(const void *text, uint32_t length) {
  const unsigned char *p = (const unsigned char *)text;
  uint32_t h = UINT32_C(2166136261);
  uint32_t i;
  for (i = 0; i < length; i++) {
    h ^= p[i];
    h *= UINT32_C(16777619);
  }
  return h ? h : 1u;
}

LainMetaSymTab *lainmeta_sym_new(uint32_t capacity, uint32_t bytes_capacity,
                                 LainVmQuota *quota) {
  LainMetaSymTab *tab;
  uint64_t buckets = 2u;
  uint64_t storage;
  if (!capacity || !bytes_capacity || capacity > LAIN_META_SYM_MAX) return NULL;
  while (buckets < (uint64_t)capacity * 2u) buckets *= 2u;
  storage = (uint64_t)bytes_capacity + (uint64_t)(capacity + 1u) * 4u * 2u +
            buckets * 4u + sizeof(*tab);
  if (lainvm_quota_charge(quota, storage) != 0) return NULL;
  tab = (LainMetaSymTab *)calloc(1, sizeof(*tab));
  if (tab) {
    tab->pool = (unsigned char *)calloc(1, bytes_capacity);
    tab->offsets = (uint32_t *)calloc((size_t)capacity + 1u, sizeof(uint32_t));
    tab->lengths = (uint32_t *)calloc((size_t)capacity + 1u, sizeof(uint32_t));
    tab->buckets = (uint32_t *)calloc((size_t)buckets, sizeof(uint32_t));
  }
  if (!tab || !tab->pool || !tab->offsets || !tab->lengths || !tab->buckets) {
    if (tab) {
      free(tab->pool); free(tab->offsets); free(tab->lengths);
      free(tab->buckets);
      free(tab);
    }
    (void)lainvm_quota_release(quota, storage);
    return NULL;
  }
  tab->bucket_count = (uint32_t)buckets;
  tab->capacity = capacity;
  tab->bytes_capacity = bytes_capacity;
  tab->quota = quota;
  tab->charged = storage;
  return tab;
}

void lainmeta_sym_free(LainMetaSymTab *tab) {
  if (!tab) return;
  free(tab->pool);
  free(tab->offsets);
  free(tab->lengths);
  free(tab->buckets);
  (void)lainvm_quota_release(tab->quota, tab->charged);
  free(tab);
}

LainMetaSymId lainmeta_sym_find(const LainMetaSymTab *tab, const void *text,
                                uint32_t length) {
  uint32_t at;
  if (!tab || !text || !length) return LAIN_META_SYM_NONE;
  at = hash_bytes(text, length) & (tab->bucket_count - 1u);
  for (;;) {
    uint32_t id = tab->buckets[at];
    if (!id) return LAIN_META_SYM_NONE;
    if (tab->lengths[id] == length &&
        memcmp(tab->pool + tab->offsets[id], text, length) == 0)
      return id;
    at = (at + 1u) & (tab->bucket_count - 1u);
  }
}

bool lainmeta_sym_bytes(const LainMetaSymTab *tab, LainMetaSymId sym,
                        const unsigned char **bytes, uint32_t *length) {
  if (!tab || !bytes || !length) return false;
  if (sym == LAIN_META_SYM_NONE || sym > tab->count) return false;
  *bytes = tab->pool + tab->offsets[sym];
  *length = tab->lengths[sym];
  return true;
}

LainMetaSymId lainmeta_sym_intern(LainMetaSymTab *tab, const void *text,
                                  uint32_t length) {
  LainMetaSymId found;
  uint32_t at;
  uint32_t id;
  if (!tab || !text || !length) return LAIN_META_SYM_NONE;
  found = lainmeta_sym_find(tab, text, length);
  if (found) return found;
  if (tab->count == tab->capacity) return LAIN_META_SYM_NONE;
  if (length > tab->bytes_capacity - tab->bytes_used) return LAIN_META_SYM_NONE;
  id = ++tab->count;
  tab->offsets[id] = tab->bytes_used;
  tab->lengths[id] = length;
  memcpy(tab->pool + tab->bytes_used, text, length);
  tab->bytes_used += length;
  at = hash_bytes(text, length) & (tab->bucket_count - 1u);
  while (tab->buckets[at]) at = (at + 1u) & (tab->bucket_count - 1u);
  tab->buckets[at] = id;
  return id;
}

uint32_t lainmeta_sym_count(const LainMetaSymTab *tab) {
  return tab ? tab->count : 0;
}

const unsigned char *lainmeta_sym_pool(const LainMetaSymTab *tab,
                                       uint32_t *capacity, uint32_t *used) {
  if (!tab) return NULL;
  if (capacity) *capacity = tab->bytes_capacity;
  if (used) *used = tab->bytes_used;
  return tab->pool;
}
