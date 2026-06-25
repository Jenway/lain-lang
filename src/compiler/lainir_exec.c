#include "lainir_exec.h"
#include "lainir/lainir.h"
#include "lainir/interpreter.h"
#include <string.h>
#include <stdlib.h>

typedef struct {
  vm_context *ctx;
  vm_value   *env;
} SchemeCapabilityEnv;

static uint32_t sexp_list_length(vm_value *list) {
  uint32_t len = 0;
  while (vm_is_pair(list)) {
    len++;
    list = vm_cdr(list);
  }
  return len;
}

static int sexp_to_lainir_value(vm_context *ctx, vm_value *value,
                                 LainirValue *out) {
  if (vm_is_fixnum(value)) {
    *out = lainir_value_bits((uint64_t)vm_fixnum_value(value), 32);
    return 1;
  }
  if (vm_is_integer(value)) {
    *out = lainir_value_bits((uint64_t)vm_uint_value(value), 64);
    return 1;
  }
  if (value == vm_true()) {
    *out = lainir_value_bits(1, 1);
    return 1;
  }
  if (value == vm_false()) {
    *out = lainir_value_bits(0, 1);
    return 1;
  }
  if (value == vm_void() || value == vm_null()) {
    *out = lainir_value_unit();
    return 1;
  }
  if (vm_is_string(value)) {
    *out = lainir_value_string(vm_string_data(value));
    return 1;
  }
  if (vm_is_symbol(value)) {
    *out = lainir_value_string(vm_symbol_name(ctx, value));
    return 1;
  }
  return 0;
}

static vm_value *lainir_value_to_sexp(vm_context *ctx, LainirValue value) {
  switch (value.kind) {
  case LAINIR_VALUE_UNIT:
    return vm_void();
  case LAINIR_VALUE_BITS:
    return vm_make_integer(ctx, (int64_t)value.as.bits);
  case LAINIR_VALUE_STRING:
    return vm_make_string(ctx, value.as.string ? value.as.string : "", -1);
  case LAINIR_VALUE_ADDR:
    return vm_make_cpointer(ctx, value.as.addr);
  case LAINIR_VALUE_FUNC:
    return vm_make_string(ctx, value.as.func ? value.as.func->name : "", -1);
  default:
    return vm_false();
  }
}

static LainirRunStatus scheme_host_call(
  const LainirValue *args,
  uint32_t arg_count,
  LainirValue *result_out,
  const char **error_out,
  void *user_data) {
  LainirCapability *cap = (LainirCapability *)user_data;
  SchemeCapabilityEnv *scheme_env = (SchemeCapabilityEnv *)cap->user_data;
  vm_context *ctx = scheme_env->ctx;
  vm_value *env = scheme_env->env;
  vm_value *proc = vm_env_ref(ctx, env, vm_intern(ctx, cap->name));
  vm_value *arg_list = vm_null();
  vm_value *result;

  if (proc == vm_false() || !vm_is_procedure(proc)) {
    *error_out = "extern capability not found in Scheme environment";
    return LAINIR_RUN_BAD_CALL;
  }

  for (int i = (int)arg_count - 1; i >= 0; i--)
    arg_list = vm_cons(ctx, lainir_value_to_sexp(ctx, args[i]), arg_list);

  result = vm_apply(ctx, proc, arg_list);
  if (vm_is_exception(result)) {
    *error_out = "Scheme extern capability raised exception";
    return LAINIR_RUN_BAD_CALL;
  }
  if (!sexp_to_lainir_value(ctx, result, result_out)) {
    *error_out = "unsupported Scheme result value for lainir extern";
    return LAINIR_RUN_BAD_CALL;
  }
  return LAINIR_RUN_OK;
}

LainirExecStatus lainir_exec_request(
  vm_context *ctx,
  vm_value *env,
  const LainirExecRequest *request,
  vm_value **result_out) {
  L1Subroutine *entry = NULL;
  LainirValue *args = NULL;
  uint32_t arg_count = sexp_list_length(request->args);
  LainirCapabilityTable *caps = lainir_caps_new();
  SchemeCapabilityEnv scheme_env = {.ctx = ctx, .env = env};
  LainirRunRequest run_request;
  LainirValue result = lainir_value_unit();
  const char *error = NULL;
  LainirExecStatus exec_status = LAINIR_EXEC_FAILED;

  if (!caps) {
    *result_out = vm_user_exception(ctx, "lainir execution out of memory");
    return LAINIR_EXEC_FAILED;
  }

  for (L1Subroutine *sub = g_subroutines_head; sub; sub = sub->next) {
    if (strcmp(sub->name, request->entry_name) == 0) {
      entry = sub;
      break;
    }
  }
  if (!entry) {
    *result_out = vm_user_exception(ctx, "lainir execution entry unavailable");
    lainir_caps_free(caps);
    return LAINIR_EXEC_UNAVAILABLE;
  }

  if (arg_count) {
    vm_value *curr = request->args;
    args = calloc(arg_count, sizeof(LainirValue));
    if (!args) {
      *result_out = vm_user_exception(ctx, "lainir execution out of memory");
      lainir_caps_free(caps);
      return LAINIR_EXEC_FAILED;
    }
    for (uint32_t i = 0; i < arg_count; i++) {
      if (!sexp_to_lainir_value(ctx, vm_car(curr), &args[i])) {
        *result_out = vm_user_exception(ctx, "unsupported lainir argument");
        free(args);
        lainir_caps_free(caps);
        return LAINIR_EXEC_FAILED;
      }
      curr = vm_cdr(curr);
    }
  }

  for (L1Subroutine *sub = g_subroutines_head; sub; sub = sub->next) {
    if (sub->is_extern && !sub->blocks) {
      LainirCapability *cap = malloc(sizeof(LainirCapability));
      if (!cap || !lainir_caps_add(caps, sub->link_name ? sub->link_name : sub->name,
                                   scheme_host_call, cap)) {
        free(cap);
        *result_out = vm_user_exception(ctx, "lainir capability registration failed");
        free(args);
        lainir_caps_free(caps);
        return LAINIR_EXEC_FAILED;
      }
      cap->name = sub->link_name ? sub->link_name : sub->name;
      cap->fn = scheme_host_call;
      cap->user_data = &scheme_env;
    }
  }

  run_request.module = g_subroutines_head;
  run_request.entry_name = request->entry_name;
  run_request.args = args;
  run_request.arg_count = arg_count;
  run_request.caps = caps;

  switch (lainir_run(&run_request, &result, &error)) {
  case LAINIR_RUN_OK:
    *result_out = lainir_value_to_sexp(ctx, result);
    exec_status = LAINIR_EXEC_OK;
    break;
  case LAINIR_RUN_NO_ENTRY:
    *result_out = vm_user_exception(ctx, "lainir execution entry unavailable");
    exec_status = LAINIR_EXEC_UNAVAILABLE;
    break;
  default:
    *result_out = vm_user_exception(ctx, error ? error : "lainir execution failed");
    exec_status = LAINIR_EXEC_FAILED;
    break;
  }

  if (caps) {
    for (uint32_t i = 0; i < caps->count; i++)
      free(caps->entries[i].user_data);
    lainir_caps_free(caps);
  }
  free(args);
  return exec_status;
}
