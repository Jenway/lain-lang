/* LAINVM 地址空间。
 *
 * microkernel 风格：VSpace 是独立的地址空间对象，TCB 只引用它——
 * 所以多个 TCB 能共享一个地址空间，那就是线程。
 *
 * 地址模型：**地址就是宿主地址**（方案 A）。
 *   好处：`#ptr2int` / `#int2ptr` 保持纯位重解释，L1Value 里的 addr 就是 void *。
 *   代价：程序能算出任意地址（但用不了，每次访问都查区段），
 *        `#ptr2int` 能读到宿主地址。
 *
 * VSpace 不分配内存。它登记别人给的内存，并判定一次访问合不合法。
 * 清零也是提供内存那一方的责任——不零就没有确定性，固定点比较会废。
 *
 * 区段不是「一个数据对象一个」，而是按权限分堆：装载器把模块的数据对象
 * 排成两段连续内存（只读、可写），登记成两个区段。这就是 .rodata 和 .data。
 * 一个 TCB 的 alloca 栈是第三个区段，owner 指向那个 TCB。
 */
#ifndef LAINVM_SPACE_H
#define LAINVM_SPACE_H

#include <stdbool.h>
#include <stdint.h>

/* 定长：内核结构不做动态分配。2 个模块段 + 各 TCB 的栈 + 宿主注入，
 * 64 足够；满了就是拒绝，不是扩容。 */
#define LAINVM_SPACE_MAX_REGIONS 64

enum { LAINVM_SPACE_NO_REGION = -1 };

/* 区段权限。CALL 是给 #call_indirect 用的：没有它，函数指针指向一块数据
 * 也会被放行。#addr 本身不区分代码和数据，区分靠这张权限。 */
typedef enum {
  LAINVM_MEM_READ = 1u << 0,
  LAINVM_MEM_WRITE = 1u << 1,
  LAINVM_MEM_CALL = 1u << 2,
} LainVmMemRights;

typedef struct {
  uintptr_t base;
  uint64_t size;
  uint32_t rights;
  uint64_t owner; /* 0 = 模块 / 装载器的；否则是拥有它的 TCB id */
} LainVmRegion;

typedef struct {
  LainVmRegion regions[LAINVM_SPACE_MAX_REGIONS]; /* 按 base 升序 */
  uint32_t region_count;
} LainVmSpace;

void lainvm_space_init(LainVmSpace *space);

/* 登记一段别人给的内存。
 * 区间重叠、表满、size == 0 都拒绝，返回 LAINVM_SPACE_NO_REGION。
 * 成功返回区段下标。 */
int32_t lainvm_space_add_region(LainVmSpace *space, uintptr_t base,
                                uint64_t size, uint32_t rights,
                                uint64_t owner);

/* TCB 销毁时回收它拥有的全部区段。 */
void lainvm_space_release_owner(LainVmSpace *space, uint64_t owner);

/* 找包含 addr 的区段；给权限判定和诊断用。没有则返回 NULL。
 * 二分查找——热路径上不许线性扫。 */
const LainVmRegion *lainvm_space_find(const LainVmSpace *space,
                                      uintptr_t addr);

/* 从 addr 起的 size 个字节，能不能按 need 访问。
 * 上界用减法判，避免 addr + size 溢出。 */
bool lainvm_space_check(const LainVmSpace *space, uintptr_t addr, uint64_t size,
                        uint32_t need);

#endif /* LAINVM_SPACE_H */
