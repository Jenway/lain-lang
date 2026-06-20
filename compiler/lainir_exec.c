#include "lainir_exec.h"
#include "lainir/lainir.h"
#include "lainir/interpreter.h"

typedef struct {
  sexp ctx;
  sexp env;
} SchemeCapabilityEnv;

static uint32_t sexp_list_length(sexp list) {
  uint32_t len = 0;
  while (sexp_pairp(list)) {
    len++;
    list = sexp_cdr(list);
  }
  return len;
}

static int sexp_to_lainir_value(sexp ctx, sexp value, LainirValue *out) {
  if (sexp_fixnump(value)) {
    *out = lainir_value_bits((uint64_t)sexp_unbox_fixnum(value), 32);
    return 1;
  }
  if (sexp_integerp(value)) {
    *out = lainir_value_bits((uint64_t)sexp_uint_value(value), 64);
    return 1;
  }
  if (value == SEXP_TRUE) {
    *out = lainir_value_bits(1, 1);
    return 1;
  }
  if (value == SEXP_FALSE) {
    *out = lainir_value_bits(0, 1);
    return 1;
  }
  if (value == SEXP_VOID || value == SEXP_NULL) {
    *out = lainir_value_unit();
    return 1;
  }
  if (sexp_stringp(value)) {
    *out = lainir_value_string(sexp_string_data(value));
    return 1;
  }
  if (sexp_symbolp(value)) {
    *out = lainir_value_string(sexp_string_data(sexp_symbol_to_string(ctx, value)));
    return 1;
  }
  return 0;
}

static sexp lainir_value_to_sexp(sexp ctx, LainirValue value) {
  switch (value.kind) {
  case LAINIR_VALUE_UNIT:
    return SEXP_VOID;
  case LAINIR_VALUE_BITS:
    return sexp_make_integer(ctx, value.as.bits);
  case LAINIR_VALUE_STRING:
    return sexp_c_string(ctx, value.as.string ? value.as.string : "", -1);
  case LAINIR_VALUE_ADDR:
    return sexp_make_cpointer(ctx, SEXP_CPOINTER, value.as.addr, SEXP_FALSE, 0);
  case LAINIR_VALUE_FUNC:
    return sexp_c_string(ctx, value.as.func ? value.as.func->name : "", -1);
  default:
    return SEXP_FALSE;
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
  sexp ctx = scheme_env->ctx;
  sexp env = scheme_env->env;
  sexp proc = sexp_env_ref(ctx, env, sexp_intern(ctx, cap->name, -1), SEXP_FALSE);
  sexp arg_list = SEXP_NULL;
  sexp result;

  if (proc == SEXP_FALSE || !sexp_procedurep(proc)) {
    *error_out = "extern capability not found in Scheme environment";
    return LAINIR_RUN_BAD_CALL;
  }

  for (int i = (int)arg_count - 1; i >= 0; i--)
    arg_list = sexp_cons(ctx, lainir_value_to_sexp(ctx, args[i]), arg_list);

  result = sexp_apply(ctx, proc, arg_list);
  if (sexp_exceptionp(result)) {
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
  sexp ctx,
  sexp env,
  const LainirExecRequest *request,
  sexp *result_out) {
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
    *result_out = sexp_user_exception(
      ctx, NULL, "lainir execution out of memory", SEXP_FALSE);
    return LAINIR_EXEC_FAILED;
  }

  for (L1Subroutine *sub = g_subroutines_head; sub; sub = sub->next) {
    if (strcmp(sub->name, request->entry_name) == 0) {
      entry = sub;
      break;
    }
  }
  if (!entry) {
    *result_out = sexp_user_exception(
      ctx, NULL, "lainir execution entry unavailable",
      sexp_c_string(ctx, request->entry_name, -1));
    lainir_caps_free(caps);
    return LAINIR_EXEC_UNAVAILABLE;
  }

  if (arg_count) {
    sexp curr = request->args;
    args = calloc(arg_count, sizeof(LainirValue));
    if (!args) {
      *result_out = sexp_user_exception(
        ctx, NULL, "lainir execution out of memory", SEXP_FALSE);
      lainir_caps_free(caps);
      return LAINIR_EXEC_FAILED;
    }
    for (uint32_t i = 0; i < arg_count; i++) {
      if (!sexp_to_lainir_value(ctx, sexp_car(curr), &args[i])) {
        *result_out = sexp_user_exception(
          ctx, NULL, "unsupported lainir argument", sexp_car(curr));
        free(args);
        lainir_caps_free(caps);
        return LAINIR_EXEC_FAILED;
      }
      curr = sexp_cdr(curr);
    }
  }

  for (L1Subroutine *sub = g_subroutines_head; sub; sub = sub->next) {
    if (sub->is_extern && !sub->blocks) {
      LainirCapability *cap = malloc(sizeof(LainirCapability));
      if (!cap || !lainir_caps_add(caps, sub->link_name ? sub->link_name : sub->name,
                                   scheme_host_call, cap)) {
        free(cap);
        *result_out = sexp_user_exception(
          ctx, NULL, "lainir capability registration failed", SEXP_FALSE);
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
    *result_out = sexp_user_exception(
      ctx, NULL, "lainir execution entry unavailable",
      sexp_c_string(ctx, request->entry_name, -1));
    exec_status = LAINIR_EXEC_UNAVAILABLE;
    break;
  default:
    *result_out = sexp_user_exception(
      ctx, NULL, error ? error : "lainir execution failed",
      sexp_c_string(ctx, request->entry_name, -1));
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
