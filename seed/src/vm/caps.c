/* lain/vm/caps.h 的实现。
 *
 * 只在装载 / admit 时查（要 strcmp）；运行期走 TCB 里那份解析好的数组。
 * 登记名会被复制一份由表持有；context 只是引用，表不拥有、不释放。 */
#include "lain/vm/caps.h"

#include <stdlib.h>
#include <string.h>

struct LainVmCapEntry {
  char *link_name; /* 表持有的副本：冻结后不受调用方改字符串影响 */
  LainVmCapKind kind;
  LainVmHostFn fn;
  void *context; /* 可信注册者绑定的宿主上下文；表不拥有 */
};

struct LainVmCaps {
  /* 按需增长：一个驱动要登记多少项是它自己的事。这里曾经是
   * entries[64]，而旧宿主登记了 92 项。 */
  LainVmCapEntry *entries;
  uint32_t count;
  uint32_t cap;
  bool frozen; /* 冻结后禁止新增/扩容；绑定的 fn/context 不再变 */
};

LainVmCaps *lainvm_caps_new(void) {
  return (LainVmCaps *)calloc(1, sizeof(LainVmCaps));
}

void lainvm_caps_free(LainVmCaps *caps) {
  uint32_t i;
  if (!caps) return;
  for (i = 0; i < caps->count; i++) free(caps->entries[i].link_name);
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
                    LainVmHostFn fn, void *context) {
  char *copy;
  size_t length;
  /* 冻结检查在**任何扩容、写入之前**：冻结表的 count、容量、项地址、
   * fn、context 全部保持不变。 */
  if (!caps) return LAINVM_CAPS_ERR_ARG;
  if (caps->frozen) return LAINVM_CAPS_ERR_FROZEN;
  if (!link_name || !link_name[0]) return LAINVM_CAPS_ERR_ARG;
  if (lainvm_caps_find(caps, link_name)) return LAINVM_CAPS_ERR_DUPLICATE;
  if (kind == LAINVM_CAP_FUNCTION && !fn) return LAINVM_CAPS_ERR_NO_FN;
  length = strlen(link_name) + 1u;
  copy = (char *)malloc(length);
  if (!copy) return LAINVM_CAPS_ERR_OOM; /* 分配失败不留下半项 */
  if (caps->count >= caps->cap && !grow_caps(caps)) {
    free(copy);
    return LAINVM_CAPS_ERR_OOM;
  }
  memcpy(copy, link_name, length);
  caps->entries[caps->count].link_name = copy;
  caps->entries[caps->count].kind = kind;
  caps->entries[caps->count].fn = fn;
  caps->entries[caps->count].context = context;
  caps->count++;
  return LAINVM_CAPS_OK;
}

int lainvm_caps_freeze(LainVmCaps *caps) {
  if (!caps) return LAINVM_CAPS_ERR_ARG;
  caps->frozen = true; /* 幂等：再次 freeze 仍成功，且不可解冻 */
  return LAINVM_CAPS_OK;
}

bool lainvm_caps_is_frozen(const LainVmCaps *caps) {
  return caps ? caps->frozen : false;
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

void *lainvm_cap_context(const LainVmCapEntry *entry) {
  return entry ? entry->context : NULL;
}

uint32_t lainvm_caps_count(const LainVmCaps *caps) {
  return caps ? caps->count : 0;
}
