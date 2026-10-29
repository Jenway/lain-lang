#include "lain/vm/scheduler.h"

#ifndef LAIN_VM_EXECUTOR_H
#define LAIN_VM_EXECUTOR_H

/* LAINVM 的执行者池：把"谁跑切片"从"跑哪个切片"里分出来。
 *
 * 语义照 docs/02-lain-vm.md §3.2：
 *   - 一个执行者 = 一个可以独立跑切片的宿主线程。宿主按 CPU 核心数拉起 M 个，
 *     它们都从同一个调度器上取活——这就是 M:N 的 N。
 *   - 取活在锁内，执行在锁外。锁只保护就绪队列那几个计数器；切片本身
 *     （引擎推进指令、访存）在锁外跑，否则 M 个线程等于串行。
 *   - 取不到活就空闲，不是退出。停机由 lainvm_executor_pool_stop 显式发起。
 *
 * 这个文件不创建线程：宿主线程由调用方拉起（lainvm_executor_slice 就是
 * 线程体里该循环调用的东西）。VM 不猜宿主用什么线程库。
 */

/* 池不需要宿主锁：共享就绪队列的并发由调度器内部的原子操作保证（§3.2 的
 * 「无锁调度」）。执行者线程之间不共享任何受保护的可变状态。 */

typedef struct LainVmExecutorPool LainVmExecutorPool;

/* 池本身是定长结构，存储由调用方给；对齐要求同调度器。 */
uint32_t lainvm_executor_pool_storage_bytes(void);

/* executors 是并发上限：同时最多这么多个执行者在跑切片。 */
LainVmExecutorPool *lainvm_executor_pool_new(void *storage, uint32_t bytes,
                                            LainVmScheduler *sched,
                                            uint32_t executors);
void lainvm_executor_pool_free(LainVmExecutorPool *pool);

/* 取一个切片并跑完，是线程体的循环体。
 *
 * 返回 false 表示这一轮没取到活（队列空，或并发上限已满）——调用方应让出
 * CPU 再来，不要把它当成"执行者该退出了"。ran 区分"跑了切片"与"没活"。 */
bool lainvm_executor_slice(LainVmExecutorPool *pool, bool *ran);

/* 反复 slice 直到没活。单线程宿主用的便捷入口，等价于 drain。 */
uint32_t lainvm_executor_drain(LainVmExecutorPool *pool);

void lainvm_executor_pool_stop(LainVmExecutorPool *pool);
bool lainvm_executor_pool_stopping(const LainVmExecutorPool *pool);

uint32_t lainvm_executor_pool_capacity(const LainVmExecutorPool *pool);
uint32_t lainvm_executor_pool_busy(const LainVmExecutorPool *pool);
uint64_t lainvm_executor_pool_slices(const LainVmExecutorPool *pool);

#endif /* LAIN_VM_EXECUTOR_H */
