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

#include "lainvm/quota.h"

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

/* 这块存储是**谁的**——决定释放语义。两个东西不能混在一个模糊的 remove 里：
 * "撤销授权"、"释放底层内存"、"归还 quota" 是三件不同的事。 */
typedef enum {
  LAINVM_BACKING_OWNED = 0,    /* VSpace 申请、清零、登记、释放；计入 quota */
  LAINVM_BACKING_EXTERNAL = 1, /* 外部借入：只授权与撤销，不 free、不重复扣账 */
} LainVmBackingKind;

/* 一个槽。
 *
 * **容量与可访问窗口是两件事**（2026-09-21）：
 *   capacity    这块存储有多少字节。**占用与重叠**判断用 [base, base + capacity)。
 *   accessible  当前允许访问的**前缀**长度。**访问判定**用 [base, base + accessible)。
 *
 * 为什么分开：栈是**一次**申请 4096 字节（计入 quota、清零、登记一次），
 * 而可访问的前缀随 `#alloca` 增长、随过程返回收缩。以前这两件事挤在一个 `size`
 * 里，于是"收窗口"只能 remove + add（句柄不稳定、还要靠重叠检查挡路）——
 * 那不是数据模型，那是绕路。
 *
 * `owner` 只留给诊断（"这段是谁的"），**撤销不再按它扫表**：
 * 撤销要么给精确句柄，要么给调用方自己记下来的句柄集合。 */
typedef struct {
  uintptr_t base;
  uint64_t capacity;   /* 存储字节数；占用与重叠看它 */
  uint64_t accessible; /* 当前可访问前缀；访问判定看它（<= capacity） */
  uint32_t rights;
  uint64_t owner; /* 0 = 模块 / 装载器的；否则是拥有它的 TCB id */
  uint32_t generation;
  bool alive;
  /* 所有权（owned / external）与借用计数 */
  uint32_t backing_kind; /* LainVmBackingKind */
  uint32_t borrow_count; /* 当前活租约数量；owned 存储有借用时不许释放 */
  /* owned 存储的账目：从哪个账户扣的、实际扣了多少、原始指针在哪（对齐过 base
   * 时要按 raw 释放）。external 存储这三项都是 0 —— 它不归 VSpace，也不扣账。 */
  LainVmQuota *quota;
  uint64_t charged;
  uintptr_t raw;
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

/* --- owned storage：VSpace 申请、清零、登记、释放 -------------------------
 *
 * 成功路径按顺序做：quota 原子预扣 -> 分配底层存储 -> 清零 -> 登记稳定区段 ->
 * 记下扣账来源与原始指针 -> 返回句柄。**任何一步失败都回滚前面的动作**，
 * 空间表与 quota 保持原状。
 *
 * alignment：<= 16 时直接用宿主分配（它至少给到 16）；更大就多要 alignment 字节
 * 再对齐，此时计入 quota 的是**实际拿到的那一块**（capacity + alignment）。
 * initial_accessible 通常传 0：栈是"先要下来、窗口从小长到大"。
 * 拒绝：capacity == 0、initial_accessible > capacity、与已有区段重叠、没有空槽、
 * 余额不足、底层分配失败。 */
LainVmRegionHandle lainvm_space_alloc(LainVmSpace *space, uint64_t capacity,
                                      uint64_t alignment,
                                      uint64_t initial_accessible,
                                      uint32_t rights, uint64_t owner,
                                      LainVmQuota *quota);

/* 精确撤销 + 释放底层存储 + 按**原来那个账户**归还 charged。
 * 拒绝（表、借用计数、quota 都不变）：句柄无效/跨空间、不是 owned、
 * `borrow_count != 0`（还有活租约）、重复释放。 */
bool lainvm_space_free(LainVmSpace *space, LainVmRegionHandle handle);

/* --- external mapping：别人给的字节，VSpace 只授权与撤销 -------------------
 *
 * 源码文本、宿主对象、映像的 ro/rw/code 都走这条：VSpace 管授权与撤销，
 * 不负责 free(base)，也不把它当成自己申请的存储重复扣账。 */
LainVmRegionHandle lainvm_space_map_external(LainVmSpace *space, uintptr_t base,
                                             uint64_t capacity,
                                             uint64_t accessible,
                                             uint32_t rights, uint64_t owner);

/* 只撤销映射：不 free、不归还 quota。拒绝条件同 free（含 borrow_count != 0）。 */
bool lainvm_space_unmap_external(LainVmSpace *space, LainVmRegionHandle handle);

/* 供给方给一条执行流备一份栈：owned 区段、16 字节对齐、**初始 accessible = 0**、
 * READ | WRITE。只是把 `lainvm_space_alloc` 的参数按栈的约定摆对，
 * 不隐藏所有权：真正释放仍要供给方自己调 `lainvm_space_free`。 */
LainVmRegionHandle lainvm_space_alloc_stack(LainVmSpace *space, uint64_t bytes,
                                            uint64_t owner, LainVmQuota *quota);

/* 长期持有的引用：句柄是身份。 */

/* --- 借用（租约） ---------------------------------------------------------
 *
 * 借出期间这块存储**不许被释放**：`lainvm_space_free` 与
 * `lainvm_space_unmap_external` 都会因为 `borrow_count != 0` 而拒绝。
 * 借用不改变 base / capacity / accessible / 权限，也不动 quota ——
 * 它只记"现在有几个活的持有人在用"。 */
bool lainvm_space_borrow(LainVmSpace *space, LainVmRegionHandle handle);
/* 结束一次借用。计数为 0 时再结束 -> false 且不变（重复归还要看得见）。 */
bool lainvm_space_end_borrow(LainVmSpace *space, LainVmRegionHandle handle);

/* --- 栈租约 -----------------------------------------------------------------
 *
 * 一条执行流**借来**的栈：只有**空间与句柄**。base / capacity / 权限都从 VSpace 的
 * 稳定记录读，不在 TCB 里复制一套可能失真的副本（以前复制的 base/size 在换空间、
 * 换窗口之后就再也对不上了）。
 *
 * 生命周期（供给方 = 谁调用 space_alloc_stack）：
 *   1. 供给方 `lainvm_space_alloc_stack` 拿到句柄（初始 accessible = 0）；
 *   2. 创建 TCB 时把租约传进去，TCB 校验后 borrow_count++；
 *   3. 执行期间 TCB 用 `set_accessible` 决定窗口（水位）；Trap 时窗口归零；
 *   4. 销毁 TCB：窗口收回 0、借用结束，**不**撤销、**不**释放、**不**归还额度；
 *   5. 供给方随后 `lainvm_space_free` 才真正释放并归还额度。 */
typedef struct {
  LainVmSpace *space;        /* 租约所属空间（不是"当前执行空间"） */
  LainVmRegionHandle region; /* 稳定句柄；no_handle = 没有栈 */
} LainVmStackLease;

LainVmStackLease lainvm_stack_no_lease(void);
bool lainvm_stack_lease_none(LainVmStackLease lease);

/* 更新同一区段的**可访问前缀**——句柄全程稳定，窗口变化**不再** remove/add。
 *
 * 这是栈窗口唯一的更新入口（水位涨了放大、水位退了缩小、Trap 归零）。
 * 拒绝：句柄无效 / 已撤销 / 代数不符 / 跨空间、accessible > capacity、
 * base + accessible 不可表示。**失败时区段一点不变**（含 accessible）。
 * 缩小之后，被收回的那一段**立刻**访问不了（check 只看 accessible）。 */
bool lainvm_space_set_accessible(LainVmSpace *space, LainVmRegionHandle handle,
                                 uint64_t accessible);

/* 按句柄取槽（只读视图）。无效 / 已撤销 / 代数不符 / 跨空间 → NULL。 */
const LainVmRegion *lainvm_space_slot(const LainVmSpace *space,
                                      LainVmRegionHandle handle);

/* 找包含 addr 的区段；给权限判定和诊断用。没有则返回 NULL。
 * 二分查找——热路径上不许线性扫。返回内部指针，见文件头那句。 */
const LainVmRegion *lainvm_space_find(const LainVmSpace *space,
                                      uintptr_t addr);

/* 从 addr 起的 size 个字节，能不能按 need 访问。
 * 判定用**可访问窗口** [base, base + accessible)，不是 capacity。
 * 上界用减法判，避免 addr + size 溢出。 */
bool lainvm_space_check(const LainVmSpace *space, uintptr_t addr, uint64_t size,
                        uint32_t need);

#endif /* LAINVM_SPACE_H */
