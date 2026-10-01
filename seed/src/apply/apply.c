/* lainapply/apply.h 的实现。 */
#include "lainapply/apply.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "lainir/build.h"
#include "lainir/parse.h"
#include "lainir/verify.h"
#include "lainvm/engine.h"
#include "lainvm/image.h"

static bool fail(L1Diagnostic *diag, int code, const char *message) {
  if (diag) {
    diag->code = code;
    diag->line = 0;
    diag->column = 0;
    snprintf(diag->message, sizeof(diag->message), "%s", message);
  }
  return false;
}

/* 按名字找模块里的过程；index_out 给出它在模块里的下标。 */
static const L1Subroutine *apply_find_sub(const L1Module *module,
                                         const char *name, uint32_t *index_out) {
  uint32_t i;
  for (i = 0; i < module->subroutine_count; i++) {
    if (module->subroutines[i].name &&
        strcmp(module->subroutines[i].name, name) == 0) {
      if (index_out) *index_out = i;
      return &module->subroutines[i];
    }
  }
  return NULL;
}

/* 一次编译期执行的公共实现：独立 VSpace + 栈租约 + TCB + 借来的能力表。
 * lainapply_call 与 lainapply_proc 都走这里；执行完的账目写进 quota_out。
 * 失败不写 value_out，诊断统一在 value_out 之外由调用方补。 */
static bool apply_run(const L1Module *module, const char *entry,
                      const L1Value *args, uint32_t arg_count,
                      const LainApplyLimits *limits, L1Value *value_out,
                      LainVmQuota *quota_out, L1Diagnostic *diag) {
  LainVmSpace space;
  LainVmImage *image = NULL;
  LainVmTcb *tcb = NULL;
  LainVmStackLease lease = lainvm_stack_no_lease();
  LainVmQuota quota;
  const L1Subroutine *callee = NULL;
  L1Value value;
  LainVmSliceResult result;
  uint32_t index = 0;
  int code = 0;
  char message[256] = "";
  bool ok = false;

  if (quota_out) lainvm_quota_init(quota_out, limits ? limits->quota_bytes : 0);

  if (!module || !entry) return fail(diag, 9302, "fold: the compile-time callee is not in the module");
  if (arg_count > 8) return fail(diag, 9303, "fold: too many arguments in the compile-time call");
  callee = apply_find_sub(module, entry, &index);
  if (!callee) return fail(diag, 9302, "fold: the compile-time callee is not in the module");
  if (callee->result_count > 1 ||
      (callee->result_count == 1 && !callee->results[0]))
    return fail(diag, 9307, "fold: compile-time call has an unsupported result");
  if (callee->result_count == 1 && callee->results[0]->kind == TY_ADDR)
    return fail(diag, 9308,
                "fold: a compile-time result that is an address must be materialized by the host, not by the IR");
  if (callee->result_count == 1 && callee->results[0]->kind == TY_FLOATS &&
      callee->results[0]->width != 32 && callee->results[0]->width != 64)
    return fail(diag, 9333, "eval: unsupported floating-point format");

  lainvm_space_init(&space);
  lainvm_quota_init(&quota, limits ? limits->quota_bytes : 0);
  image = lainvm_image_load(module, &space, diag);
  if (!image) {
    code = 9313;
    snprintf(message, sizeof(message), "fold: cannot load the module for compile-time runs");
    goto cleanup;
  }
  if (limits && limits->stack_bytes) {
    lease.space = &space;
    lease.region = lainvm_space_alloc_stack(&space, limits->stack_bytes, 1, &quota);
    if (lainvm_space_handle_none(lease.region)) {
      code = 9318;
      snprintf(message, sizeof(message), "fold: cannot admit a stack for the compile-time run");
      goto cleanup;
    }
  }
  tcb = lainvm_tcb_new(image, &space, 1, 1,
                       (limits && limits->max_call_depth) ? limits->max_call_depth : 64,
                       lease, &quota);
  if (!tcb) {
    code = 9314;
    snprintf(message, sizeof(message), "fold: cannot admit a compile-time activation");
    goto cleanup;
  }
  if (limits && limits->caps && lainvm_tcb_set_caps(tcb, limits->caps, diag) != 0) {
    /* 借来的表必须由调用方先冻结；这里不替它 freeze，也不改它。 */
    code = 9315;
    snprintf(message, sizeof(message),
             "fold: cannot resolve capabilities (the table must be frozen)");
    goto cleanup;
  }
  memset(&value, 0, sizeof(value));
  if (callee->flags & SUBROUTINE_EXTERN) {
    result = lainvm_vm_call_host(tcb, index, args, arg_count,
                                 callee->result_count ? &value : NULL);
    if (result != LAINVM_SLICE_RUNNABLE) {
      code = result == LAINVM_SLICE_TRAPPED && tcb->trap.status > 0
                 ? tcb->trap.status : 9305;
      snprintf(message, sizeof(message), "fold: the compile-time host call was refused");
      goto cleanup;
    }
  } else {
    if (lainvm_tcb_start(tcb, entry, args, arg_count, diag) != 0) {
      code = 9305;
      snprintf(message, sizeof(message), "fold: cannot start the compile-time call");
      goto cleanup;
    }
    result = lainvm_engine_run(tcb, (limits && limits->fuel) ? limits->fuel : 1000000);
    if (result != LAINVM_SLICE_DONE) {
      code = result == LAINVM_SLICE_TRAPPED && tcb->trap.status > 0
                 ? tcb->trap.status : 9306;
      snprintf(message, sizeof(message), "fold: the compile-time call trapped (%d)", code);
      goto cleanup;
    }
    if (!tcb->has_result && callee->result_count > 0) {
      code = 9307;
      snprintf(message, sizeof(message), "fold: compile-time call produced no value");
      goto cleanup;
    }
    value = tcb->result;
  }
  if (value_out) *value_out = value;
  ok = true;

cleanup:
  if (tcb) lainvm_tcb_free(tcb);
  if (!lainvm_stack_lease_none(lease)) (void)lainvm_space_free(&space, lease.region);
  if (image) lainvm_image_free(image);
  if (quota_out) *quota_out = quota;
  if (!ok) return fail(diag, code, message);
  return true;
}

/* 过程签名里的一项类型是否接受这个常量实参。
 * 判据只用物理类型：类别与位宽都要对上；#addr 类型的形参不接受任何实参。 */
static bool apply_param_accepts(const L1Type *ty, const LainApplyValue *arg) {
  if (!ty || !arg) return false;
  if (ty->kind != (L1TypeKind)arg->kind) return false;
  if (ty->kind == TY_ADDR) return false; /* #addr 形参一律拒 9343 */
  if (arg->width == 0 || arg->width > 64) return false;
  return ty->width == arg->width;
}

bool lainapply_call(const L1Module *module, const L1Inst *inst,
                    const L1Operand *operands, const LainApplyLimits *limits,
                    LainApplyValue *out, L1Diagnostic *diag) {
  return lainapply_call_report(module, inst, operands, limits, out, diag, NULL);
}

bool lainapply_call_report(const L1Module *module, const L1Inst *inst,
                           const L1Operand *operands, const LainApplyLimits *limits,
                           LainApplyValue *out, L1Diagnostic *diag,
                           LainVmQuota *quota_out) {
  L1Value args[8];
  L1Value value;
  LainApplyValue pending;
  uint32_t i;

  if (quota_out) lainvm_quota_init(quota_out, limits ? limits->quota_bytes : 0);

  if (!module || !inst || !limits || !out || !inst->symbol ||
      inst->kind != INST_CALL || !inst->is_eval)
    return fail(diag, 9301, "fold: #eval without a callee");
  if (inst->operand_count > 8)
    return fail(diag, 9303, "fold: too many arguments in #eval");
  for (i = 0; i < inst->operand_count; i++) {
    if (!operands || operands[i].kind == OPERAND_VALUE)
      return fail(diag, 9304, "fold: #eval argument is not compile-time known");
    args[i] = (L1Value){L1_VALUE_BITS, 64, {.bits = operands[i].bits}};
  }
  {
    const L1Subroutine *callee = apply_find_sub(module, inst->symbol, NULL);
    if (!callee) return fail(diag, 9302, "fold: #eval callee is not in the module");
    memset(&value, 0, sizeof(value));
    if (!apply_run(module, inst->symbol, args, inst->operand_count, limits,
                   &value, quota_out, diag)) {
      if (diag) {
        diag->line = inst->line;
        diag->column = inst->column;
      }
      return false;
    }
    pending.kind = callee->result_count ? callee->results[0]->kind : TY_BITS;
    pending.width = callee->result_count ? callee->results[0]->width : 64;
    pending.bits = callee->result_count ? value.as.bits : 0;
  }
  *out = pending;
  return true;
}

bool lainapply_text(const char *text, const char *entry,
                    const LainApplyLimits *limits, LainApplyValue *out,
                    L1Diagnostic *diag) {
  L1Builder *builder;
  const L1Module *module;
  const L1Subroutine *query = NULL;
  const L1Inst *call, *ret;
  uint32_t i;
  bool ok = false;
  if (!text || !entry || !limits || !out)
    return fail(diag, 9330, "eval: missing request input");
  builder = lainir_builder_new();
  if (!builder) return fail(diag, 2028, "eval: out of memory");
  module = lainir_parse(builder, text, diag);
  if (!module || lainir_verify(module, diag) != 0) goto cleanup;
  for (i = 0; i < module->subroutine_count; i++) {
    if (module->subroutines[i].name &&
        strcmp(module->subroutines[i].name, entry) == 0) {
      query = &module->subroutines[i];
      break;
    }
  }
  if (!query || !query->body || query->body->inst_count != 2 ||
      query->param_count != 0 || query->result_count != 1) {
    fail(diag, 9331, "eval: entry must be a zero-argument query procedure");
    goto cleanup;
  }
  call = &query->body->insts[0];
  ret = &query->body->insts[1];
  if (call->kind != INST_CALL || !call->is_eval ||
      call->result_count != 1 || ret->kind != INST_RETURN ||
      ret->operand_count != 1 || ret->operands[0].kind != OPERAND_VALUE ||
      !call->results[0] || !ret->operands[0].name ||
      strcmp(call->results[0], ret->operands[0].name) != 0) {
    fail(diag, 9332, "eval: entry must return its single #eval result");
    goto cleanup;
  }
  ok = lainapply_call(module, call, call->operands, limits, out, diag);
cleanup:
  lainir_builder_free(builder);
  return ok;
}

/* --- 字节块：类型、分配/释放与校验（第一版） -------------------------------- */

void lainapply_bytes_set_len(LainApplyBytes *bytes, uint64_t length) {
  if (bytes) bytes->length = length;
}

bool lainapply_bytes_alloc(uint64_t length, LainApplyBytes *out,
                           L1Diagnostic *diag) {
  if (!out) return fail(diag, 9345, "apply: byte block has no destination");
  out->data = NULL;
  out->length = 0;
  if (length == 0 || length > LAINAPPLY_BYTES_MAX)
    return fail(diag, 9345, "apply: byte block length is out of range");
  out->data = (uint8_t *)malloc((size_t)length);
  if (!out->data) return fail(diag, 2028, "apply: out of memory");
  memset(out->data, 0, (size_t)length);
  out->length = length;
  return true;
}

void lainapply_bytes_free(LainApplyBytes *bytes) {
  if (!bytes) return;
  free(bytes->data);
  bytes->data = NULL;
  bytes->length = 0;
}

bool lainapply_bytes_check(const LainApplyBytes *bytes, L1Diagnostic *diag) {
  if (!bytes) return fail(diag, 9346, "apply: byte block is missing");
  if (bytes->length > LAINAPPLY_BYTES_MAX)
    return fail(diag, 9345, "apply: byte block length is out of range");
  if (bytes->length == 0 || !bytes->data)
    return fail(diag, 9346, "apply: byte block is empty or has no storage");
  return true;
}

/* --- 通用 apply：执行模块里的普通 #proc ------------------------------------- */

bool lainapply_proc(const char *module_text, const char *entry,
                    const LainApplyValue *args, uint32_t arg_count,
                    const LainApplyLimits *limits, LainApplyResult *out,
                    L1Diagnostic *diag) {
  L1Builder *builder;
  const L1Module *module;
  const L1Subroutine *callee;
  L1Value values[8];
  L1Value value;
  const L1Type *result_ty = NULL;
  uint32_t i;
  bool ok;

  if (out) memset(out, 0, sizeof(*out));
  if (diag) {
    diag->code = 0;
    diag->line = 0;
    diag->column = 0;
    diag->message[0] = '\0';
  }
  if (!module_text || !entry || !out)
    return fail(diag, 9340, "apply: entry does not exist");
  if (arg_count > 8 || (arg_count && !args))
    return fail(diag, 9341, "apply: argument count does not match the entry");

  builder = lainir_builder_new();
  if (!builder) return fail(diag, 2028, "apply: out of memory");
  module = lainir_parse(builder, module_text, diag);
  if (!module || lainir_verify(module, diag) != 0) {
    lainir_builder_free(builder);
    return false;
  }
  callee = apply_find_sub(module, entry, NULL);
  if (!callee) {
    lainir_builder_free(builder);
    return fail(diag, 9340, "apply: entry does not exist");
  }
  if (callee->param_count != arg_count) {
    lainir_builder_free(builder);
    return fail(diag, 9341, "apply: argument count does not match the entry");
  }
  for (i = 0; i < arg_count; i++) {
    /* 实参自己带地址的，先按 9343 报，而不是笼统的类型不符 */
    if (args[i].kind == TY_ADDR) {
      lainir_builder_free(builder);
      return fail(diag, 9343, "apply: an argument is an address");
    }
    if (callee->params[i].ty && callee->params[i].ty->kind == TY_ADDR) {
      lainir_builder_free(builder);
      return fail(diag, 9343, "apply: the entry declares an address parameter");
    }
    if (!apply_param_accepts(callee->params[i].ty, &args[i])) {
      lainir_builder_free(builder);
      return fail(diag, 9342, "apply: argument type does not match the entry");
    }
    values[i] = (L1Value){L1_VALUE_BITS, args[i].width, {.bits = args[i].bits}};
  }
  if (callee->result_count > 1) {
    lainir_builder_free(builder);
    return fail(diag, 9344, "apply: the entry declares more than one result");
  }
  if (callee->result_count == 1) {
    result_ty = callee->results[0];
    if (!result_ty || result_ty->kind == TY_ADDR) {
      lainir_builder_free(builder);
      return fail(diag, 9344, "apply: an address result must be materialized by the host");
    }
  }

  memset(&value, 0, sizeof(value));
  ok = apply_run(module, entry, values, arg_count, limits, &value, NULL, diag);
  if (!ok) {
    lainir_builder_free(builder);
    /* 位置属于生成的模块文本；今天给不出行号，留 0 = 未知 */
    if (diag) {
      diag->line = 0;
      diag->column = 0;
    }
    return false;
  }
  out->kind = LAINAPPLY_RESULT_SCALAR;
  out->scalar.kind = result_ty ? result_ty->kind : TY_BITS;
  out->scalar.width = result_ty ? result_ty->width : 64;
  out->scalar.bits = callee->result_count ? value.as.bits : 0;
  if (result_ty && result_ty->kind == TY_FLOATS && result_ty->width < 64)
    out->scalar.bits &= ((uint64_t)1 << result_ty->width) - 1u;
  lainir_builder_free(builder);
  return true;
}
