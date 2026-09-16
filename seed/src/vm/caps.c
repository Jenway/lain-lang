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
  LainVmCapEntry entries[LAINVM_CAPS_MAX];
  uint32_t count;
};

LainVmCaps *lainvm_caps_new(void) {
  return (LainVmCaps *)calloc(1, sizeof(LainVmCaps));
}

void lainvm_caps_free(LainVmCaps *caps) { free(caps); }

int lainvm_caps_add(LainVmCaps *caps, const char *link_name, LainVmCapKind kind,
                    LainVmHostFn fn) {
  if (!caps || !link_name || !link_name[0]) return 1;
  if (caps->count >= LAINVM_CAPS_MAX) return 2;
  if (lainvm_caps_find(caps, link_name)) return 3;
  if (kind == LAINVM_CAP_FUNCTION && !fn) return 4;
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
