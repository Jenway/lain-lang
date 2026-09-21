/* lainvm/tcb.h 的实现：admit。
 *
 * 引擎里不许分配，所以所有上界都在这里算好并一次分配：
 * 帧栈、值槽竞技场、alloca 用的栈区段。
 */
#include "lainvm/tcb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lainvm/image.h"

static void start_fail(L1Diagnostic *diag, int code, const char *message);

LainVmTcb *lainvm_tcb_new(LainVmImage *image, LainVmSpace *space, uint64_t id,
                          uint64_t owner, uint32_t max_call_depth,
                          uint64_t stack_bytes, LainVmQuota *quota) {
  LainVmTcb *tcb;
  uint32_t frame_cap;
  uint32_t slot_cap;
  uint32_t depth;

  if (!image || !space || max_call_depth == 0) return NULL;
  depth = image->max_region_depth ? image->max_region_depth : 1;
  frame_cap = max_call_depth * (depth + 1);
  slot_cap = frame_cap * (image->max_slots ? image->max_slots : 1) + 1;

  tcb = (LainVmTcb *)calloc(1, sizeof(LainVmTcb));
  if (!tcb) return NULL;
  tcb->id = id;
  tcb->owner = owner;
  tcb->state = LAINVM_READY;
  tcb->image = image;
  tcb->vspace = space;
  tcb->stack_space = NULL;
  tcb->stack_base = 0;
  tcb->stack_size = 0;
  tcb->stack_used = 0;
  tcb->stack_window = lainvm_space_no_handle();
  tcb->stack_window_size = 0;
  tcb->quota = quota;
  tcb->slice_result = LAINVM_SLICE_RUNNABLE;

  tcb->frames = (LainVmFrame *)calloc(frame_cap, sizeof(LainVmFrame));
  tcb->slots = (L1Value *)calloc(slot_cap, sizeof(L1Value));
  if (!tcb->frames || !tcb->slots) {
    lainvm_tcb_free(tcb);
    return NULL;
  }
  tcb->frame_cap = frame_cap;
  tcb->slot_cap = slot_cap;

  if (stack_bytes > 0) {
    /* 先预扣**整块容量**（规范：栈整块预留时只在预留那一刻扣一次），再分配。
     * 预扣失败就什么都不留；预扣成功而分配失败必须**回滚**，否则"分配失败"
     * 会留下抹不掉的扣账。 */
    if (lainvm_quota_charge(quota, stack_bytes) != 0) {
      lainvm_tcb_free(tcb);
      return NULL;
    }
    /* 清零是提供内存这一方的责任：不零就没有确定性。 */
    void *stack = calloc(1, (size_t)stack_bytes);
    if (!stack) {
      (void)lainvm_quota_release(quota, stack_bytes);
      lainvm_tcb_free(tcb);
      return NULL;
    }
    /* 只记租约，**不登记区段**：授权跟着活窗口走（见 tcb.h）。整块登记会让返回后
     * 的旧地址仍然可访问，那就只能靠地址身份去拦 —— 而 `#addr` 没有身份。 */
    tcb->stack_space = space;
    tcb->stack_base = (uintptr_t)stack;
    tcb->stack_size = stack_bytes;
  }

  return tcb;
}

void lainvm_tcb_free(LainVmTcb *tcb) {
  if (!tcb) return;
  /* 先撤**活窗口**。空间从**句柄自己身上**取，不能用 `tcb->vspace`：句柄带着它
   * 所在空间的身份，而 `tcb->vspace` 可能已经被 set_space 换成别的空间了。实测
   * （用例 cap_tcb_destroy_after_switch）：换空间后销毁 TCB，拿当前空间去撤销
   * 一段都撤不掉 —— 窗口留在原地、栈内存再也没人 free，静默泄漏。
   *
   * 这里原来还有一处「按缓存的区段下标去取地址」——下标被别的插入挪走之后取回来
   * 的是别人的地址，free 直接堆损坏（0xC0000374，实测 R03）。 */
  if (!lainvm_space_handle_none(tcb->stack_window)) {
    LainVmSpace *window_space = (LainVmSpace *)(uintptr_t)tcb->stack_window.space;
    lainvm_space_unmap_external(window_space, tcb->stack_window);
    tcb->stack_window = lainvm_space_no_handle();
    tcb->stack_window_size = 0;
  }
  /* 字节由本 TCB 分配，也由本 TCB 还（这一版供给方就是本 TCB）。
   * **真的还回去了**才归还额度：水位回退不算归还。 */
  if (tcb->stack_base != 0) {
    free((void *)tcb->stack_base);
    (void)lainvm_quota_release(tcb->quota, tcb->stack_size);
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
  /* 重启时活窗口必须归零：上一次激活留下的授权要是跟着新激活一起活着，
   * 授权范围就比水位大，旧地址还能访问。 */
  if (!lainvm_space_handle_none(tcb->stack_window)) {
    lainvm_space_unmap_external((LainVmSpace *)(uintptr_t)tcb->stack_window.space,
                        tcb->stack_window);
    tcb->stack_window = lainvm_space_no_handle();
  }
  tcb->stack_window_size = 0;
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
