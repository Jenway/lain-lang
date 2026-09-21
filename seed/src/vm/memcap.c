/* 最小内存能力模型。契约与诊断码见 lainvm/memcap.h 的文件头。
 *
 * 纪律（同 space.c）：
 *   - 失败时表**不变**（先把参数验完，再改表）。
 *   - 槽的位置永不改变；代数用尽就不再用那个槽，宁可少一个槽。
 *   - 撤销访问权和释放存储是两个动作：`committed` 只在释放时归还。
 */
#include "lainvm/memcap.h"

#include <string.h>

void lainvm_memcap_init(LainVmMemTable *table, uint64_t generation_base) {
  uint32_t i;

  memset(table, 0, sizeof(*table));
  /* 代数的起点。调用方保证 `generation_base` 大于**同一块宿主内存上以前发过的
   * 所有代数** —— 这样同址重建的上下文发出来的代数一定更高，旧引用（槽号对得上、
   * 代数对不上）照样被拒，而句柄里不必再带表身份（8B 而不是 16B）。
   * 每个槽从基数起步、用一次 +1，所以句柄里的代数永远 ≥ 1（0 留给 no_handle）。 */
  for (i = 0; i < LAINVM_MEMCAP_MAX_OBJECTS; i++)
    table->objects[i].generation = (uint32_t)generation_base;
  for (i = 0; i < LAINVM_MEMCAP_MAX_CAPS; i++)
    table->caps[i].cap_generation = (uint32_t)generation_base;
}

LainVmMemHandle lainvm_memcap_no_handle(void) {
  LainVmMemHandle handle;
  handle.slot = 0;
  handle.generation = 0; /* 0 = 没有句柄（保留值） */
  return handle;
}

bool lainvm_memcap_handle_none(LainVmMemHandle handle) {
  return handle.generation == 0;
}

/* 对象句柄还指着那笔存储吗？ */
static const LainVmMemObject *object_of(const LainVmMemTable *table,
                                        LainVmMemHandle handle) {
  const LainVmMemObject *object;
  if (table == NULL) return NULL;
  if (handle.slot >= LAINVM_MEMCAP_MAX_OBJECTS) return NULL;
  object = &table->objects[handle.slot];
  if (!object->live) return NULL;
  if (object->generation != handle.generation) return NULL;
  return object;
}

/* 找一个能用的空槽：位置永不改变，代数用尽的槽不再重用。 */
static uint32_t free_slot(const uint32_t generations[], uint32_t count,
                          bool live[]) {
  uint32_t i;
  for (i = 0; i < count; i++) {
    if (live[i]) continue;
    if (generations[i] >= LAINVM_MEMCAP_MAX_GENERATION) continue;
    return i;
  }
  return count; /* 没空槽 */
}

LainVmMemHandle lainvm_memcap_object_add(LainVmMemTable *table, uintptr_t base,
                                         uint64_t size, uint64_t owner) {
  LainVmMemHandle handle;
  LainVmMemObject *object;
  uint32_t generations[LAINVM_MEMCAP_MAX_OBJECTS];
  bool live[LAINVM_MEMCAP_MAX_OBJECTS];
  uint32_t pick, i;

  handle = lainvm_memcap_no_handle();
  if (table == NULL) return handle;
  if (size == 0) return handle; /* 9201 */
  if (size - 1 > (uint64_t)UINTPTR_MAX - (uint64_t)base)
    return handle; /* 9201：base + size 不可表示 */

  for (i = 0; i < LAINVM_MEMCAP_MAX_OBJECTS; i++) {
    generations[i] = table->objects[i].generation;
    live[i] = table->objects[i].live;
  }
  pick = free_slot(generations, LAINVM_MEMCAP_MAX_OBJECTS, live);
  if (pick == LAINVM_MEMCAP_MAX_OBJECTS) return handle; /* 9200：表满 */

  object = &table->objects[pick];
  object->base = base;
  object->size = size;
  object->owner = owner;
  object->generation += 1;
  object->live = true;
  table->objects_live += 1;
  table->committed += size;

  handle.slot = pick;
  handle.generation = object->generation;
  return handle;
}

int32_t lainvm_memcap_object_release(LainVmMemTable *table,
                                     LainVmMemHandle object) {
  const LainVmMemObject *seen = object_of(table, object);
  LainVmMemObject *slot;
  if (seen == NULL) return 9213;
  slot = &table->objects[object.slot];
  table->committed -= slot->size;
  table->released += slot->size;
  table->objects_live -= 1;
  slot->live = false;
  /* 能力记录**不动**：释放存储不等于撤销访问权。持有者再去用会被判
   * 「对象已不存活」（9211）—— 这正是要留下的证据。 */
  return 0;
}

int32_t lainvm_memcap_grant(LainVmMemTable *table, LainVmMemHandle object,
                            uint64_t offset, uint64_t length, uint32_t rights,
                            uint64_t owner, LainVmMemHandle *out) {
  const LainVmMemObject *target = object_of(table, object);
  LainVmMemCap *cap;
  uint32_t generations[LAINVM_MEMCAP_MAX_CAPS];
  bool live[LAINVM_MEMCAP_MAX_CAPS];
  uint32_t pick, i;
  uint32_t memory_rights = rights & (LAINVM_MEM_READ | LAINVM_MEM_WRITE);

  if (out != NULL) *out = lainvm_memcap_no_handle();
  if (out == NULL || table == NULL) return 9201;
  if (target == NULL) return 9213; /* 对象句柄无效 / 已释放 */
  /* 授权范围必须整个落在对象里（先验 offset，再减法比剩余）。 */
  if (length == 0 || offset > target->size) return 9203;
  if (length > target->size - offset) return 9203;
  /* CALL 不在这一层（间接调用单独判），所以只有 READ/WRITE 的授权才有效。 */
  if (memory_rights == 0) return 9204;

  for (i = 0; i < LAINVM_MEMCAP_MAX_CAPS; i++) {
    generations[i] = table->caps[i].cap_generation;
    live[i] = table->caps[i].live;
  }
  pick = free_slot(generations, LAINVM_MEMCAP_MAX_CAPS, live);
  if (pick == LAINVM_MEMCAP_MAX_CAPS) return 9202; /* 能力表满 */

  cap = &table->caps[pick];
  cap->object = object.slot;
  cap->generation = target->generation;
  cap->offset = offset;
  cap->length = length;
  cap->rights = memory_rights;
  cap->owner = owner;
  cap->cap_generation += 1;
  cap->live = true;
  table->caps_live += 1;

  if (out != NULL) {
    out->slot = pick;
    out->generation = cap->cap_generation;
  }
  return 0;
}

int32_t lainvm_memcap_revoke_owner(LainVmMemTable *table, uint64_t owner,
                                   uint32_t *revoked) {
  uint32_t i, count = 0;

  if (revoked != NULL) *revoked = 0;
  if (table == NULL) return 9213;
  /* 0 是保留值（模块 / 装载器的能力）。按 0 扫表撤销曾经会连它们一起清掉。 */
  if (owner == 0) return 9213;
  for (i = 0; i < LAINVM_MEMCAP_MAX_CAPS; i++) {
    if (!table->caps[i].live) continue;
    if (table->caps[i].owner != owner) continue;
    table->caps[i].live = false;
    table->caps_live -= 1;
    count += 1;
  }
  if (revoked != NULL) *revoked = count;
  return 0;
}

void lainvm_memcap_end_owner(LainVmMemTable *table, uint64_t owner,
                             uint32_t *revoked, uint32_t *released) {
  uint32_t i, rv = 0, rl = 0;

  if (table == NULL) {
    if (revoked != NULL) *revoked = 0;
    if (released != NULL) *released = 0;
    return;
  }
  if (owner != 0) {
    for (i = 0; i < LAINVM_MEMCAP_MAX_CAPS; i++) {
      if (!table->caps[i].live || table->caps[i].owner != owner) continue;
      table->caps[i].live = false;
      table->caps_live -= 1;
      rv += 1;
    }
    for (i = 0; i < LAINVM_MEMCAP_MAX_OBJECTS; i++) {
      LainVmMemObject *object = &table->objects[i];
      if (!object->live || object->owner != owner) continue;
      table->committed -= object->size;
      table->released += object->size;
      object->live = false;
      table->objects_live -= 1;
      rl += 1;
    }
  }
  if (revoked != NULL) *revoked = rv;
  if (released != NULL) *released = rl;
}

void lainvm_memcap_end_all(LainVmMemTable *table, uint32_t *revoked,
                           uint32_t *released) {
  uint32_t i, rv = 0, rl = 0;

  if (table == NULL) {
    if (revoked != NULL) *revoked = 0;
    if (released != NULL) *released = 0;
    return;
  }
  for (i = 0; i < LAINVM_MEMCAP_MAX_CAPS; i++) {
    if (!table->caps[i].live) continue;
    table->caps[i].live = false;
    table->caps_live -= 1;
    rv += 1;
  }
  for (i = 0; i < LAINVM_MEMCAP_MAX_OBJECTS; i++) {
    LainVmMemObject *object = &table->objects[i];
    if (!object->live) continue;
    table->committed -= object->size;
    table->released += object->size;
    object->live = false;
    table->objects_live -= 1;
    rl += 1;
  }
  if (revoked != NULL) *revoked = rv;
  if (released != NULL) *released = rl;
}

/* 文档 §3「访问」五条，按顺序。返回 0 或稳定拒绝码。
 * 句柄不带表身份之后，同址重建的上下文靠**代数基数**检出（9208）—— 9205 已废弃。 */
static int32_t resolve_code(const LainVmMemTable *table,
                            const LainVmSpace *space, LainVmMemRef ref,
                            uint32_t need, uint64_t length, uint64_t context,
                            uintptr_t *out_base) {
  const LainVmMemCap *cap;
  const LainVmMemObject *object;
  uint64_t absolute;
  uintptr_t address;

  if (table == NULL) return 9207;
  /* 1) 引用由允许的途径取得：所属上下文对得上。 */
  if (ref.cap.slot >= LAINVM_MEMCAP_MAX_CAPS) return 9207;
  cap = &table->caps[ref.cap.slot];
  if (!cap->live) return 9207;
  if (cap->owner != context) return 9206;
  /* 2) 对象仍活动，引用代数与对象代数一致。 */
  if (cap->cap_generation != ref.cap.generation) return 9208;
  if (cap->object >= LAINVM_MEMCAP_MAX_OBJECTS) return 9208;
  object = &table->objects[cap->object];
  if (object->generation != cap->generation) return 9208;
  if (!object->live) return 9211;
  /* 3) 操作要求的权限存在。 */
  if (need != 0 && (cap->rights & need) != need) return 9209;
  /* 4) 完整访问区间同时落在能力授权范围与对象边界内。 */
  if (ref.offset > cap->length) return 9210;
  if (length > cap->length - ref.offset) return 9210;
  absolute = cap->offset + ref.offset;
  if (absolute > object->size) return 9210;
  if (length > object->size - absolute) return 9210;
  address = object->base + absolute;
  if (address < object->base) return 9210;
  /* 5) 当前 VSpace 对这段存储也有授权。 */
  if (space == NULL || !lainvm_space_check(space, address, length, need))
    return 9212;
  if (out_base != NULL) *out_base = address;
  return 0;
}

int lainvm_memcap_resolve(const LainVmMemTable *table, const LainVmSpace *space,
                          LainVmMemRef ref, uint32_t need, uint64_t length,
                          uint64_t context, uintptr_t *out_base, int32_t *code) {
  int32_t result = resolve_code(table, space, ref, need, length, context,
                                out_base);
  if (code != NULL) *code = result;
  return (int)result;
}

int lainvm_memcap_read(const LainVmMemTable *table, const LainVmSpace *space,
                       LainVmMemRef ref, uint64_t context, uint64_t length,
                       void *out, int32_t *code) {
  uintptr_t base = 0;
  int32_t result = resolve_code(table, space, ref, LAINVM_MEM_READ, length,
                                context, &base);
  if (code != NULL) *code = result;
  if (result != 0) return (int)result;
  if (out != NULL && length != 0)
    memcpy(out, (const void *)base, (size_t)length);
  return 0;
}

int lainvm_memcap_write(const LainVmMemTable *table, const LainVmSpace *space,
                        LainVmMemRef ref, uint64_t context, uint64_t length,
                        const void *in, int32_t *code) {
  uintptr_t base = 0;
  int32_t result = resolve_code(table, space, ref, LAINVM_MEM_WRITE, length,
                                context, &base);
  if (code != NULL) *code = result;
  if (result != 0) return (int)result;
  if (in != NULL && length != 0) memcpy((void *)base, in, (size_t)length);
  return 0;
}

int lainvm_memcap_host_write(const LainVmMemTable *table,
                             const LainVmSpace *space, LainVmMemRef ref,
                             uint64_t context, uint64_t length, const void *in,
                             uint64_t *side_effects, int32_t *code) {
  uintptr_t base = 0;
  int32_t result = resolve_code(table, space, ref, LAINVM_MEM_WRITE, length,
                                context, &base);
  if (code != NULL) *code = result;
  if (result != 0) return (int)result; /* 副作用一点都没发生 */
  if (side_effects != NULL) *side_effects += 1;
  if (in != NULL && length != 0) memcpy((void *)base, in, (size_t)length);
  return 0;
}

uint64_t lainvm_memcap_ref_to_int(LainVmMemRef ref) {
  return ((uint64_t)ref.cap.generation << 32) | (uint64_t)ref.cap.slot;
}

LainVmMemRef lainvm_memcap_int_to_ref(uint64_t bits, uint64_t offset) {
  LainVmMemRef ref;
  ref.cap.generation = (uint32_t)(bits >> 32);
  ref.cap.slot = (uint32_t)(bits & 0xFFFFFFFFu);
  ref.offset = offset;
  return ref;
}
