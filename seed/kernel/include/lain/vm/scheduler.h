#ifndef LAIN_VM_SCHEDULER_H
#define LAIN_VM_SCHEDULER_H

#include "lain/ir/codes.h"
#include "lain/vm/engine.h"
#include "lain/vm/tcb.h"

/* M:N 调度（docs/02-lain-vm.md §3.2）。
 *
 * 这里只管**就绪队列与切片的归宿**，不管线程：宿主按 CPU 核心数拉起若干个
 * 执行者，每个执行者拿着同一个调度器反复取就绪 TCB 跑一个切片。队列本身没有
 * 锁，也**不做任何分配**——容量和存储都由调用方给，跟引擎"不许 malloc"的约定
 * 一致。跨线程使用时由宿主自己串行化对 admit / wake / next 的调用。
 *
 * 切片的四种归宿在这里都有确切处置：
 *   RUNNABLE → 回队尾（它还有事做，别让它逃出队列）
 *   BLOCKED  → 出队且**不回队**，等端点事件；端点就绪后由 `lainvm_scheduler_wake`
 *              放回队尾。端点本身（§3.1）不在这一层，所以调度器只记住"它阻塞了"。
 *   DONE     → 出队，计入完成数
 *   TRAPPED  → 出队，计入捕获数；fault_handler 不在这一层
 * 只有 RUNNABLE 会重新入队，其余三种都必须由外部再次 admit / wake 才能回来——
 * 否则一个 Trap 过的 TCB 会被无限重跑，trap 就白记了。 */

typedef enum {
  LAINVM_SCHED_ERR_FULL = 9501,      /* 就绪队列满，这次 admit/wake 被拒 */
  LAINVM_SCHED_ERR_DUPLICATE = 9502, /* 同一个 TCB 已经在就绪队列里 */
  LAINVM_SCHED_ERR_NOT_READY = 9503, /* TCB 不在 RUNNING，入队没有意义 */
  LAINVM_SCHED_ERR_CAPACITY = 9504,  /* 容量为 0，或存储不够放下这么多槽 */
  LAINVM_SCHED_ERR_NULL = 9505,      /* 调度器、存储或 TCB 为 NULL */
} LainVmSchedStatus;

_Static_assert(LAINVM_SCHED_ERR_FULL > LAINIR_CODES_SCHED_BASE &&
                   LAINVM_SCHED_ERR_NULL < LAINIR_CODES_SCHED_LIMIT,
               "scheduler codes must stay inside the sched segment");

typedef struct LainVmScheduler LainVmScheduler;

/* 放下 capacity 个槽需要的字节数；调用方按它准备存储。 */
uint32_t lainvm_scheduler_storage_bytes(uint32_t capacity);

/* 在调用方给的存储上就地构造。capacity 个就绪槽，fuel 是每个切片发给引擎的
 * 指令预算（0 视为 1——没有预算的切片等于不让位，会让 M>1 退化回串行）。
 * 存储必须按 alignof(void *) 对齐。返回 NULL 表示容量或存放下不去。 */
LainVmScheduler *lainvm_scheduler_new(void *storage, uint32_t bytes,
                                      uint32_t capacity, uint64_t fuel);

/* 入队尾。TCB 必须处于 RUNNING（即已 `lainvm_tcb_start`），已在队里则拒绝。 */
int lainvm_scheduler_admit(LainVmScheduler *sched, LainVmTcb *tcb,
                           L1Diagnostic *diag);

/* BLOCKED 的 TCB 在端点事件就绪后由这里放回队尾，随后照常被选中。
 * 已经在队里同样拒绝——重复入队会让它在一轮里被跑两次，切片就不公平了。 */
int lainvm_scheduler_wake(LainVmScheduler *sched, LainVmTcb *tcb,
                          L1Diagnostic *diag);

/* 取一个就绪 TCB 出队并把它标成 RUNNING。队列空返回 NULL。
 *
 * 拆出来是为了让执行者池把"取活"和"执行"放进不同的临界区：取活在锁内，
 * 执行在锁外。取出来但没 retire 的 TCB 不在任何队列里，调用方必须要么
 * 自己跑完再 retire，要么原样放回——悬空的 TCB 会从这个 VM 里彻底消失。 */
LainVmTcb *lainvm_scheduler_claim(LainVmScheduler *sched);

/* 结算一个切片的归宿：RUNNABLE 回队尾、BLOCKED/DONE/TRAPPED 只计数。 */
void lainvm_scheduler_retire(LainVmScheduler *sched, LainVmTcb *tcb,
                             LainVmSliceResult result);

/* 每个切片发给引擎的指令预算。 */
uint64_t lainvm_scheduler_fuel(const LainVmScheduler *sched);

/* 以上三个入口各自不加锁：claim 与 retire 之间必须由调用方保证互斥
 * （见 lain/vm/executor.h）。lainvm_scheduler_step 是它们的单线程组合。 */

/* 取一个就绪 TCB 跑一个切片并处置它的归宿。返回这次切片的归宿；
 * 队列空时返回 LAINVM_SLICE_DONE 并把 *ran 置 0。 */
LainVmSliceResult lainvm_scheduler_step(LainVmScheduler *sched, bool *ran);

/* 反复 step 直到就绪队列空。返回执行的切片数。BLOCKED 的 TCB 会让队列变空，
 * 所以返回不代表"所有激活都结束了"——要看 `lainvm_scheduler_done_count`。 */
uint32_t lainvm_scheduler_drain(LainVmScheduler *sched);

uint32_t lainvm_scheduler_ready_count(const LainVmScheduler *sched);
uint32_t lainvm_scheduler_done_count(const LainVmScheduler *sched);
uint32_t lainvm_scheduler_blocked_count(const LainVmScheduler *sched);
uint32_t lainvm_scheduler_trapped_count(const LainVmScheduler *sched);
uint64_t lainvm_scheduler_slice_count(const LainVmScheduler *sched);

#endif /* LAIN_VM_SCHEDULER_H */
