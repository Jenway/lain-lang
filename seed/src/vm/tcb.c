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
                          uint64_t stack_bytes) {
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
  tcb->stack_region = LAINVM_SPACE_NO_REGION;
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
    /* 清零是提供内存这一方的责任：不零就没有确定性。 */
    void *stack = calloc(1, (size_t)stack_bytes);
    int32_t index;
    if (!stack) {
      lainvm_tcb_free(tcb);
      return NULL;
    }
    index = lainvm_space_add_region(space, (uintptr_t)stack, stack_bytes,
                                    LAINVM_MEM_READ | LAINVM_MEM_WRITE, id);
    if (index == LAINVM_SPACE_NO_REGION) {
      free(stack);
      lainvm_tcb_free(tcb);
      return NULL;
    }
    tcb->stack_region = index;
    /* 区段表是定长的，插入后下标可能变；按 owner 找回自己的那段。 */
    {
      uint32_t i;
      for (i = 0; i < space->region_count; i++) {
        if (space->regions[i].owner == id && space->regions[i].base == (uintptr_t)stack) {
          tcb->stack_region = (int32_t)i;
          break;
        }
      }
    }
  }

  return tcb;
}

void lainvm_tcb_free(LainVmTcb *tcb) {
  if (!tcb) return;
  if (tcb->vspace && tcb->stack_region != LAINVM_SPACE_NO_REGION) {
    /* 栈的字节由本 TCB 分配，先取回地址再让区段消失。 */
    uintptr_t base = tcb->vspace->regions[tcb->stack_region].base;
    lainvm_space_release_owner(tcb->vspace, tcb->id);
    free((void *)base);
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
