/* lainvm/caps.h 的实现。
 *
 * 只在装载 / admit 时查（要 strcmp）；运行期走 TCB 里那份解析好的数组。 */
#include "lainvm/caps.h"

#include <stdlib.h>
#include <string.h>

struct LainVmCapEntry {
  const char *link_name;
  LainVmCapKind kind;
  LainVmHostFn fn;
};

struct LainVmCaps {
  /* 按需增长：一个驱动要登记多少项是它自己的事。这里曾经是
   * entries[64]，而旧宿主登记了 92 项。 */
  LainVmCapEntry *entries;
  uint32_t count;
  uint32_t cap;
};

LainVmCaps *lainvm_caps_new(void) {
  return (LainVmCaps *)calloc(1, sizeof(LainVmCaps));
}

void lainvm_caps_free(LainVmCaps *caps) {
  if (!caps) return;
  free(caps->entries);
  free(caps);
}

static bool grow_caps(LainVmCaps *caps) {
  uint32_t next = caps->cap ? caps->cap * 2u : 32u;
  LainVmCapEntry *grown =
      (LainVmCapEntry *)realloc(caps->entries, sizeof(LainVmCapEntry) * (size_t)next);
  if (!grown) return false;
  caps->entries = grown;
  caps->cap = next;
  return true;
}

int lainvm_caps_add(LainVmCaps *caps, const char *link_name, LainVmCapKind kind,
                    LainVmHostFn fn) {
  if (!caps || !link_name || !link_name[0]) return 1;
  if (lainvm_caps_find(caps, link_name)) return 3;
  if (kind == LAINVM_CAP_FUNCTION && !fn) return 4;
  if (caps->count >= caps->cap && !grow_caps(caps)) return 2;
  caps->entries[caps->count].link_name = link_name;
  caps->entries[caps->count].kind = kind;
  caps->entries[caps->count].fn = fn;
  caps->count++;
  return 0;
}

const LainVmCapEntry *lainvm_caps_find(const LainVmCaps *caps,
                                       const char *link_name) {
  uint32_t i;
  if (!caps || !link_name) return NULL;
  for (i = 0; i < caps->count; i++) {
    if (strcmp(caps->entries[i].link_name, link_name) == 0)
      return &caps->entries[i];
  }
  return NULL;
}

LainVmCapKind lainvm_cap_kind(const LainVmCapEntry *entry) {
  return entry ? entry->kind : LAINVM_CAP_FUNCTION;
}

LainVmHostFn lainvm_cap_fn(const LainVmCapEntry *entry) {
  return entry ? entry->fn : NULL;
}

uint32_t lainvm_caps_count(const LainVmCaps *caps) {
  return caps ? caps->count : 0;
}
