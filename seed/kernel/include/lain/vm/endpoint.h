/* LAINVM 的端点：执行流之间的会合点（IPC rendezvous）。
 *
 * 端点对象本身还没实现，第一期只保留对象身份与结果码。契约如下：
 *   - 一次会合 = 发送方与接收方在同一个端点上相遇，各自得到一次
 *     LAINVM_ENDPOINT_SEND / LAINVM_ENDPOINT_RECEIVE；
 *   - 等待是 TCB 的状态：没有对手时阻塞，原因记 LAINVM_SUSPEND_ENDPOINT
 *     （见 lain/vm/tcb.h），等待的 TCB 挂在端点上，由 scheduler 重新选中；
 *   - 谁是下一个、队列多长，属于调度策略，这里不规定；
 *   - 会合被取消报 LAINVM_ENDPOINT_CANCELLED。
 * 端点不解释消息语义：交接的就是 TCB 上已有的 pending 值。
 */
#ifndef LAINVM_ENDPOINT_H
#define LAINVM_ENDPOINT_H

typedef struct LainVmTcb LainVmTcb;
typedef struct LainVmEndpoint LainVmEndpoint;

/* 会合结果 */
enum {
  LAINVM_ENDPOINT_SEND = 1,
  LAINVM_ENDPOINT_RECEIVE = 2,
  LAINVM_ENDPOINT_CANCELLED = 3,
};

/* 最小接口。定义留给 scheduler 那一层，现在只是契约：阻塞版本在会合完成前
 * 不返回，nb_ 版本没有对手时立刻返回非 0。 */
LainVmEndpoint *lainvm_endpoint_new(void);
void lainvm_endpoint_free(LainVmEndpoint *endpoint);
int lainvm_endpoint_send(LainVmEndpoint *endpoint, LainVmTcb *tcb);
int lainvm_endpoint_receive(LainVmEndpoint *endpoint, LainVmTcb *tcb);
int lainvm_endpoint_nb_send(LainVmEndpoint *endpoint, LainVmTcb *tcb);
int lainvm_endpoint_nb_receive(LainVmEndpoint *endpoint, LainVmTcb *tcb);

#endif /* LAINVM_ENDPOINT_H */
