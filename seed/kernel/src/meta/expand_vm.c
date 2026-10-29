#include "expand_vm.h"

#include <string.h>

static L1Value scalar(uint64_t bits) {
  L1Value value;
  memset(&value, 0, sizeof value);
  value.kind = L1_VALUE_BITS;
  value.bit_width = 64;
  value.as.bits = bits;
  return value;
}

static L1Value address(uintptr_t addr) {
  L1Value value;
  memset(&value, 0, sizeof value);
  value.kind = L1_VALUE_ADDR;
  value.as.addr = (void *)addr;
  return value;
}

uint64_t lainmeta_vm_handler_init(LainMetaVmHandlerContext *context,
                                  LainVmSpace *space, void *window,
                                  uint32_t max_call_depth) {
  if (!context || !space) return LAINMETA_EXPAND_VM_ERR_NO_SPACE;
  if (!window || ((uintptr_t)window & 7u) != 0)
    return LAINMETA_EXPAND_VM_ERR_NO_WINDOW;

  memset(context, 0, sizeof *context);
  context->space = space;
  context->window = window;
  context->max_call_depth = max_call_depth ? max_call_depth : 8u;
  context->fuel = 100000;
  context->window_handle = lainvm_space_map_external(
      space, (uintptr_t)window, LAINMETA_EXPAND_SLOT_COUNT * 8,
      LAINMETA_EXPAND_SLOT_COUNT * 8, LAINVM_MEM_READ | LAINVM_MEM_WRITE, 0);
  if (lainvm_space_handle_none(context->window_handle)) {
    memset(context, 0, sizeof *context);
    return LAINMETA_EXPAND_VM_ERR_WINDOW_MAP;
  }
  context->window_addr = (uintptr_t)window;
  return 0;
}

void lainmeta_vm_handler_release(LainMetaVmHandlerContext *context) {
  if (!context || !context->space) return;
  if (!lainvm_space_handle_none(context->window_handle))
    (void)lainvm_space_unmap_external(context->space, context->window_handle);
  /* 搁置的激活不在这里释放：它们还挂在某个端点上，撤销要靠会合方。 */
  context->window_handle = lainvm_space_no_handle();
  context->window = NULL;
  context->space = NULL;
}

/* 会把单次调用的判定落回 result；失败路径一律 ERROR，区分原因靠 diagnostic。 */
uint64_t lainmeta_vm_handler(void *context, const LainExpandRequest *request,
                             LainExpandResult *result) {
  LainMetaVmHandlerContext *ctx = context;
  uint64_t *slots;
  uint64_t id;
  L1Value args[LAINMETA_EXPAND_ARG_COUNT];
  LainVmTcb *tcb;
  LainVmSliceResult slice;
  uint64_t status;

  if (!ctx || !result || !request) return LAIN_EXPAND_ERROR;
  memset(result, 0, sizeof *result);
  if (!ctx->space) {
    result->diagnostic = LAINMETA_EXPAND_VM_ERR_NO_SPACE;
    return LAIN_EXPAND_ERROR;
  }
  if (!ctx->image || !ctx->entry) {
    result->diagnostic = LAINMETA_EXPAND_VM_ERR_NO_IMAGE;
    return LAIN_EXPAND_ERROR;
  }

  slots = (uint64_t *)ctx->window;
  memset(slots, 0, LAINMETA_EXPAND_SLOT_COUNT * 8);

  args[LAINMETA_EXPAND_ARG_ABI] = scalar(request->abi_version);
  args[LAINMETA_EXPAND_ARG_PARENT] = scalar(request->parent);
  args[LAINMETA_EXPAND_ARG_CURSOR] = scalar(request->cursor);
  args[LAINMETA_EXPAND_ARG_SYNTAX_ENV] = scalar(request->syntax_env);
  args[LAINMETA_EXPAND_ARG_SCOPE] = scalar(request->scope);
  args[LAINMETA_EXPAND_ARG_CONTEXT_KIND] = scalar(request->context_kind);
  args[LAINMETA_EXPAND_ARG_EXPANSION_ID] = scalar(request->expansion_id);
  args[LAINMETA_EXPAND_ARG_RESULT_WINDOW] = address(ctx->window_addr);

  id = ++ctx->next_id;
  tcb = lainvm_tcb_new(ctx->image, ctx->space, id, ctx->owner,
                       ctx->max_call_depth, lainvm_stack_no_lease(),
                       ctx->quota);
  if (!tcb) {
    result->diagnostic = LAINMETA_EXPAND_VM_ERR_TCB;
    return LAIN_EXPAND_ERROR;
  }

  if (lainvm_tcb_start(tcb, ctx->entry, args, LAINMETA_EXPAND_ARG_COUNT,
                       NULL) != 0) {
    lainvm_tcb_free(tcb);
    result->diagnostic = LAINMETA_EXPAND_VM_ERR_START;
    return LAIN_EXPAND_ERROR;
  }

  ctx->calls++;
  slice = lainvm_engine_run(tcb, ctx->fuel ? ctx->fuel : 100000);

  if (slice == LAINVM_SLICE_BLOCKED) {
    /* 处理器不该阻塞。释放会让端点悬空，所以只能搁置——留给会合方收尾。 */
    if (ctx->parked_count < LAINMETA_EXPAND_PARKED_MAX)
      ctx->parked[ctx->parked_count++] = tcb;
    else
      result->diagnostic = LAINMETA_EXPAND_VM_ERR_PARKED_FULL;
    ctx->blocked++;
    return LAIN_EXPAND_DEFER;
  }

  if (slice == LAINVM_SLICE_TRAPPED) {
    lainvm_tcb_free(tcb);
    ctx->trapped++;
    result->status = LAIN_EXPAND_ERROR;
    result->diagnostic = LAINMETA_EXPAND_VM_ERR_TRAPPED;
    return LAIN_EXPAND_ERROR;
  }

  if (slice != LAINVM_SLICE_DONE) {
    /* 燃料耗尽：激活还活着，但驱动不能等它——按错误上报，由上层加大预算。 */
    lainvm_tcb_free(tcb);
    ctx->exhausted++;
    result->status = LAIN_EXPAND_ERROR;
    result->diagnostic = LAINMETA_EXPAND_VM_ERR_EXHAUSTED;
    return LAIN_EXPAND_ERROR;
  }

  lainvm_tcb_free(tcb);

  status = slots[LAINMETA_EXPAND_SLOT_STATUS];
  if (status > LAIN_EXPAND_ERROR) {
    ctx->protocol_errors++;
    result->status = LAIN_EXPAND_ERROR;
    result->diagnostic = LAINMETA_EXPAND_VM_ERR_PROTOCOL;
    return LAIN_EXPAND_ERROR;
  }

  result->status = status;
  result->consumed = slots[LAINMETA_EXPAND_SLOT_CONSUMED];
  result->replacement = slots[LAINMETA_EXPAND_SLOT_REPLACEMENT];
  result->replacement_count = slots[LAINMETA_EXPAND_SLOT_REPLACEMENT_COUNT];
  result->diagnostic = slots[LAINMETA_EXPAND_SLOT_DIAGNOSTIC];
  result->dependency = slots[LAINMETA_EXPAND_SLOT_DEPENDENCY];
  return status;
}
