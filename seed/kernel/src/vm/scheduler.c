/* LAINVM 的就绪队列：无锁 MPMC 环形队列（docs/02-lain-vm.md §3.2
 * 「由无状态执行引擎池在宿主多核物理线程间无锁调度」「从共享就绪队列中窃取」）。
 *
 * 队列本身就是共享就绪队列，执行者从同一处取活；队列的每个槽带一个序号，
 * 入队/出队各自 CAS 位置，所以入队与出队可以并发进行，不需要宿主锁。
 * 不分配：槽数组跟在结构体后面，存储由调用方给。
 *
 * 计数器全是原子量，读取函数可以在任何线程调用。
 */

#include "lain/vm/scheduler.h"

#include "tcb_internal.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

typedef struct LainVmSlot {
  _Atomic uint64_t seq;
  LainVmTcb *tcb; /* 由 seq 的 acquire/release 保护，自身不原子 */
} LainVmSlot;

struct LainVmScheduler {
  uint32_t capacity; /* 始终是 2 的幂：取模退化成按位与 */
  uint32_t mask;
  _Atomic uint64_t enqueue_pos;
  _Atomic uint64_t dequeue_pos;
  uint64_t fuel;
  _Atomic uint32_t live; /* 队列里当前的激活数 */
  _Atomic uint32_t done;
  _Atomic uint32_t blocked;
  _Atomic uint32_t trapped;
  _Atomic uint64_t slices;
  LainVmSlot slots[];
};

static uint32_t sched_round_capacity(uint32_t capacity) {
  uint32_t pow2 = 1;

  if (capacity == 0) return 1;
  while (pow2 < capacity) pow2 <<= 1;
  return pow2;
}

uint32_t lainvm_scheduler_storage_bytes(uint32_t capacity) {
  return (uint32_t)(sizeof(LainVmScheduler) +
                    (size_t)sched_round_capacity(capacity) * sizeof(LainVmSlot));
}

LainVmScheduler *lainvm_scheduler_new(void *storage, uint32_t bytes,
                                      uint32_t capacity, uint64_t fuel) {
  LainVmScheduler *sched;
  uint32_t i;

  if (!storage || capacity == 0) return NULL;
  if (bytes < lainvm_scheduler_storage_bytes(capacity)) return NULL;

  sched = (LainVmScheduler *)storage;
  memset(sched, 0, lainvm_scheduler_storage_bytes(capacity));
  sched->capacity = sched_round_capacity(capacity);
  sched->mask = sched->capacity - 1;
  sched->fuel = fuel ? fuel : 1;
  atomic_init(&sched->enqueue_pos, 0);
  atomic_init(&sched->dequeue_pos, 0);
  atomic_init(&sched->live, 0);
  atomic_init(&sched->done, 0);
  atomic_init(&sched->blocked, 0);
  atomic_init(&sched->trapped, 0);
  atomic_init(&sched->slices, 0);
  for (i = 0; i < sched->capacity; i++) atomic_init(&sched->slots[i].seq, i);
  return sched;
}

static int sched_reject(L1Diagnostic *diag, int32_t code, const char *message) {
  if (diag) {
    memset(diag, 0, sizeof(*diag));
    diag->code = code;
    snprintf(diag->message, sizeof(diag->message), "%s", message);
  }
  return code;
}

/* 入队：位置用 CAS 抢，槽位用序号判断是不是轮到自己写。队列满返回 false。 */
static bool sched_push_tail(LainVmScheduler *sched, LainVmTcb *tcb) {
  uint64_t pos = atomic_load_explicit(&sched->enqueue_pos, memory_order_relaxed);
  LainVmSlot *slot;
  uint64_t seq;
  int64_t diff;

  for (;;) {
    slot = &sched->slots[pos & sched->mask];
    seq = atomic_load_explicit(&slot->seq, memory_order_acquire);
    diff = (int64_t)seq - (int64_t)pos;
    if (diff == 0) {
      if (atomic_compare_exchange_weak_explicit(&sched->enqueue_pos, &pos, pos + 1,
                                                memory_order_relaxed,
                                                memory_order_relaxed))
        break;
    } else if (diff < 0) {
      return false; /* 槽还没被腾出来：队列满 */
    } else {
      pos = atomic_load_explicit(&sched->enqueue_pos, memory_order_relaxed);
    }
  }
  slot->tcb = tcb;
  atomic_store_explicit(&slot->seq, pos + 1, memory_order_release);
  atomic_fetch_add_explicit(&sched->live, 1, memory_order_release);
  return true;
}

static LainVmTcb *sched_pop_head(LainVmScheduler *sched) {
  uint64_t pos = atomic_load_explicit(&sched->dequeue_pos, memory_order_relaxed);
  LainVmSlot *slot;
  uint64_t seq;
  int64_t diff;
  LainVmTcb *tcb;

  for (;;) {
    slot = &sched->slots[pos & sched->mask];
    seq = atomic_load_explicit(&slot->seq, memory_order_acquire);
    diff = (int64_t)seq - (int64_t)(pos + 1);
    if (diff == 0) {
      if (atomic_compare_exchange_weak_explicit(&sched->dequeue_pos, &pos, pos + 1,
                                                memory_order_relaxed,
                                                memory_order_relaxed))
        break;
    } else if (diff < 0) {
      return NULL; /* 槽里还没有东西：队列空 */
    } else {
      pos = atomic_load_explicit(&sched->dequeue_pos, memory_order_relaxed);
    }
  }
  tcb = slot->tcb;
  slot->tcb = NULL;
  atomic_store_explicit(&slot->seq, pos + sched->capacity, memory_order_release);
  atomic_fetch_sub_explicit(&sched->live, 1, memory_order_release);
  return tcb;
}

static int sched_enqueue(LainVmScheduler *sched, LainVmTcb *tcb,
                         L1Diagnostic *diag) {
  /* 在队标记用原子交换抢：并发下两个线程同时入队同一个激活，只有一个能赢。 */
  if (atomic_exchange_explicit(&tcb->queued, true, memory_order_acq_rel))
    return sched_reject(diag, LAINVM_SCHED_ERR_DUPLICATE, "tcb is already ready");
  if (!sched_push_tail(sched, tcb)) {
    atomic_store_explicit(&tcb->queued, false, memory_order_release);
    return sched_reject(diag, LAINVM_SCHED_ERR_FULL, "ready queue is full");
  }
  return 0;
}

int lainvm_scheduler_admit(LainVmScheduler *sched, LainVmTcb *tcb,
                           L1Diagnostic *diag) {
  if (!sched || !tcb) return sched_reject(diag, LAINVM_SCHED_ERR_NULL, "scheduler or tcb is null");
  if (lainvm_tcb_state(tcb) != LAINVM_RUNNING)
    return sched_reject(diag, LAINVM_SCHED_ERR_NOT_READY,
                        "tcb is not running; call tcb_start first");
  return sched_enqueue(sched, tcb, diag);
}

int lainvm_scheduler_wake(LainVmScheduler *sched, LainVmTcb *tcb,
                          L1Diagnostic *diag) {
  LainVmState state;

  if (!sched || !tcb) return sched_reject(diag, LAINVM_SCHED_ERR_NULL, "scheduler or tcb is null");
  /* 端点会先把挂起方标成 READY（§3.1），所以这里是 READY 与 RUNNING 都收。
   * BLOCKED 仍然在等对手，DEAD 已经结束——这两个入队没有意义。 */
  state = lainvm_tcb_state(tcb);
  if (state != LAINVM_RUNNING && state != LAINVM_READY)
    return sched_reject(diag, LAINVM_SCHED_ERR_NOT_READY,
                        "tcb is neither running nor ready; a finished or trapped activation cannot be woken");
  return sched_enqueue(sched, tcb, diag);
}

LainVmTcb *lainvm_scheduler_claim(LainVmScheduler *sched) {
  LainVmTcb *tcb;

  if (!sched) return NULL;
  tcb = sched_pop_head(sched);
  if (!tcb) return NULL;

  atomic_store_explicit(&tcb->queued, false, memory_order_release);
  /* 选中即运行：READY → RUNNING。被端点唤醒的激活是 READY 进队的，引擎只肯
   * 推进 RUNNING 的 TCB，这一步不做就会空转。 */
  tcb->state = LAINVM_RUNNING;
  return tcb;
}

void lainvm_scheduler_retire(LainVmScheduler *sched, LainVmTcb *tcb,
                             LainVmSliceResult result) {
  if (!sched || !tcb) return;

  atomic_fetch_add_explicit(&sched->slices, 1, memory_order_relaxed);
  switch (result) {
    case LAINVM_SLICE_RUNNABLE: {
      /* 预算用完了但没结束：放回队尾，让排在后面的执行流先走。必须走 enqueue
       * 而不是 push：在队标记要一起恢复，否则这个激活「人在队里、标记是假」，
       * 下一次重复入队拦不住，同一个 TCB 会被跑两遍。 */
      int rc = sched_enqueue(sched, tcb, NULL);
      while (rc == LAINVM_SCHED_ERR_FULL) {
        /* 队满说明就绪激活数超过了调用方给的容量。别的执行者一定在推进，
         * 等到有人取走一个就有位置——不丢激活，也不返回半截状态。 */
        rc = sched_enqueue(sched, tcb, NULL);
      }
      break;
    }
    case LAINVM_SLICE_BLOCKED:
      atomic_fetch_add_explicit(&sched->blocked, 1, memory_order_relaxed);
      break;
    case LAINVM_SLICE_DONE:
      atomic_fetch_add_explicit(&sched->done, 1, memory_order_relaxed);
      break;
    case LAINVM_SLICE_TRAPPED:
    default:
      atomic_fetch_add_explicit(&sched->trapped, 1, memory_order_relaxed);
      break;
  }
}

uint64_t lainvm_scheduler_fuel(const LainVmScheduler *sched) {
  return sched ? sched->fuel : 0;
}

LainVmSliceResult lainvm_scheduler_step(LainVmScheduler *sched, bool *ran) {
  LainVmTcb *tcb;
  LainVmSliceResult result;

  if (ran) *ran = false;
  if (!sched) return LAINVM_SLICE_DONE;

  tcb = lainvm_scheduler_claim(sched);
  if (!tcb) return LAINVM_SLICE_DONE;

  if (ran) *ran = true;
  result = lainvm_engine_run(tcb, sched->fuel);
  lainvm_scheduler_retire(sched, tcb, result);
  return result;
}

uint32_t lainvm_scheduler_drain(LainVmScheduler *sched) {
  uint32_t slices = 0;
  bool ran = false;

  if (!sched) return 0;
  do {
    (void)lainvm_scheduler_step(sched, &ran);
    if (ran) slices++;
  } while (ran);
  return slices;
}

uint32_t lainvm_scheduler_ready_count(const LainVmScheduler *sched) {
  return sched ? atomic_load_explicit(&sched->live, memory_order_acquire) : 0;
}

uint32_t lainvm_scheduler_done_count(const LainVmScheduler *sched) {
  return sched ? atomic_load_explicit(&sched->done, memory_order_acquire) : 0;
}

uint32_t lainvm_scheduler_blocked_count(const LainVmScheduler *sched) {
  return sched ? atomic_load_explicit(&sched->blocked, memory_order_acquire) : 0;
}

uint32_t lainvm_scheduler_trapped_count(const LainVmScheduler *sched) {
  return sched ? atomic_load_explicit(&sched->trapped, memory_order_acquire) : 0;
}

uint64_t lainvm_scheduler_slice_count(const LainVmScheduler *sched) {
  return sched ? atomic_load_explicit(&sched->slices, memory_order_acquire) : 0;
}
