/* LAINVM 的端点：执行流之间的会合点（IPC rendezvous）。
 *
 * 语义照 docs/02-lain-vm.md §3.1 与 lain/vm/endpoint.h 的契约：
 *   - **无缓冲**：端点不排队数据，也不排队 TCB。它只记住"哪一侧已经到达并挂起"，
 *     一次会合 = 一个发送方与一个接收方在同一端点上相遇，两侧各得一次结果。
 *   - **让出挂起**：没有对手时，到达方进入 BLOCKED，原因记 LAINVM_SUSPEND_ENDPOINT，
 *     blocked_on 指向端点本身；对手到达时直接完成交接，把挂起方标记 READY 并交回
 *     调度器（文档原文「同时将 TCB_A 标记为 READY」）。
 *   - **语义中立**：交接的只是 TCB 上已有的 pending 值，端点不解释它是什么。
 *
 * 关于"阻塞版本"：Lain-VM 是协作式的，宿主线程不能真的停在这里等。所以阻塞版本
 * 的"不返回"实现为**让出**——挂起 TCB 后返回 0，等对手到达把这次会合结算掉，
 * TCB 被重新选中时再进同一个调用，这时返回 LAINVM_ENDPOINT_SEND / _RECEIVE。
 * 这个"已结算的结果"记在 TCB 的 endpoint_result 上，取走即清空，所以一次会合
 * 只结算一次。nb_ 版本从不让出：没对手就返回 0，TCB 状态不动。
 *
 * 端点对象本身是唯一会分配的东西（lainvm_endpoint_new 的签名如此），
 * 每个端点一个对象，之后不再分配。 */
#include "lain/vm/endpoint.h"

#include <stdlib.h>
#include <string.h>

#include "lain/vm/scheduler.h"
#include "lain/vm/tcb.h"
#include "tcb_internal.h"

struct LainVmEndpoint {
  LainVmScheduler *sched; /* 可选：唤醒时直接回就绪队列 */
  LainVmTcb *sender;      /* 已到达并挂起的发送方，没有则为 NULL */
  LainVmTcb *receiver;    /* 已到达并挂起的接收方，没有则为 NULL */
  uint32_t meetings;      /* 完成的会合数（诊断用） */
  uint32_t parks;         /* 到达后被挂起的次数（诊断用） */
};

LainVmEndpoint *lainvm_endpoint_new(void) {
  return (LainVmEndpoint *)calloc(1, sizeof(LainVmEndpoint));
}

void lainvm_endpoint_free(LainVmEndpoint *endpoint) {
  free(endpoint);
}

void lainvm_endpoint_attach(LainVmEndpoint *endpoint, LainVmScheduler *sched) {
  if (endpoint) endpoint->sched = sched;
}

uint32_t lainvm_endpoint_meetings(const LainVmEndpoint *endpoint) {
  return endpoint ? endpoint->meetings : 0;
}

uint32_t lainvm_endpoint_parks(const LainVmEndpoint *endpoint) {
  return endpoint ? endpoint->parks : 0;
}

/* 挂起：状态、原因、归属一次写完，别的地方只读。 */
static void endpoint_park(LainVmEndpoint *endpoint, LainVmTcb *tcb,
                          uint32_t side) {
  tcb->state = LAINVM_BLOCKED;
  tcb->suspend_reason = LAINVM_SUSPEND_ENDPOINT;
  tcb->blocked_on = endpoint;
  tcb->pending_kind = side;
  endpoint->parks++;
}

/* 唤醒：挂起方回到 READY，并把这次会合的结果记在它身上。 */
static void endpoint_wake(LainVmEndpoint *endpoint, LainVmTcb *tcb,
                          uint32_t result) {
  tcb->state = LAINVM_READY;
  tcb->suspend_reason = 0;
  tcb->blocked_on = NULL;
  tcb->pending_kind = 0;
  tcb->endpoint_result = result;
  if (endpoint->sched) {
    L1Diagnostic diag;
    memset(&diag, 0, sizeof diag);
    /* 队列满时它仍是 READY，只是这一轮没进去；宿主会再试。
     * 端点不能自己决定丢掉一个 READY 的激活。 */
    (void)lainvm_scheduler_wake(endpoint->sched, tcb, &diag);
  }
}

/* 会合：把发送方已有的 pending 值交给接收方。端点不解释值的含义。
 *
 * 两侧都记下结果，但**只有挂起过的那一侧**回到就绪队列——刚到的一侧没有挂起，
 * 它的调用马上返回，把它塞进队列会让它在一轮里被跑两次。 */
static void endpoint_handover(LainVmEndpoint *endpoint, LainVmTcb *sender,
                              LainVmTcb *receiver, LainVmTcb *parked) {
  receiver->pending_kind = sender->pending_kind;
  receiver->pending_value = sender->pending_value;
  receiver->has_pending = sender->has_pending;
  sender->has_pending = false;
  endpoint->sender = NULL;
  endpoint->receiver = NULL;
  endpoint->meetings++;
  sender->endpoint_result = LAINVM_ENDPOINT_SEND;
  receiver->endpoint_result = LAINVM_ENDPOINT_RECEIVE;
  endpoint_wake(endpoint, parked, parked->endpoint_result);
}

/* 一次到达的公共流程。side 是 LAINVM_ENDPOINT_SEND 或 _RECEIVE。 */
static int endpoint_arrive(LainVmEndpoint *endpoint, LainVmTcb *tcb,
                           uint32_t side, bool blocking) {
  LainVmTcb **own = (side == LAINVM_ENDPOINT_SEND) ? &endpoint->sender
                                                   : &endpoint->receiver;
  LainVmTcb **peer = (side == LAINVM_ENDPOINT_SEND) ? &endpoint->receiver
                                                    : &endpoint->sender;

  /* 参数不成立时没有会合可言，按取消交付。 */
  if (!endpoint || !tcb) return LAINVM_ENDPOINT_CANCELLED;

  /* 上一次挂起已经结算过了：这次进入同一个调用就是回来取结果的。 */
  if (tcb->endpoint_result != 0) {
    int done = (int)tcb->endpoint_result;
    tcb->endpoint_result = 0;
    return done;
  }

  /* 已经挂在这个端点上：不许重复挂，也不许自会合。 */
  if (*own == tcb) return 0;
  if (*peer == tcb) return LAINVM_ENDPOINT_CANCELLED;

  if (*peer != NULL) {
    LainVmTcb *other = *peer;
    *peer = NULL;
    *own = NULL;
    if (side == LAINVM_ENDPOINT_SEND)
      endpoint_handover(endpoint, tcb, other, other);
    else
      endpoint_handover(endpoint, other, tcb, other);
    /* 自己这一侧也会被 endpoint_meet 结算，结果就在 endpoint_result 上；
     * 直接取走返回，别让它留到下一次进入同一个调用。 */
    {
      int done = (int)tcb->endpoint_result;
      tcb->endpoint_result = 0;
      return done;
    }
  }

  if (!blocking) return 0;
  *own = tcb;
  endpoint_park(endpoint, tcb, side);
  return 0;
}

int lainvm_endpoint_send(LainVmEndpoint *endpoint, LainVmTcb *tcb) {
  return endpoint_arrive(endpoint, tcb, LAINVM_ENDPOINT_SEND, true);
}

int lainvm_endpoint_receive(LainVmEndpoint *endpoint, LainVmTcb *tcb) {
  return endpoint_arrive(endpoint, tcb, LAINVM_ENDPOINT_RECEIVE, true);
}

int lainvm_endpoint_nb_send(LainVmEndpoint *endpoint, LainVmTcb *tcb) {
  return endpoint_arrive(endpoint, tcb, LAINVM_ENDPOINT_SEND, false);
}

int lainvm_endpoint_nb_receive(LainVmEndpoint *endpoint, LainVmTcb *tcb) {
  return endpoint_arrive(endpoint, tcb, LAINVM_ENDPOINT_RECEIVE, false);
}
