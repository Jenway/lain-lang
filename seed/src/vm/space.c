/* lainvm/space.h 的实现。
 *
 * 三条不变量，改这个文件时先读这几句：
 *   1. `slots` 里的位置**永不改变**（句柄的身份靠它 + 代数）。所有重排只发生在
 *      `by_base` 这条索引上。谁要是再来一次「插入之后按 base 找回来」，
 *      就是把身份写回位置，这个设计立刻作废。
 *   2. 撤销只认句柄。没有「按 owner 扫表删」这条路——它会让 owner=0 的
 *      装载器区段被一次误调用清掉，也表达不了「这块是我派生给你的那一块」。
 *   3. **三件事分开**：撤销授权（unmap / free）、释放底层存储（free）、
 *      归还 quota（free）。每个接口只做自己名字里写的那几件，不再有一个模糊的
 *      `remove` 同时承担三个含义。
 */
#include "lainvm/space.h"

#include <stdlib.h>
#include <string.h>

void lainvm_space_init(LainVmSpace *space) {
  if (!space) return;
  memset(space, 0, sizeof(*space));
}

LainVmRegionHandle lainvm_space_no_handle(void) {
  LainVmRegionHandle handle;
  handle.space = 0;
  handle.slot = LAINVM_SPACE_NO_SLOT;
  handle.generation = 0;
  return handle;
}

bool lainvm_space_handle_none(LainVmRegionHandle handle) {
  return handle.slot == LAINVM_SPACE_NO_SLOT || handle.space == 0;
}

const LainVmRegion *lainvm_space_slot(const LainVmSpace *space,
                                      LainVmRegionHandle handle) {
  const LainVmRegion *region;
  if (!space) return NULL;
  if (handle.slot >= LAINVM_SPACE_MAX_REGIONS) return NULL;
  /* 跨空间：句柄里存的是空间对象自己的地址。 */
  if (handle.space != (uintptr_t)space) return NULL;
  region = &space->slots[handle.slot];
  if (!region->alive) return NULL;
  if (region->generation != handle.generation) return NULL;
  return region;
}

/* 占用与重叠看 **capacity**：登记进去的是整块存储，不只是当前窗口。 */
static bool overlaps(const LainVmRegion *region, uintptr_t base, uint64_t capacity) {
  uintptr_t end = region->base + (uintptr_t)region->capacity;
  uintptr_t want_end = base + (uintptr_t)capacity;
  return base < end && region->base < want_end;
}

/* 把槽号插进 by_base，保持按 base 升序。
 * 调用方已经把 live_count 加过了，所以新元素最后能待的位置是 live_count - 1。
 * （差这一位会同时踩两个坑：最后一个区段对 find 不可见，且到第 64 段时越界写。） */
static void index_insert(LainVmSpace *space, uint32_t slot) {
  uintptr_t key = space->slots[slot].base;
  uint32_t i = space->live_count - 1;
  while (i > 0 && space->slots[space->by_base[i - 1]].base > key) {
    space->by_base[i] = space->by_base[i - 1];
    i--;
  }
  space->by_base[i] = slot;
}

/* 把槽号从 by_base 里摘掉（后面的往前挪；slots 不动）。 */
static void index_remove(LainVmSpace *space, uint32_t slot) {
  uint32_t i = 0;
  while (i < space->live_count && space->by_base[i] != slot) i++;
  if (i >= space->live_count) return;
  for (; i + 1 < space->live_count; i++) space->by_base[i] = space->by_base[i + 1];
}

/* 句柄 → 可写槽。`lainvm_space_slot` 返回的是 const 视图（查表语义），
 * 这里要改的就是它指的那个槽，所以去掉 const —— 换的不是类型，是访问方式。 */
static LainVmRegion *slot_mut(LainVmSpace *space, LainVmRegionHandle handle) {
  return (LainVmRegion *)lainvm_space_slot(space, handle);
}

/* 找一个能用的空槽：空的，而且代数还没到顶（到顶就不再重用——宁可少一个槽，
 * 也不让撤销过的旧句柄"活过来"指到新对象）。 */
static uint32_t free_slot(const LainVmSpace *space) {
  uint32_t i;
  for (i = 0; i < LAINVM_SPACE_MAX_REGIONS; i++) {
    if (!space->slots[i].alive && space->slots[i].generation != 0xFFFFFFFFu)
      return i;
  }
  return LAINVM_SPACE_NO_SLOT;
}

static bool any_overlap(const LainVmSpace *space, uintptr_t base,
                        uint64_t capacity) {
  uint32_t i;
  for (i = 0; i < space->live_count; i++) {
    if (overlaps(&space->slots[space->by_base[i]], base, capacity)) return true;
  }
  return false;
}

/* 真正登记进表。调用方负责在此之前把要扣的账扣好、在失败之后回滚。
 * 失败（容量非法 / 上界不可表示 / 重叠 / 没空槽）返回 no_handle 且表不变。 */
static LainVmRegionHandle register_region(LainVmSpace *space, uintptr_t base,
                                          uint64_t capacity, uint64_t accessible,
                                          uint32_t rights, uint64_t owner,
                                          uint32_t kind, uintptr_t raw,
                                          LainVmQuota *quota, uint64_t charged) {
  LainVmRegionHandle handle = lainvm_space_no_handle();
  uint32_t slot;

  if (!space || capacity == 0) return handle;
  if (accessible > capacity) return handle;
  if (base + (uintptr_t)capacity < base) return handle;
  if (any_overlap(space, base, capacity)) return handle;
  slot = free_slot(space);
  if (slot == LAINVM_SPACE_NO_SLOT) return handle;

  {
    LainVmRegion *region = &space->slots[slot];
    region->base = base;
    region->capacity = capacity;
    region->accessible = accessible;
    region->rights = rights;
    region->owner = owner;
    region->backing_kind = kind;
    region->borrow_count = 0;
    region->quota = quota;
    region->charged = charged;
    region->raw = raw;
    region->generation += 1; /* 从 1 开始：全零句柄永远不会撞上活槽 */
    region->alive = true;
  }
  space->live_count++;
  index_insert(space, slot);

  handle.space = (uintptr_t)space;
  handle.slot = slot;
  handle.generation = space->slots[slot].generation;
  return handle;
}

/* 真正从表里摘掉。**不动存储、不动账**：那两件事由调用方按语义决定。 */
static bool revoke_region(LainVmSpace *space, LainVmRegionHandle handle) {
  if (!lainvm_space_slot(space, handle)) return false;
  index_remove(space, handle.slot);
  space->live_count--;
  space->slots[handle.slot].alive = false;
  /* base/capacity 留着不清：诊断时还能看出来这个槽曾经是谁的。 */
  return true;
}

/* 底层分配。alignment <= 16 时直接用 ask（宿主至少给 16 字节对齐）；
 * 更大就多要 alignment 字节再对齐，并把原始指针交回去（free 必须用它）。
 * `charge_out` 是**实际拿到的那一块**的字节数——计入 quota 的就是它。 */
static void *alloc_block(uint64_t capacity, uint64_t alignment,
                         uintptr_t *base_out, uintptr_t *raw_out,
                         uint64_t *charge_out) {
  uint64_t pad = alignment > 16 ? alignment : 0;
  uint64_t block = capacity + pad;
  void *raw;

  if (block < capacity) return NULL; /* 加法溢出 */
  raw = calloc(1, (size_t)block);
  if (!raw) return NULL;
  *raw_out = (uintptr_t)raw;
  *base_out = pad ? (((uintptr_t)raw + (uintptr_t)(alignment - 1)) &
                     ~(uintptr_t)(alignment - 1))
                  : (uintptr_t)raw;
  *charge_out = block;
  return raw;
}

static uint64_t block_bytes(uint64_t capacity, uint64_t alignment) {
  uint64_t pad = alignment > 16 ? alignment : 0;
  return capacity + pad;
}

LainVmRegionHandle lainvm_space_alloc(LainVmSpace *space, uint64_t capacity,
                                      uint64_t alignment,
                                      uint64_t initial_accessible,
                                      uint32_t rights, uint64_t owner,
                                      LainVmQuota *quota) {
  LainVmRegionHandle handle = lainvm_space_no_handle();
  uintptr_t base = 0;
  uintptr_t raw = 0;
  uint64_t charged = 0;
  void *block;

  if (!space || capacity == 0) return handle;
  if (initial_accessible > capacity) return handle;
  if (block_bytes(capacity, alignment) < capacity) return handle;
  /* 1) quota 原子预扣（余额不足 / 溢出 -> 拒，账目不动）。 */
  charged = block_bytes(capacity, alignment);
  if (lainvm_quota_charge(quota, charged) != 0) return handle;
  /* 2) 分配 + 清零（calloc）。失败要回滚预扣。 */
  block = alloc_block(capacity, alignment, &base, &raw, &charged);
  if (!block) {
    (void)lainvm_quota_release(quota, block_bytes(capacity, alignment));
    return handle;
  }
  /* 3) 登记稳定区段；失败（重叠 / 没空槽）要把刚拿到的东西全部还回去。 */
  handle = register_region(space, base, capacity, initial_accessible, rights,
                           owner, LAINVM_BACKING_OWNED, raw, quota, charged);
  if (lainvm_space_handle_none(handle)) {
    free((void *)raw);
    (void)lainvm_quota_release(quota, charged);
    return handle;
  }
  return handle;
}

bool lainvm_space_free(LainVmSpace *space, LainVmRegionHandle handle) {
  const LainVmRegion *view = lainvm_space_slot(space, handle);
  LainVmRegion snapshot;
  if (!view) return false;
  if (view->backing_kind != LAINVM_BACKING_OWNED) return false; /* external 走 unmap */
  if (view->borrow_count != 0) return false; /* 还有活租约：区域、借用计数、账都不动 */
  snapshot = *view; /* 撤销之后槽里的字段就只是"曾将是谁"的记录了 */
  if (!revoke_region(space, handle)) return false;
  free((void *)(snapshot.raw ? snapshot.raw : snapshot.base));
  (void)lainvm_quota_release(snapshot.quota, snapshot.charged);
  return true;
}

LainVmRegionHandle lainvm_space_map_external(LainVmSpace *space, uintptr_t base,
                                             uint64_t capacity,
                                             uint64_t accessible,
                                             uint32_t rights, uint64_t owner) {
  return register_region(space, base, capacity, accessible, rights, owner,
                         LAINVM_BACKING_EXTERNAL, 0, NULL, 0);
}

bool lainvm_space_unmap_external(LainVmSpace *space, LainVmRegionHandle handle) {
  const LainVmRegion *region = lainvm_space_slot(space, handle);
  if (!region) return false;
  if (region->backing_kind != LAINVM_BACKING_EXTERNAL) return false; /* owned 走 free */
  if (region->borrow_count != 0) return false; /* 还有活租约 */
  /* 只撤销映射：**不** free(base)，**不**归还 quota（它就没扣过）。 */
  return revoke_region(space, handle);
}

LainVmRegionHandle lainvm_space_alloc_stack(LainVmSpace *space, uint64_t bytes,
                                            uint64_t owner, LainVmQuota *quota) {
  /* 栈的约定：owned、16 字节对齐（水位按 16 对齐算）、初始窗口 0、READ|WRITE。 */
  return lainvm_space_alloc(space, bytes, 16, 0,
                            LAINVM_MEM_READ | LAINVM_MEM_WRITE, owner, quota);
}

bool lainvm_space_borrow(LainVmSpace *space, LainVmRegionHandle handle) {
  LainVmRegion *region = slot_mut(space, handle);
  if (!region) return false;
  if (region->borrow_count == 0xFFFFFFFFu) return false; /* 计数到顶：不加，拒绝 */
  region->borrow_count += 1;
  return true;
}

bool lainvm_space_end_borrow(LainVmSpace *space, LainVmRegionHandle handle) {
  LainVmRegion *region = slot_mut(space, handle);
  if (!region) return false;
  if (region->borrow_count == 0) return false; /* 没有借用可结束 */
  region->borrow_count -= 1;
  return true;
}

LainVmStackLease lainvm_stack_no_lease(void) {
  LainVmStackLease lease;
  lease.space = NULL;
  lease.region = lainvm_space_no_handle();
  return lease;
}

bool lainvm_stack_lease_none(LainVmStackLease lease) {
  return lease.space == NULL || lainvm_space_handle_none(lease.region);
}

bool lainvm_space_set_accessible(LainVmSpace *space, LainVmRegionHandle handle,
                                 uint64_t accessible) {
  LainVmRegion *region = slot_mut(space, handle);
  if (!region) return false;
  if (accessible > region->capacity) return false; /* 窗口不许超过存储 */
  if (region->base + (uintptr_t)accessible < region->base) return false;
  region->accessible = accessible; /* 失败路径都在上面返回了：区段一点没动 */
  return true;
}

const LainVmRegion *lainvm_space_find(const LainVmSpace *space, uintptr_t addr) {
  uint32_t lo = 0;
  uint32_t hi;
  if (!space || space->live_count == 0) return NULL;
  hi = space->live_count;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    const LainVmRegion *region = &space->slots[space->by_base[mid]];
    if (addr < region->base) {
      hi = mid;
    } else if (addr - region->base >= region->capacity) {
      lo = mid + 1;
    } else {
      return region;
    }
  }
  return NULL;
}

bool lainvm_space_check(const LainVmSpace *space, uintptr_t addr, uint64_t size,
                        uint32_t need) {
  const LainVmRegion *region = lainvm_space_find(space, addr);
  if (!region) return false;
  if ((region->rights & need) != need) return false;
  /* `find` 是按 **capacity** 找到这段存储的，所以 addr 可能落在存储里、却在
   * 可访问窗口之外 —— 必须先判一次窗口边界。少了这一句，下面的减法会下溢成
   * 一个巨大的"剩余量"，于是超出窗口的访问被**静默放行**（实测抓到）。 */
  if (addr - region->base >= region->accessible) return false;
  /* 上界用**可访问窗口**判（不是 capacity）：被收回的窗口立刻访问不了。 */
  if ((uint64_t)(region->base + (uintptr_t)region->accessible - addr) < size)
    return false;
  return true;
}
