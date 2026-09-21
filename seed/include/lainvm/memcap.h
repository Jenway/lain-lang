/* LAINVM 最小内存能力模型：存储对象 + 内存能力 + 受检引用。
 *
 * 依据 `docs/implementation/vm-memory-capability.md`：内存访问能力要关联对象身份、
 * 可访问范围、权限和有效代数；过程调用结束后它那次调用的局部对象对应的能力失效；
 * 底层存储可以复用，**但旧引用必须继续被拒**。
 *
 * 分工（同文档 §2）：
 *   TCB 执行上下文   调用栈、水位、调用结束事件
 *   VSpace           可访问的区域及空间授权
 *   **这一层**       对象存活状态、代数、内存能力
 *   供给方           底层存储的供应与释放（这一层**不分配内存**）
 *   共享预算账户     实际承诺存储的额度与消耗（`committed` / `released`）
 *
 * VSpace 与这一层**都必须成立**：VSpace 管「这段内存在这个空间里能不能碰」，
 * 这一层管「这个引用指的是不是那个对象、范围与权限还在不在」。切换空间不会给
 * 旧引用自动补发权限。
 *
 * --- 引用的形状与宽度（作者 2026-09-21 定：**大卡**）--------------------------
 *
 *   受检引用 = 能力句柄 {槽号, 代数}（8 B） + 对象内偏移（8 B） = **16 B**
 *
 * 对象身份、范围、权限、代数**都在表里**，不在引用里 —— 所以改引用的位不能扩大
 * 范围或提权，最多把它改坏（改坏就被拒）。
 *
 * **表身份不放在句柄里**，改用「代数基数」：`lainvm_memcap_init` 要一个
 * `generation_base`，调用方保证它**大于同一块宿主内存上以前发过的所有代数**。
 * 于是同址重建的上下文发出来的代数一定更高，旧引用（槽号对得上、代数对不上）
 * 照样被拒。好处是句柄从 16 B 降到 8 B，受检引用 16 B 而不是 24 B。
 * 代价：调用方要把这个单调基数传下来（仍然是"能力注入、没有全局可变状态"）。
 *
 * 引用里可复制的字段**本身不是不可伪造的凭据**。本模型的伪造防护靠两条：
 *   1. 代数：改代数、拿别的上下文的引用（代数不同）→ 拒。
 *   2. 能力记录里的所属上下文 owner：拿到别人的引用（位一模一样）也用不了 → 拒。
 * 残留缺口（写在运行报告里，不假装解决）：槽号与代数是可猜的；真正的系统要把
 * 能力记录放进程序寻址不到的表（CSpace），程序只拿得到其中的索引。
 *
 * 诊断码（本层稳定码，全树此前未占用）：
 *   9200 对象表满            9201 对象登记参数非法        9202 能力表满
 *   9203 授权范围越出对象     9204 授权权限为空            9205 **已废弃**
 *   9206 当前上下文无权使用   9207 能力已撤销 / 槽无效      9208 引用代数不符
 *   9209 权限不足            9210 越出授权范围             9211 对象已不存活（存储已释放）
 *   9212 当前 VSpace 未授权   9213 撤销 / 释放的句柄无效（含 owner=0、重复释放）
 *
 * 9205 原义是「引用的表身份不符」。句柄不带表身份之后这个情形不存在了：同址重建
 * 由代数基数检出（9208）。号码**保留不重用**，免得以后接上真实通路时旧日志对不上。
 *
 * 最小模型**不含**：能力派生树、通用委派、跨进程序列化、并发（检查与实际访问之间
 * 不得发生撤销或释放，这个前提由调用方保证）。
 */
#ifndef LAINVM_MEMCAP_H
#define LAINVM_MEMCAP_H

#include <stdbool.h>
#include <stdint.h>

#include "lainvm/space.h"

/* 定长，不做动态分配。对象数比 VSpace 区段数增长得快，所以是**另一张表**：
 * 一次调用里每次 alloca 都是一个对象，而栈整段只是一个区段。 */
#define LAINVM_MEMCAP_MAX_OBJECTS 32
#define LAINVM_MEMCAP_MAX_CAPS 64

/* 代数上限。到顶就不再重用那个槽 —— 宁可少一个槽，也不让旧引用复活。 */
#define LAINVM_MEMCAP_MAX_GENERATION 0xFFFFFFFFu

/* 一笔存储对象（一次分配一个生命周期）。 */
typedef struct {
  uintptr_t base;   /* 宿主基址（存储由供给方给） */
  uint64_t size;
  uint64_t owner;   /* 生命周期归属：一次调用一个标签 */
  uint32_t generation;
  bool live;
} LainVmMemObject;

/* 一条内存能力：授权子范围 + 权限 + 所属上下文。 */
typedef struct {
  uint32_t object;         /* 对象槽 */
  uint32_t generation;     /* 发放时对象的代数 */
  uint64_t offset;         /* 授权范围起点（对象内） */
  uint64_t length;         /* 授权范围长度 */
  uint32_t rights;         /* LAINVM_MEM_READ / WRITE */
  uint64_t owner;          /* 授权给的上下文（一次调用的标签） */
  uint32_t cap_generation; /* 能力槽自己的代数：撤销后重用不会复活旧引用 */
  bool live;
} LainVmMemCap;

typedef struct {
  uint64_t committed; /* 已承诺、尚未释放的存储字节（共享预算账户的消耗） */
  uint64_t released;  /* 已释放归还的字节 */
  LainVmMemObject objects[LAINVM_MEMCAP_MAX_OBJECTS];
  LainVmMemCap caps[LAINVM_MEMCAP_MAX_CAPS];
  uint32_t objects_live;
  uint32_t caps_live;
} LainVmMemTable;

/* 句柄引用的是身份，不是位置（同 space.h 的道理）。8 B：这就是"大卡"的宽度。 */
typedef struct {
  uint32_t slot;
  uint32_t generation; /* 0 = 没有句柄（保留值），所以句柄里代数永远 ≥ 1 */
} LainVmMemHandle;

/* 受检引用：能力 + 对象内偏移。**16 B** —— 值表示变更按这个宽度设计。 */
typedef struct {
  LainVmMemHandle cap;
  uint64_t offset;
} LainVmMemRef;

/* 表初始化。`generation_base` 必须大于同一块内存上以前发过的所有代数：
 * 销毁一个上下文并原地重建时，新基数更大，旧引用才不会复活。 */
void lainvm_memcap_init(LainVmMemTable *table, uint64_t generation_base);

LainVmMemHandle lainvm_memcap_no_handle(void);
bool lainvm_memcap_handle_none(LainVmMemHandle handle);

/* 供给方登记一笔存储。失败（参数非法 / 表满）返回 no_handle 且**表不变**。
 * 成功则 committed += size。 */
LainVmMemHandle lainvm_memcap_object_add(LainVmMemTable *table, uintptr_t base,
                                         uint64_t size, uint64_t owner);

/* 供给方释放存储（能力本身不动：持有者再去用会被判「对象已不存活」）。
 * committed 在这里归还，不在撤销能力时 —— 撤销访问权和释放存储是两个动作。 */
int32_t lainvm_memcap_object_release(LainVmMemTable *table,
                                     LainVmMemHandle object);

/* 按对象发放能力（最小模型不做能力派生：只能从对象发，不能从能力再发）。 */
int32_t lainvm_memcap_grant(LainVmMemTable *table, LainVmMemHandle object,
                            uint64_t offset, uint64_t length, uint32_t rights,
                            uint64_t owner, LainVmMemHandle *out);

/* 撤销一个上下文的全部能力（含从它复制出的引用——它们共用同一条记录）。
 * owner == 0 是保留值（模块 / 装载器的能力），按 0 撤销一律拒绝：9213。 */
int32_t lainvm_memcap_revoke_owner(LainVmMemTable *table, uint64_t owner,
                                   uint32_t *revoked);

/* 受检解析：通过 → 0，`*out_base` 是这次访问的宿主地址；否则非 0 且 `*code` 是拒绝码。
 * 检查顺序就是文档 §3 的五条。`length` 是这次访问的字节数。 */
int lainvm_memcap_resolve(const LainVmMemTable *table, const LainVmSpace *space,
                          LainVmMemRef ref, uint32_t need, uint64_t length,
                          uint64_t context, uintptr_t *out_base, int32_t *code);

/* 受检读 / 写：解析通过才碰内存。 */
int lainvm_memcap_read(const LainVmMemTable *table, const LainVmSpace *space,
                       LainVmMemRef ref, uint64_t context, uint64_t length,
                       void *out, int32_t *code);
int lainvm_memcap_write(const LainVmMemTable *table, const LainVmSpace *space,
                        LainVmMemRef ref, uint64_t context, uint64_t length,
                        const void *in, int32_t *code);

/* 统一受检宿主适配器（§7）：**先解析、再产生副作用**。
 * 解析失败返回非 0，`*side_effects` 不变、目标内存不变。 */
int lainvm_memcap_host_write(const LainVmMemTable *table,
                             const LainVmSpace *space, LainVmMemRef ref,
                             uint64_t context, uint64_t length, const void *in,
                             uint64_t *side_effects, int32_t *code);

/* 整数转换（§4.2）：把引用压成一个整数，再解回来。
 * 位布局：代数(32) | 槽号(32)，**不含偏移**（偏移由来往双方各自提供）。
 * 撤销后旧整数解回来代数不符 → 拒；伪造整数的槽号/代数对不上 → 拒。 */
uint64_t lainvm_memcap_ref_to_int(LainVmMemRef ref);
LainVmMemRef lainvm_memcap_int_to_ref(uint64_t bits, uint64_t offset);

#endif /* LAINVM_MEMCAP_H */
