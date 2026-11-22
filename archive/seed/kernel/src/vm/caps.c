/* lain/vm/caps.h 的实现。
 *
 * 只在装载 / admit 时查（要 strcmp）；运行期走 TCB 里那份解析好的数组。
 * 表和项存储都由调用方给，本层一次分配都不做；名字复制进项自带的缓冲。
 * context 只是引用，表不拥有、不释放。 */
#include "lain/vm/caps.h"

#include <string.h>

void lainvm_caps_init(LainVmCaps *caps, LainVmCapEntry *entries,
                      uint32_t capacity) {
  if (!caps) return;
  caps->entries = entries;
  caps->capacity = entries ? capacity : 0u;
  caps->count = 0;
  caps->frozen = false;
}

int lainvm_caps_add(LainVmCaps *caps, const char *link_name, LainVmCapKind kind,
                    LainVmHostFn fn, void *context) {
  LainVmCapEntry *entry;
  size_t length;
  /* 冻结检查在**任何写入之前**：冻结表的 count、容量、项地址、fn、context
   * 全部保持不变。 */
  if (!caps || !caps->entries || caps->capacity == 0u)
    return LAINVM_CAPS_ERR_ARG;
  if (caps->frozen) return LAINVM_CAPS_ERR_FROZEN;
  if (!link_name || !link_name[0]) return LAINVM_CAPS_ERR_ARG;
  length = strlen(link_name) + 1u;
  if (length > (size_t)LAINVM_CAPS_NAME_MAX) return LAINVM_CAPS_ERR_NAME;
  if (lainvm_caps_find(caps, link_name)) return LAINVM_CAPS_ERR_DUPLICATE;
  if (kind == LAINVM_CAP_FUNCTION && !fn) return LAINVM_CAPS_ERR_NO_FN;
  /* 满了就是拒绝，没有扩容：容量是调用方初始化时给的。 */
  if (caps->count >= caps->capacity) return LAINVM_CAPS_ERR_FULL;
  entry = &caps->entries[caps->count];
  memcpy(entry->link_name, link_name, length);
  entry->kind = kind;
  entry->fn = fn;
  entry->context = context;
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
