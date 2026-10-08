/* lain/meta/apply.h 的实现。 */
#include "lain/meta/apply.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "lain/ir/build.h"
#include "lain/text/parse.h"
#include "lain/ir/verify.h"
#include "lain/vm/engine.h"
#include "lain/vm/image.h"

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
 * lainmeta_apply_proc 走这里；执行完的账目写进 quota_out。
 * 失败不写 value_out，诊断统一在 value_out 之外由调用方补。 */
static bool apply_run(const L1Module *module, const char *entry,
                      const L1Value *args, uint32_t arg_count,
                      const LainMetaApplyLimits *limits, L1Value *value_out,
                      LainVmQuota *quota_out, uint64_t *fuel_used_out,
                      LainMetaApplyBytes *bytes_out, uint64_t byte_length,
                      const LainMetaApplyBytes *byte_arg,
                      uint32_t byte_arg_index, L1Diagnostic *diag) {
  LainVmSpace space;
  LainVmImage *image = NULL;
  LainVmTcb *tcb = NULL;
  LainVmStackLease lease = lainvm_stack_no_lease();
  LainVmRegionHandle byte_region = lainvm_space_no_handle();
  LainVmQuota quota;
  L1Value call_args[8];
  uint8_t *byte_copy = NULL;
  bool byte_charged = false;
  const L1Subroutine *callee = NULL;
  L1Value value;
  LainVmSliceResult result;
  uint32_t index = 0;
  int code = 0;
  char message[256] = "";
  bool ok = false;

  if (fuel_used_out) *fuel_used_out = 0;

  if (quota_out) lainvm_quota_init(quota_out, limits ? limits->quota_bytes : 0);

  if (!module || !entry) return fail(diag, 9302, "apply: the entry does not exist");
  if (arg_count > 8) return fail(diag, 9303, "apply: too many arguments");
  callee = apply_find_sub(module, entry, &index);
  if (!callee) return fail(diag, 9302, "apply: the entry does not exist");
  if (callee->result_count > 1 ||
      (callee->result_count == 1 && !callee->results[0]))
    return fail(diag, 9307, "apply: entry has an unsupported result");
  if (callee->result_count == 1 && callee->results[0]->kind == TY_ADDR &&
      !bytes_out)
    return fail(diag, 9308,
                "apply: an address result must be materialized by the host");
  if (callee->result_count == 1 && callee->results[0]->kind == TY_FLOATS &&
      callee->results[0]->width != 32 && callee->results[0]->width != 64)
    return fail(diag, 9333, "apply: unsupported floating-point format");

  lainvm_space_init(&space);
  lainvm_quota_init(&quota, limits ? limits->quota_bytes : 0);
  image = lainvm_image_load(module, &space, diag);
  if (!image) {
    code = 9313;
    snprintf(message, sizeof(message), "apply: cannot load the module");
    goto cleanup;
  }
  if (byte_arg) {
    if (lainvm_quota_charge(&quota, byte_arg->length) != 0) {
      code = LAINVM_QUOTA_TRAP;
      snprintf(message, sizeof(message), "apply: byte argument exceeds the allocation quota");
      goto cleanup;
    }
    byte_charged = true;
    byte_copy = (uint8_t *)malloc((size_t)byte_arg->length);
    if (!byte_copy) {
      code = 2028;
      snprintf(message, sizeof(message), "apply: cannot copy the byte argument");
      goto cleanup;
    }
    memcpy(byte_copy, byte_arg->data, (size_t)byte_arg->length);
    byte_region = lainvm_space_map_external(
        &space, (uintptr_t)byte_copy, byte_arg->length, byte_arg->length,
        LAINVM_MEM_READ, 0);
    if (lainvm_space_handle_none(byte_region)) {
      code = 9348;
      snprintf(message, sizeof(message), "apply: cannot map the byte argument");
      goto cleanup;
    }
  }
  if (limits && limits->stack_bytes) {
    lease.space = &space;
    lease.region = lainvm_space_alloc_stack(&space, limits->stack_bytes, 1, &quota);
    if (lainvm_space_handle_none(lease.region)) {
      code = 9318;
      snprintf(message, sizeof(message), "apply: cannot admit a stack");
      goto cleanup;
    }
  }
  tcb = lainvm_tcb_new(image, &space, 1, 1,
                       (limits && limits->max_call_depth) ? limits->max_call_depth : 64,
                       lease, &quota);
  if (!tcb) {
    code = 9314;
    snprintf(message, sizeof(message), "apply: cannot admit an activation");
    goto cleanup;
  }
  if (limits && limits->caps && lainvm_tcb_set_caps(tcb, limits->caps, diag) != 0) {
    /* 借来的表必须由调用方先冻结；这里不替它 freeze，也不改它。 */
    code = 9315;
    snprintf(message, sizeof(message),
             "apply: cannot resolve capabilities (the table must be frozen)");
    goto cleanup;
  }
  memset(&value, 0, sizeof(value));
  if (arg_count > 8) {
    code = 9303;
    snprintf(message, sizeof(message), "apply: too many arguments");
    goto cleanup;
  }
  if (arg_count) memcpy(call_args, args, arg_count * sizeof(*args));
  if (byte_arg) {
    call_args[byte_arg_index] = (L1Value){
        L1_VALUE_ADDR, 0, {.addr = (void *)byte_copy}};
  }
  if (callee->flags & SUBROUTINE_EXTERN) {
    result = lainvm_vm_call_host(tcb, index, call_args, arg_count,
                                 callee->result_count ? &value : NULL);
    if (result != LAINVM_SLICE_RUNNABLE) {
      code = result == LAINVM_SLICE_TRAPPED && tcb->trap.status > 0
                 ? tcb->trap.status : 9305;
      snprintf(message, sizeof(message), "apply: the host call was refused");
      goto cleanup;
    }
  } else {
    if (lainvm_tcb_start(tcb, entry, call_args, arg_count, diag) != 0) {
      code = 9305;
      snprintf(message, sizeof(message), "apply: cannot start the entry");
      goto cleanup;
    }
    result = lainvm_engine_run(tcb, (limits && limits->fuel) ? limits->fuel : 1000000);
    if (result != LAINVM_SLICE_DONE) {
      code = result == LAINVM_SLICE_TRAPPED && tcb->trap.status > 0
                 ? tcb->trap.status : 9306;
      snprintf(message, sizeof(message), "apply: the entry trapped (%d)", code);
      goto cleanup;
    }
    if (!tcb->has_result && callee->result_count > 0) {
      code = 9307;
      snprintf(message, sizeof(message), "apply: the entry produced no value");
      goto cleanup;
    }
    value = tcb->result;
  }
  if (bytes_out) {
    if (value.kind != L1_VALUE_ADDR ||
        !lainvm_space_check(&space, (uintptr_t)value.as.addr, byte_length,
                            LAINVM_MEM_READ)) {
      code = 9344;
      snprintf(message, sizeof(message),
               "apply: byte result is not a readable range in its VSpace");
      goto cleanup;
    }
    if (lainvm_quota_charge(&quota, byte_length) != 0) {
      code = LAINVM_QUOTA_TRAP;
      snprintf(message, sizeof(message),
               "apply: byte result exceeds the allocation quota");
      goto cleanup;
    }
    if (!lainmeta_apply_bytes_alloc(byte_length, bytes_out, diag)) {
      (void)lainvm_quota_release(&quota, byte_length);
      code = diag && diag->code ? diag->code : 9345;
      snprintf(message, sizeof(message), "apply: cannot allocate byte result");
      goto cleanup;
    }
    memcpy(bytes_out->data, value.as.addr, (size_t)byte_length);
  }
  if (value_out) *value_out = value;
  ok = true;

cleanup:
  if (!ok && bytes_out) lainmeta_apply_bytes_free(bytes_out);
  if (fuel_used_out && tcb) *fuel_used_out = tcb->steps;
  if (tcb) lainvm_tcb_free(tcb);
  if (!lainvm_stack_lease_none(lease)) (void)lainvm_space_free(&space, lease.region);
  if (!lainvm_space_handle_none(byte_region))
    (void)lainvm_space_unmap_external(&space, byte_region);
  free(byte_copy);
  if (byte_charged) (void)lainvm_quota_release(&quota, byte_arg->length);
  if (image) lainvm_image_free(image);
  if (quota_out) *quota_out = quota;
  if (!ok) return fail(diag, code, message);
  return true;
}

/* 过程签名里的一项类型是否接受这个常量实参。
 * 判据只用物理类型：类别与位宽都要对上；#addr 类型的形参不接受任何实参。 */
static bool apply_param_accepts(const L1Type *ty, const LainMetaApplyValue *arg) {
  if (!ty || !arg) return false;
  if (ty->kind != (L1TypeKind)arg->kind) return false;
  if (ty->kind == TY_ADDR) return false; /* #addr 形参一律拒 9343 */
  if (arg->width == 0 || arg->width > 64) return false;
  return ty->width == arg->width;
}

/* --- 字节块：类型、分配/释放与校验（第一版） -------------------------------- */

void lainmeta_apply_bytes_set_len(LainMetaApplyBytes *bytes, uint64_t length) {
  if (bytes) bytes->length = length;
}

bool lainmeta_apply_bytes_alloc(uint64_t length, LainMetaApplyBytes *out,
                           L1Diagnostic *diag) {
  if (!out) return fail(diag, 9345, "apply: byte block has no destination");
  out->data = NULL;
  out->length = 0;
  if (length == 0 || length > LAINMETA_APPLY_BYTES_MAX)
    return fail(diag, 9345, "apply: byte block length is out of range");
  out->data = (uint8_t *)malloc((size_t)length);
  if (!out->data) return fail(diag, 2028, "apply: out of memory");
  memset(out->data, 0, (size_t)length);
  out->length = length;
  return true;
}

void lainmeta_apply_bytes_free(LainMetaApplyBytes *bytes) {
  if (!bytes) return;
  free(bytes->data);
  bytes->data = NULL;
  bytes->length = 0;
}

bool lainmeta_apply_bytes_check(const LainMetaApplyBytes *bytes, L1Diagnostic *diag) {
  if (!bytes) return fail(diag, 9346, "apply: byte block is missing");
  if (bytes->length > LAINMETA_APPLY_BYTES_MAX)
    return fail(diag, 9345, "apply: byte block length is out of range");
  if (bytes->length == 0 || !bytes->data)
    return fail(diag, 9346, "apply: byte block is empty or has no storage");
  return true;
}

/* --- 通用 apply：执行模块里的普通 #proc ------------------------------------- */

static bool apply_proc_common(const char *module_text, const char *entry,
                              const LainMetaApplyValue *args, uint32_t arg_count,
                              uint64_t byte_length,
                              uint32_t byte_arg_index,
                              const LainMetaApplyBytes *byte_arg,
                              const LainMetaApplyLimits *limits,
                              LainMetaApplyResult *out, L1Diagnostic *diag) {
  L1Builder *builder;
  const L1Module *module;
  const L1Subroutine *callee;
  L1Value values[8];
  L1Value value;
  const L1Type *result_ty = NULL;
  uint32_t i;
  bool ok;
  bool want_bytes = byte_length != 0;

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
  if (byte_arg && !lainmeta_apply_bytes_check(byte_arg, diag)) return false;
  if (byte_arg && byte_arg_index >= arg_count)
    return fail(diag, 9341, "apply: byte argument index is out of range");

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
    if (byte_arg && i == byte_arg_index) {
      if (args[i].kind != TY_ADDR || !callee->params[i].ty ||
          callee->params[i].ty->kind != TY_ADDR) {
        lainir_builder_free(builder);
        return fail(diag, 9342, "apply: byte argument requires an #addr parameter");
      }
      values[i] = (L1Value){L1_VALUE_ADDR, 0, {.addr = NULL}};
      continue;
    }
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
  if (want_bytes &&
      (callee->result_count != 1 || !callee->results[0] ||
       callee->results[0]->kind != TY_ADDR)) {
    lainir_builder_free(builder);
    return fail(diag, 9344, "apply: byte export requires one address result");
  }
  if (callee->result_count == 1) {
    result_ty = callee->results[0];
    if (!result_ty || (result_ty->kind == TY_ADDR && !want_bytes) ||
        (result_ty->kind != TY_ADDR && want_bytes)) {
      lainir_builder_free(builder);
      return fail(diag, 9344, "apply: result kind does not match the export API");
    }
  }

  memset(&value, 0, sizeof(value));
  ok = apply_run(module, entry, values, arg_count, limits, &value, NULL,
                 &out->fuel_used, want_bytes ? &out->bytes : NULL,
                 byte_length, byte_arg, byte_arg_index, diag);
  if (!ok) {
    lainir_builder_free(builder);
    /* 位置属于生成的模块文本；今天给不出行号，留 0 = 未知 */
    if (diag) {
      diag->line = 0;
      diag->column = 0;
    }
    return false;
  }
  if (want_bytes) {
    out->kind = LAINMETA_APPLY_RESULT_BYTES;
  } else {
    out->kind = LAINMETA_APPLY_RESULT_SCALAR;
    out->scalar.kind = result_ty ? result_ty->kind : TY_BITS;
    out->scalar.width = result_ty ? result_ty->width : 64;
    out->scalar.bits = callee->result_count ? value.as.bits : 0;
    if (result_ty && result_ty->kind == TY_FLOATS && result_ty->width < 64)
      out->scalar.bits &= ((uint64_t)1 << result_ty->width) - 1u;
  }
  lainir_builder_free(builder);
  return true;
}

bool lainmeta_apply_proc(const char *module_text, const char *entry,
                    const LainMetaApplyValue *args, uint32_t arg_count,
                    const LainMetaApplyLimits *limits, LainMetaApplyResult *out,
                    L1Diagnostic *diag) {
  return apply_proc_common(module_text, entry, args, arg_count, 0,
                           UINT32_MAX, NULL,
                           limits, out, diag);
}

bool lainmeta_apply_proc_bytes(const char *module_text, const char *entry,
                          const LainMetaApplyValue *args, uint32_t arg_count,
                          uint64_t byte_length,
                          const LainMetaApplyLimits *limits, LainMetaApplyResult *out,
                          L1Diagnostic *diag) {
  if (byte_length == 0 || byte_length > LAINMETA_APPLY_BYTES_MAX) {
    if (out) memset(out, 0, sizeof(*out));
    return fail(diag, 9345, "apply: byte result length is out of range");
  }
  return apply_proc_common(module_text, entry, args, arg_count, byte_length,
                           UINT32_MAX, NULL,
                           limits, out, diag);
}

bool lainmeta_apply_proc_with_bytes_arg(
    const char *module_text, const char *entry, const LainMetaApplyValue *args,
    uint32_t arg_count, uint32_t byte_arg_index,
    const LainMetaApplyBytes *byte_arg, const LainMetaApplyLimits *limits,
    LainMetaApplyResult *out, L1Diagnostic *diag) {
  if (out) memset(out, 0, sizeof(*out));
  if (!byte_arg)
    return fail(diag, 9346, "apply: byte argument is missing");
  if (!lainmeta_apply_bytes_check(byte_arg, diag))
    return false;
  return apply_proc_common(module_text, entry, args, arg_count, 0,
                           byte_arg_index, byte_arg, limits, out, diag);
}
