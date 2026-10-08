/* LAINVM 的内核对象。
 *
 * microkernel 风格：
 *   - TCB 是不透明的内核对象。LAINIR 里没有任何类型、值或指令能引用它，
 *     TCB 的地址绝不进入程序。
 *   - TCB ≠ 进程。地址空间（LainVmSpace）是独立对象，TCB 只引用它，
 *     所以多个 TCB 能共享一个地址空间——那就是线程。
 *   - 上下文住在 TCB，不住在地址空间：区域栈和值槽是执行状态，
 *     程序既不能读也不能命名它们（值不占存储、不可寻址）。
 *   - 故障不致命：出错走 fault_handler 这条端点，把 trap 交出去；
 *     TCB 只记录，不自行终止。
 *
 * 命名：LAINIR 侧 L1*，LAINVM 侧 LainVm*。
 */
#ifndef LAINVM_TCB_H
#define LAINVM_TCB_H

#include <stdbool.h>
#include <stdint.h>

#include "lain/ir/codes.h"
#include "lain/ir/core.h"
#include "lain/ir/value.h"
#include "lain/vm/caps.h"
#include "lain/vm/endpoint.h"
#include "lain/vm/quota.h"
#include "lain/vm/space.h"
#include "lain/vm/trap.h"

typedef struct LainVmTcb LainVmTcb;
typedef struct LainVmImage LainVmImage;

typedef enum {
  LAINVM_READY = 0,
  LAINVM_RUNNING = 1,
  LAINVM_BLOCKED = 2,
  LAINVM_DEAD = 3,
} LainVmState;

typedef enum {
  LAINVM_SLICE_RUNNABLE = 0, /* 还有事做：可再次选中 */
  LAINVM_SLICE_BLOCKED = 1,  /* 阻塞在某个端点上 */
  LAINVM_SLICE_DONE = 2,     /* 激活结束 */
  LAINVM_SLICE_TRAPPED = 3,  /* 拒绝执行，trap 已记录 */
} LainVmSliceResult;

/* BLOCKED 时的原因 */
enum {
  LAINVM_SUSPEND_YIELD = 1,
  LAINVM_SUSPEND_ENDPOINT = 2,
};

/* 租约校验的**接口层**状态（不是运行期 Trap，也不占用 engine 的 1xxx 号段）。
 * 供给方与驱动靠它分辨"为什么 TCB 不肯收这份租约"。 */
typedef enum {
  LAINVM_LEASE_OK = 0,
  LAINVM_LEASE_NOT_IN_SPACE = 1,    /* 句柄不属于给定的 VSpace */
  LAINVM_LEASE_BAD_HANDLE = 2,      /* 区段无效 / 已撤销 / 代数不符 */
  LAINVM_LEASE_NEEDS_READ_WRITE = 3,/* 权限里缺 READ 或 WRITE */
  LAINVM_LEASE_WINDOW_NOT_ZERO = 4, /* 交来时窗口不是 0（该由 TCB 自己开窗） */
  LAINVM_LEASE_ZERO_CAPACITY = 5,   /* 容量为 0 */
  LAINVM_LEASE_ALREADY_BORROWED = 6,/* 已借给别的执行流（栈租约是独借） */
  LAINVM_LEASE_BORROW_FAILED = 7,   /* 借用计数加不上（到顶） */
} LainVmLeaseStatus;

/* 只校验，不产生任何副作用（不借用、不改区段）。`lainvm_tcb_new` 用它，
 * 驱动与验收也用它 —— 失败原因因此是可分辨的、可断言的。 */
LainVmLeaseStatus lainvm_lease_check(const LainVmSpace *space,
                                     LainVmStackLease lease);

/* --- 读回执行状态 ---------------------------------------------------------
 * 结构体本身是引擎私有的（seed/src/vm/tcb_internal.h）：驱动与宿主机制只能
 * 从这里读，不能再直接取字段。写路径永远是引擎与下面这些 admit/控制函数。 */

LainVmState lainvm_tcb_state(const LainVmTcb *tcb);

/* has_result 为假时 result() 返回零值，别用。 */
bool lainvm_tcb_has_result(const LainVmTcb *tcb);
L1Value lainvm_tcb_result(const LainVmTcb *tcb);

/* 已记录的那次故障。没有故障时 kind 是 LAINVM_TRAP_NONE，读不到 NULL。 */
const LainVmTrap *lainvm_tcb_trap(const LainVmTcb *tcb);

/* 这个 TCB 一共执行了多少步。 */
uint64_t lainvm_tcb_steps(const LainVmTcb *tcb);

/* admit、重绑与换空间的拒绝码（L1Diagnostic）。 */
enum {
  LAINVM_TCB_ERR_NO_ENTRY = 9101,
  LAINVM_TCB_ERR_ENTRY_NOT_FOUND = 9102,
  LAINVM_TCB_ERR_ARG_COUNT = 9103,
  LAINVM_TCB_ERR_ENTRY_TOO_LARGE = 9104, /* 入口不落在 admit 算出的上界里 */
  LAINVM_TCB_ERR_NO_IMAGE = 9110,
  LAINVM_TCB_ERR_RESOLVE_FAILED = 9111,
  LAINVM_TCB_ERR_CAPS_NOT_FROZEN = 9112,
  LAINVM_TCB_ERR_ACTIVE = 9113, /* 还有活动帧，不能重绑 */
  LAINVM_TCB_ERR_NO_SPACE = 9120,
  LAINVM_TCB_ERR_RUNNING = 9121, /* 运行中不许换地址空间 */
};

_Static_assert(LAINVM_TCB_ERR_NO_ENTRY > LAINIR_CODES_TCB_BASE &&
                   LAINVM_TCB_ERR_RUNNING < LAINIR_CODES_TCB_LIMIT,
               "tcb codes must stay inside the tcb segment");

/* --- admit -----------------------------------------------------------------
 * 引擎里不许分配，所以帧栈和值槽在这里一次给够：
 *   frame_cap = max_call_depth * (image->max_region_depth + 1)
 *   slot_cap  = frame_cap * image->max_slots
 * id 只是诊断字段（区段表里写着"这段是谁的"）；回收按句柄精确撤销，不按 owner 扫表。
 * lease 是供给方给的**栈租约**（`lainvm_stack_no_lease()` = 程序没有 `#alloca`）。
 * 有租约时先校验（句柄属于 `space`、区段有效、含 READ|WRITE、当前 accessible == 0、
 * capacity > 0、**还没被别的执行流借走**），通过后 `borrow_count++`。**TCB 不申请、不清零、不释放栈字节，
 * 也不扣配额** —— 额度在供给方 `lainvm_space_alloc_stack` 时就扣过了。
 * 创建中途失败会把已取得的借用还回去，但**不**释放供给方的区段。
 * quota 是这次执行的分配账户（NULL = 不限额）；TCB 只持有这个引用，不扣栈容量。 */
LainVmTcb *lainvm_tcb_new(LainVmImage *image, LainVmSpace *space, uint64_t id,
                          uint64_t owner, uint32_t max_call_depth,
                          LainVmStackLease lease, LainVmQuota *quota);
void lainvm_tcb_free(LainVmTcb *tcb);

/* 把模块里每个 extern 子过程按 link_name 解析到能力空间。
 * 解析不到的记 NULL，等真去调它时再拒绝——一个模块可以带着用不到的外部依赖。
 *
 * 非 NULL 表**必须已冻结**（9112），否则稳定拒绝；这里不替调用方 freeze。
 * 已有活动帧（运行中、暂停中或 Trap 后未结束的激活）时拒绝重绑（9113）。
 * NULL 表表示无能力：清掉解析项，外部调用仍拒 1110。纯计算 TCB 不要求表。
 * 解析在临时数组里完成，任何拒绝或 OOM 都保留旧绑定。
 * 表必须活得比 TCB 长，绑定的 context（宿主）同样如此；表不拥有 context。 */
int lainvm_tcb_set_caps(LainVmTcb *tcb, LainVmCaps *caps, L1Diagnostic *diag);

/* 换地址空间（seL4 的 TCB_SetSpace）。
 * 只换引用，不搬内存；栈租约记着自己属于哪个空间，换过去就失效（`#alloca` 拒 1006），
 * 映像里烤进去的数据地址同理：它们不再属于新空间，访问被 1004/1005 稳定拒绝。
 * 要接着跑得由供给方在新空间里重新给一份租约、重新装载映像。
 * 运行中的 TCB 不许换。成功返回 0；失败返回非 0 并把原因写进 diag（可为 NULL）。 */
int lainvm_tcb_set_space(LainVmTcb *tcb, LainVmSpace *space, L1Diagnostic *diag);

/* 压入根帧、把 args 绑到入口过程的参数槽、置 RUNNING。
 * 成功返回 0；失败返回非 0 并把原因写进 diag（可为 NULL）。 */
int lainvm_tcb_start(LainVmTcb *tcb, const char *entry, const L1Value *args,
                     uint32_t arg_count, L1Diagnostic *diag);

#endif /* LAINVM_TCB_H */
