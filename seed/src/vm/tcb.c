/* lainvm/tcb.h 的实现：admit。
 *
 * 引擎里不许分配，所以所有上界都在这里算好并一次分配：帧栈、值槽竞技场。
 * **栈不在这里分配**：字节由供给方用 `lainvm_space_alloc_stack` 申请，
 * TCB 只接收一条租约并借用（见 tcb.h）。
 */
#include "lainvm/tcb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lainvm/image.h"

static void start_fail(L1Diagnostic *diag, int code, const char *message);

/* 校验供给方给的租约。失败返回非 0；成功时已经 borrow_count++。 */
static int accept_lease(const LainVmSpace *space, LainVmStackLease lease) {
  const LainVmRegion *region;
  if (lainvm_stack_lease_none(lease)) return 0; /* 没有栈的程序：合法 */
  if (lease.space != space) return 1; /* 句柄必须属于给定的 VSpace */
  region = lainvm_space_slot(lease.space, lease.region);
  if (!region) return 2;                              /* 区段无效 */
  if ((region->rights & (LAINVM_MEM_READ | LAINVM_MEM_WRITE)) !=
      (LAINVM_MEM_READ | LAINVM_MEM_WRITE)) return 3; /* 缺读或写 */
  if (region->accessible != 0) return 4;              /* 窗口必须从 0 开始 */
  if (region->capacity == 0) return 5;                /* 没有容量 */
  /* 一份**栈**租约只借给一条执行流：两条执行流共用一块栈、各自推进水位，
   * 会互相踩。`borrow_count` 仍是个计数（将来只读的共享映射可以用 N>1），
   * 但栈这一路今天只认独借。 */
  if (region->borrow_count != 0) return 7;
  if (!lainvm_space_borrow(lease.space, lease.region)) return 6;
  return 0;
}

LainVmTcb *lainvm_tcb_new(LainVmImage *image, LainVmSpace *space, uint64_t id,
                          uint64_t owner, uint32_t max_call_depth,
                          LainVmStackLease lease, LainVmQuota *quota) {
  LainVmTcb *tcb;
  uint32_t frame_cap;
  uint32_t slot_cap;
  uint32_t depth;
  bool borrowed;

  if (!image || !space || max_call_depth == 0) return NULL;
  depth = image->max_region_depth ? image->max_region_depth : 1;
  frame_cap = max_call_depth * (depth + 1);
  slot_cap = frame_cap * (image->max_slots ? image->max_slots : 1) + 1;

  if (accept_lease(space, lease) != 0) return NULL;
  borrowed = !lainvm_stack_lease_none(lease);

  tcb = (LainVmTcb *)calloc(1, sizeof(LainVmTcb));
  if (!tcb) {
    if (borrowed) (void)lainvm_space_end_borrow(lease.space, lease.region);
    return NULL;
  }
  tcb->id = id;
  tcb->owner = owner;
  tcb->state = LAINVM_READY;
  tcb->image = image;
  tcb->vspace = space;
  tcb->stack_lease = lease;
  tcb->stack_used = 0;
  tcb->quota = quota;
  tcb->slice_result = LAINVM_SLICE_RUNNABLE;

  tcb->frames = (LainVmFrame *)calloc(frame_cap, sizeof(LainVmFrame));
  tcb->slots = (L1Value *)calloc(slot_cap, sizeof(L1Value));
  if (!tcb->frames || !tcb->slots) {
    /* 创建中途失败：把自己取得的借用还回去，但**不**释放供给方的区段。 */
    if (borrowed) (void)lainvm_space_end_borrow(lease.space, lease.region);
    tcb->stack_lease = lainvm_stack_no_lease();
    lainvm_tcb_free(tcb);
    return NULL;
  }
  tcb->frame_cap = frame_cap;
  tcb->slot_cap = slot_cap;

  return tcb;
}

void lainvm_tcb_free(LainVmTcb *tcb) {
  if (!tcb) return;
  /* **只结束借用**：窗口收回 0，然后 borrow_count--。
   * 不撤销区段、不释放字节、不归还额度 —— 那三件事归供给方（`lainvm_space_free`）。
   * 空间从**租约自己**身上取，不能用 `tcb->vspace`：`lainvm_tcb_set_space` 可以把
   * 当前执行空间换成别的，而租约仍在原空间里（拿当前空间去找，一段都找不到，
   * 借用计数永远归不了零，供给方也就永远释放不了那块存储）。 */
  if (!lainvm_stack_lease_none(tcb->stack_lease)) {
    (void)lainvm_space_set_accessible(tcb->stack_lease.space,
                                      tcb->stack_lease.region, 0);
    (void)lainvm_space_end_borrow(tcb->stack_lease.space, tcb->stack_lease.region);
    tcb->stack_lease = lainvm_stack_no_lease();
  }
  free(tcb->frames);
  free(tcb->slots);
  free(tcb->resolved);
  free(tcb);
}

int lainvm_tcb_set_caps(LainVmTcb *tcb, LainVmCaps *caps, L1Diagnostic *diag) {
  uint32_t i;
  if (!tcb || !tcb->image) {
    start_fail(diag, 9110, "tcb: no image");
    return 1;
  }
  free(tcb->resolved);
  tcb->resolved = NULL;
  tcb->resolved_count = 0;
  tcb->caps = caps;

  if (tcb->image->sub_count == 0) return 0;
  tcb->resolved = (const LainVmCapEntry **)calloc(
      tcb->image->sub_count, sizeof(*tcb->resolved));
  if (!tcb->resolved) {
    start_fail(diag, 9111, "tcb: cannot resolve capabilities");
    return 1;
  }
  tcb->resolved_count = tcb->image->sub_count;
  for (i = 0; i < tcb->image->sub_count; i++) {
    const L1Subroutine *sub = tcb->image->subs[i].sub;
    const char *key;
    if (!(sub->flags & SUBROUTINE_EXTERN)) continue;
    key = sub->link_name ? sub->link_name : sub->name;
    tcb->resolved[i] = lainvm_caps_find(caps, key); /* 查不到就是 NULL */
  }
  return 0;
}

/* 换地址空间（seL4 的 TCB_SetSpace 那件事）。
 *
 * 只换引用，不搬内存：TCB 从此按**新**空间的区段表判权限。它手里的栈句柄带着
 * 旧空间的身份，所以在新的空间里自动失效——`#alloca` 会稳定地拒（1006），
 * 而不是悄悄用错内存。要让它在新的空间里继续跑，得由供给方在新空间里登记
 * 一段栈、再给它一份新租约。原来那块内存怎么处理归供给方，不归 TCB。
 *
 * 运行中的 TCB 不许换（先 suspend），免得"换到一半还在跑"。 */
int lainvm_tcb_set_space(LainVmTcb *tcb, LainVmSpace *space,
                         L1Diagnostic *diag) {
  if (!tcb || !space) {
    start_fail(diag, 9120, "tcb: no tcb or no address space");
    return 1;
  }
  if (tcb->state == LAINVM_RUNNING) {
    start_fail(diag, 9121, "tcb: cannot switch address space while running");
    return 1;
  }
  tcb->vspace = space;
  return 0;
}

static void start_fail(L1Diagnostic *diag, int code, const char *message) {
  if (!diag) return;
  diag->code = code;
  diag->line = 0;
  diag->column = 0;
  snprintf(diag->message, sizeof(diag->message), "%s", message);
}

int lainvm_tcb_start(LainVmTcb *tcb, const char *entry, const L1Value *args,
                     uint32_t arg_count, L1Diagnostic *diag) {
  const L1Subroutine *sub;
  const LainVmImageRegion *rec;
  uint32_t body = LAINVM_IMAGE_NO_REGION;
  LainVmFrame *frame;
  uint32_t i;

  if (!tcb || !entry) {
    start_fail(diag, 9101, "tcb: no entry");
    return 1;
  }
  sub = lainvm_image_entry(tcb->image, entry, &body);
  if (!sub || body == LAINVM_IMAGE_NO_REGION) {
    start_fail(diag, 9102, "tcb: entry not found");
    return 1;
  }
  if (arg_count != sub->param_count) {
    start_fail(diag, 9103, "tcb: argument count does not match the entry");
    return 1;
  }
  rec = lainvm_image_region(tcb->image, body);
  if (!rec || rec->slot_count > tcb->slot_cap) {
    start_fail(diag, 9104, "tcb: entry does not fit the admitted bounds");
    return 1;
  }

  memset(tcb->frames, 0, sizeof(LainVmFrame) * tcb->frame_cap);
  memset(tcb->slots, 0, sizeof(L1Value) * tcb->slot_cap);
  tcb->stack_used = 0;
  /* 新 activation 从**空窗口**开始：上一次留下的授权要是跟着新激活一起活着，
   * 授权范围就比水位大，旧地址还能访问。 */
  if (!lainvm_stack_lease_none(tcb->stack_lease)) {
    (void)lainvm_space_set_accessible(tcb->stack_lease.space,
                                      tcb->stack_lease.region, 0);
  }
  tcb->has_result = false;
  memset(&tcb->result, 0, sizeof(tcb->result));
  memset(&tcb->trap, 0, sizeof(tcb->trap));

  frame = &tcb->frames[0];
  frame->region = body;
  frame->position = 0;
  frame->slot_base = 0;
  frame->slot_count = rec->slot_count;
  frame->parent_position = 0;
  frame->label = NULL;
  frame->is_call_frame = true;
  frame->stack_mark = 0;
  tcb->frame_count = 1;

  for (i = 0; i < arg_count; i++) tcb->slots[i] = args[i];

  tcb->state = LAINVM_RUNNING;
  tcb->slice_result = LAINVM_SLICE_RUNNABLE;
  tcb->steps = 0;
  tcb->fuel = 0;
  return 0;
}
