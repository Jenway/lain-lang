/* lainvm/space.h 的实现。
 *
 * 两条不变量，改这个文件时先读这两句：
 *   1. `slots` 里的位置**永不改变**（句柄的身份靠它 + 代数）。所有重排只发生在
 *      `by_base` 这条索引上。谁要是再来一次「插入之后按 base 找回来」，
 *      就是把身份写回位置，这个设计立刻作废。
 *   2. 撤销只认句柄。没有「按 owner 扫表删」这条路——它会让 owner=0 的
 *      装载器区段被一次误调用清掉，也表达不了「这块是我派生给你的那一块」。
 */
#include "lainvm/space.h"

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

static bool overlaps(const LainVmRegion *region, uintptr_t base, uint64_t size) {
  uintptr_t end = region->base + (uintptr_t)region->capacity;
  uintptr_t want_end = base + (uintptr_t)size;
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

LainVmRegionHandle lainvm_space_add(LainVmSpace *space, uintptr_t base,
                                    uint64_t size, uint32_t rights,
                                    uint64_t owner) {
  uint32_t i;
  uint32_t slot = LAINVM_SPACE_NO_SLOT;
  LainVmRegionHandle handle = lainvm_space_no_handle();

  if (!space || size == 0) return handle;
  if (base + (uintptr_t)size < base) return handle; /* 上界不可表示 */
  for (i = 0; i < space->live_count; i++) {
    if (overlaps(&space->slots[space->by_base[i]], base, size)) return handle;
  }
  /* 找一个能用的槽：空的，而且代数还没到顶（到顶就不再重用——
   * 宁可少一个槽，也不让撤销过的旧句柄"活过来"指到新对象）。 */
  for (i = 0; i < LAINVM_SPACE_MAX_REGIONS; i++) {
    if (!space->slots[i].alive && space->slots[i].generation != 0xFFFFFFFFu) {
      slot = i;
      break;
    }
  }
  if (slot == LAINVM_SPACE_NO_SLOT) return handle;

  space->slots[slot].base = base;
  space->slots[slot].capacity = size;
  space->slots[slot].accessible = size; /* 登记之后整段立刻可访问 */
  space->slots[slot].rights = rights;
  space->slots[slot].owner = owner;
  space->slots[slot].generation += 1; /* 从 1 开始：全零句柄永远不会撞上活槽 */
  space->slots[slot].alive = true;
  space->live_count++;
  index_insert(space, slot);

  handle.space = (uintptr_t)space;
  handle.slot = slot;
  handle.generation = space->slots[slot].generation;
  return handle;
}

/* 句柄 → 可写槽。`lainvm_space_slot` 返回的是 const 视图（查表语义），
 * 这里要改的就是它指的那个槽，所以去掉 const —— 换的不是类型，是访问方式。 */
static LainVmRegion *slot_mut(LainVmSpace *space, LainVmRegionHandle handle) {
  return (LainVmRegion *)lainvm_space_slot(space, handle);
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

bool lainvm_space_remove(LainVmSpace *space, LainVmRegionHandle handle) {
  const LainVmRegion *region = lainvm_space_slot(space, handle);
  if (!region) return false;
  index_remove(space, handle.slot);
  space->live_count--;
  space->slots[handle.slot].alive = false;
  /* base/size 留着不清：诊断时还能看出来这个槽曾经是谁的。 */
  return true;
}

const LainVmRegion *lainvm_space_find(const LainVmSpace *space,
                                      uintptr_t addr) {
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
