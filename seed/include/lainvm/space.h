/* LAINVM 地址空间。
 *
 * microkernel 风格：VSpace 是独立的地址空间对象，TCB 只引用它——
 * 所以多个 TCB 能共享一个地址空间，那就是线程。
 *
 * 地址模型：**地址就是宿主地址**。
 *   好处：`#ptr2int` / `#int2ptr` 保持纯位重解释，L1Value 里的 addr 就是 void *。
 *   代价：程序能算出任意地址（但用不了，每次访问都查区段）。
 *
 * VSpace 不分配内存。它登记别人给的内存，并判定一次访问合不合法。
 * 清零也是提供内存那一方的责任——不零就没有确定性，固定点比较会废。
 *
 * 区段不是「一个数据对象一个」，而是按权限分堆：装载器把模块的数据对象
 * 排成两段连续内存（只读、可写），登记成两个区段。这就是 .rodata 和 .data。
 *
 * --- 句柄：引用的是**身份**，不是位置 ---------------------------------------
 *
 * 区段引用曾经是数组下标——于是插入或移除别的区段会把已有引用挪到别的对象上
 * （实测：TCB 缓存的栈下标指到别人，销毁时按那个下标取地址去 free，
 * 直接把进程打成堆损坏 0xC0000374）。现在引用是句柄：
 *
 *   { space, slot, generation }
 *
 *   space       哪个 VSpace。用空间对象**自己的地址**做标签（不需要全局计数器），
 *               所以跨空间使用会被拒。
 *   slot        槽号。**槽的位置永不改变**，内部怎么排序都跟它无关。
 *   generation  这个槽被重用过几次。撤销之后句柄不会"活过来"指到新对象。
 *
 * 重排只发生在 `by_base` 这条索引上（给地址查找用二分）。`slots` 不动。
 * 代数加到 0xFFFFFFFF 就不再重用那个槽（宁可少一个槽，也不让旧句柄复活）。
 *
 * `find` 返回的是**内部指针**，只在本次调用到下一次修改之间有效；
 * 要长期持有的引用请保存句柄。
 */
#ifndef LAINVM_SPACE_H
#define LAINVM_SPACE_H

#include <stdbool.h>
#include <stdint.h>

/* 定长：内核结构不做动态分配。2 个模块段 + 各 TCB 的栈 + 宿主注入，
 * 64 足够；满了就是拒绝，不是扩容。 */
#define LAINVM_SPACE_MAX_REGIONS 64

/* 槽号的"没有"哨兵。用 #define 而不是 enum：0xFFFFFFFF 超出 int 的范围。 */
#define LAINVM_SPACE_NO_SLOT 0xFFFFFFFFu

typedef struct {
  uintptr_t space;    /* 所属 VSpace（空间对象地址） */
  uint32_t slot;      /* 槽号 */
  uint32_t generation; /* 这个槽的代数 */
} LainVmRegionHandle;

/* 区段权限。CALL 是给 #call_indirect 用的：没有它，函数指针指向一块数据
 * 也会被放行。#addr 本身不区分代码和数据，区分靠这张权限。 */
typedef enum {
  LAINVM_MEM_READ = 1u << 0,
  LAINVM_MEM_WRITE = 1u << 1,
  LAINVM_MEM_CALL = 1u << 2,
} LainVmMemRights;

/* 一个槽。`owner` 只留给诊断（"这段是谁的"），**撤销不再按它扫表**：
 * 撤销要么给精确句柄，要么给调用方自己记下来的句柄集合。 */
typedef struct {
  uintptr_t base;
  uint64_t size;
  uint32_t rights;
  uint64_t owner; /* 0 = 模块 / 装载器的；否则是拥有它的 TCB id */
  uint32_t generation;
  bool alive;
} LainVmRegion;

typedef struct {
  LainVmRegion slots[LAINVM_SPACE_MAX_REGIONS];
  uint32_t by_base[LAINVM_SPACE_MAX_REGIONS]; /* 活槽号，按 slots[槽].base 升序 */
  uint32_t live_count;
} LainVmSpace;

void lainvm_space_init(LainVmSpace *space);

/* 一个"没有区段"的句柄（未登记 / 已撤销的返回值）。 */
LainVmRegionHandle lainvm_space_no_handle(void);
bool lainvm_space_handle_none(LainVmRegionHandle handle);

/* 登记一段别人给的内存，返回句柄。
 * 拒绝：size == 0、base + size 不可表示、与已有区段重叠、没有空槽。
 * 失败返回 no_handle，且**表不变**。 */
LainVmRegionHandle lainvm_space_add(LainVmSpace *space, uintptr_t base,
                                    uint64_t size, uint32_t rights,
                                    uint64_t owner);

/* 按句柄取槽。无效、已撤销、代数不符、跨空间 → NULL。 */
const LainVmRegion *lainvm_space_slot(const LainVmSpace *space,
                                      LainVmRegionHandle handle);

/* 按句柄精确撤销。成功返回 true（表里少一段）；
 * 无效 / 重复撤销 / 跨空间 → false，且**表不变**。 */
bool lainvm_space_remove(LainVmSpace *space, LainVmRegionHandle handle);

/* 找包含 addr 的区段；给权限判定和诊断用。没有则返回 NULL。
 * 二分查找——热路径上不许线性扫。返回内部指针，见文件头那句。 */
const LainVmRegion *lainvm_space_find(const LainVmSpace *space,
                                      uintptr_t addr);

/* 从 addr 起的 size 个字节，能不能按 need 访问。
 * 上界用减法判，避免 addr + size 溢出。 */
bool lainvm_space_check(const LainVmSpace *space, uintptr_t addr, uint64_t size,
                        uint32_t need);

#endif /* LAINVM_SPACE_H */
