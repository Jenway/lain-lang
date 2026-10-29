/* 执行者池：宿主线程从这里取活，VM 不创建线程（docs/02-lain-vm.md §3.2）。
 *
 * 无锁：队列的并发由调度器内部的原子操作保证，这里不接任何宿主锁。切片的编排是
 * 「原子取活 → 无锁执行 → 原子回收」，所以 M 个执行者可以真正并行推进各自的
 * Engine，而不是轮流拿同一把锁。
 */

#include "lain/vm/executor.h"

#include <stdatomic.h>
#include <string.h>

struct LainVmExecutorPool {
  LainVmScheduler *sched;
  uint32_t capacity;
  _Atomic uint32_t busy;     /* 正在跑切片的执行者数：并发上限就是它 */
  _Atomic uint32_t stopping;
  _Atomic uint64_t slices;
};

uint32_t lainvm_executor_pool_storage_bytes(void) {
  return (uint32_t)sizeof(LainVmExecutorPool);
}

LainVmExecutorPool *lainvm_executor_pool_new(void *storage, uint32_t bytes,
                                            LainVmScheduler *sched,
                                            uint32_t executors) {
  LainVmExecutorPool *pool;

  if (!storage || !sched) return NULL;
  if (executors == 0) return NULL;
  if (bytes < lainvm_executor_pool_storage_bytes()) return NULL;

  pool = (LainVmExecutorPool *)storage;
  memset(pool, 0, sizeof(*pool));
  pool->sched = sched;
  pool->capacity = executors;
  atomic_init(&pool->busy, 0);
  atomic_init(&pool->stopping, 0);
  atomic_init(&pool->slices, 0);
  return pool;
}

void lainvm_executor_pool_free(LainVmExecutorPool *pool) {
  /* 存储归调用方，这里只把状态清掉：拿回存储的人不该看到半截的池。 */
  if (!pool) return;
  memset(pool, 0, sizeof(*pool));
}

bool lainvm_executor_slice(LainVmExecutorPool *pool, bool *ran) {
  LainVmTcb *tcb;
  LainVmSliceResult result;
  uint32_t busy;
  uint64_t fuel;

  if (ran) *ran = false;
  if (!pool) return false;
  if (atomic_load_explicit(&pool->stopping, memory_order_acquire)) return false;

  /* 并发上限：抢到位子才去取活，否则让给别人。 */
  busy = atomic_load_explicit(&pool->busy, memory_order_relaxed);
  for (;;) {
    if (busy >= pool->capacity) return false;
    if (atomic_compare_exchange_weak_explicit(&pool->busy, &busy, busy + 1,
                                              memory_order_acq_rel,
                                              memory_order_relaxed))
      break;
  }

  tcb = lainvm_scheduler_claim(pool->sched);
  if (!tcb) {
    atomic_fetch_sub_explicit(&pool->busy, 1, memory_order_release);
    return false;
  }
  fuel = lainvm_scheduler_fuel(pool->sched);

  /* 执行期间不持任何锁。 */
  result = lainvm_engine_run(tcb, fuel);

  lainvm_scheduler_retire(pool->sched, tcb, result);
  atomic_fetch_sub_explicit(&pool->busy, 1, memory_order_release);
  atomic_fetch_add_explicit(&pool->slices, 1, memory_order_relaxed);

  if (ran) *ran = true;
  return true;
}

uint32_t lainvm_executor_drain(LainVmExecutorPool *pool) {
  uint32_t taken = 0;
  bool ran = false;

  if (!pool) return 0;
  while (lainvm_executor_slice(pool, &ran)) {
    if (ran) taken++;
  }
  return taken;
}

void lainvm_executor_pool_stop(LainVmExecutorPool *pool) {
  if (!pool) return;
  atomic_store_explicit(&pool->stopping, 1, memory_order_release);
}

bool lainvm_executor_pool_stopping(const LainVmExecutorPool *pool) {
  return pool ? atomic_load_explicit(&pool->stopping, memory_order_acquire) != 0 : true;
}

uint32_t lainvm_executor_pool_capacity(const LainVmExecutorPool *pool) {
  return pool ? pool->capacity : 0;
}

uint32_t lainvm_executor_pool_busy(const LainVmExecutorPool *pool) {
  return pool ? atomic_load_explicit(&pool->busy, memory_order_acquire) : 0;
}

uint64_t lainvm_executor_pool_slices(const LainVmExecutorPool *pool) {
  return pool ? atomic_load_explicit(&pool->slices, memory_order_acquire) : 0;
}
