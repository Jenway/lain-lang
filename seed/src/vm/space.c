/* lainvm/space.h 的实现。 */
#include "lainvm/space.h"

#include <string.h>

void lainvm_space_init(LainVmSpace *space) {
  if (!space) return;
  memset(space, 0, sizeof(*space));
}

/* 按 base 升序插入：区段很少变动，插入排序足够。 */
static void insert_sorted(LainVmSpace *space, uint32_t at) {
  LainVmRegion region = space->regions[at];
  uint32_t i = at;
  while (i > 0 && space->regions[i - 1].base > region.base) {
    space->regions[i] = space->regions[i - 1];
    i--;
  }
  space->regions[i] = region;
}

static bool overlaps(const LainVmRegion *region, uintptr_t base, uint64_t size) {
  uintptr_t end = region->base + (uintptr_t)region->size;
  uintptr_t want_end = base + (uintptr_t)size;
  return base < end && region->base < want_end;
}

int32_t lainvm_space_add_region(LainVmSpace *space, uintptr_t base,
                                uint64_t size, uint32_t rights,
                                uint64_t owner) {
  uint32_t i;
  if (!space || size == 0) return LAINVM_SPACE_NO_REGION;
  if (space->region_count >= LAINVM_SPACE_MAX_REGIONS)
    return LAINVM_SPACE_NO_REGION;
  if (base + (uintptr_t)size < base) return LAINVM_SPACE_NO_REGION; /* 溢出 */
  for (i = 0; i < space->region_count; i++) {
    if (overlaps(&space->regions[i], base, size))
      return LAINVM_SPACE_NO_REGION;
  }
  space->regions[space->region_count].base = base;
  space->regions[space->region_count].size = size;
  space->regions[space->region_count].rights = rights;
  space->regions[space->region_count].owner = owner;
  space->region_count++;
  insert_sorted(space, space->region_count - 1);
  /* 插入之后下标可能变了，按 base 找回来。 */
  for (i = 0; i < space->region_count; i++) {
    if (space->regions[i].base == base && space->regions[i].size == size)
      return (int32_t)i;
  }
  return LAINVM_SPACE_NO_REGION;
}

void lainvm_space_release_owner(LainVmSpace *space, uint64_t owner) {
  uint32_t i = 0;
  if (!space) return;
  while (i < space->region_count) {
    if (space->regions[i].owner == owner) {
      uint32_t j;
      for (j = i; j + 1 < space->region_count; j++)
        space->regions[j] = space->regions[j + 1];
      space->region_count--;
    } else {
      i++;
    }
  }
}

const LainVmRegion *lainvm_space_find(const LainVmSpace *space,
                                      uintptr_t addr) {
  uint32_t lo = 0;
  uint32_t hi;
  if (!space || space->region_count == 0) return NULL;
  hi = space->region_count;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    const LainVmRegion *region = &space->regions[mid];
    if (addr < region->base) {
      hi = mid;
    } else if (addr - region->base >= region->size) {
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
  /* 上界用减法判，避免 addr + size 溢出。 */
  if ((uint64_t)(region->base + (uintptr_t)region->size - addr) < size)
    return false;
  return true;
}
