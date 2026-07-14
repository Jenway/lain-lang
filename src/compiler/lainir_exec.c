#include "lainir_exec.h"
#include "lainir/lainir.h"
#include "lainir/interpreter.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

typedef struct {
  vm_context *ctx;
  vm_value   *env;
  char      **owned_strings;
  uint32_t    owned_string_count;
  uint32_t    owned_string_capacity;
} SchemeCapabilityEnv;

static void scheme_env_release_strings(SchemeCapabilityEnv *env) {
  for (uint32_t i = 0; i < env->owned_string_count; i++)
    free(env->owned_strings[i]);
  free(env->owned_strings);
  env->owned_strings = NULL;
  env->owned_string_count = 0;
  env->owned_string_capacity = 0;
}

static int scheme_env_own_string(SchemeCapabilityEnv *env,
                                 LainirValue *value) {
  char *copy;
  char **grown;
  uint32_t capacity;
  if (value->kind != LAINIR_VALUE_STRING) return 1;
  copy = strdup(value->as.string ? value->as.string : "");
  if (!copy) return 0;
  if (env->owned_string_count == env->owned_string_capacity) {
    capacity = env->owned_string_capacity ? env->owned_string_capacity * 2 : 32;
    grown = realloc(env->owned_strings, capacity * sizeof(char *));
    if (!grown) { free(copy); return 0; }
    env->owned_strings = grown;
    env->owned_string_capacity = capacity;
  }
  env->owned_strings[env->owned_string_count++] = copy;
  value->as.string = copy;
  return 1;
}

static void scheme_env_release_owned_pointer(SchemeCapabilityEnv *env,
                                             const char *pointer) {
  for (uint32_t i = 0; i < env->owned_string_count; i++) {
    if (env->owned_strings[i] == pointer) {
      free(env->owned_strings[i]);
      env->owned_string_count--;
      env->owned_strings[i] = env->owned_strings[env->owned_string_count];
      return;
    }
  }
}

static LainirRunStatus scheme_host_call(
  const LainirValue *args,
  uint32_t arg_count,
  LainirValue *result_out,
  const char **error_out,
  void *user_data);

static int capability_is_allowed(const LainirExecTextRequest *request,
                                 const char *name) {
  for (uint32_t i = 0; i < request->allowed_capability_count; i++)
    if (strcmp(request->allowed_capabilities[i], name) == 0)
      return 1;
  return 0;
}

LainirExecStatus lainir_exec_text_request(
  const LainirExecTextRequest *request,
  LainirValue *result_out,
  L1Diagnostic *diagnostic) {
  L1Subroutine *module = NULL;
  LainirRunRequest run_request;
  const char *run_error = NULL;
  LainirRunStatus run_status;
  LainirCapabilityTable *caps = NULL;
  SchemeCapabilityEnv scheme_env = {0};

  if (diagnostic)
    memset(diagnostic, 0, sizeof(*diagnostic));
  if (!request || !request->text || !request->entry_name) {
    if (diagnostic) {
      diagnostic->code = 3001;
      snprintf(diagnostic->message, sizeof(diagnostic->message),
               "execute-text requires source text and an entry name");
    }
    return LAINIR_EXEC_FAILED;
  }
  if (strlen(request->text) > 1024u * 1024u) {
    if (diagnostic) {
      diagnostic->code = 3004;
      snprintf(diagnostic->message, sizeof(diagnostic->message),
               "generated LAIN-IR text exceeds the 1 MiB bootstrap limit");
    }
    return LAINIR_EXEC_FAILED;
  }
  if (!lainir_parse_module_checked(request->text, &module, diagnostic))
    return LAINIR_EXEC_FAILED;
  if (!lainir_verify_module(module, request->entry_name, diagnostic)) {
    lainir_free_subroutines(module);
    return LAINIR_EXEC_FAILED;
  }

  run_request.module = module;
  run_request.entry_name = request->entry_name;
  run_request.args = request->args;
  run_request.arg_count = request->arg_count;
  /* Generated target IR remains capability-free.  A precompiled compiler
   * artifact may opt in by supplying an explicit host environment; only its
   * declared extern procedures are then bound. */
  run_request.caps = NULL;
  if (request->host_ctx && request->host_env) {
    scheme_env.ctx = request->host_ctx;
    scheme_env.env = request->host_env;
    caps = lainir_caps_new();
    if (!caps) {
      if (diagnostic) {
        diagnostic->code = 3005;
        snprintf(diagnostic->message, sizeof(diagnostic->message),
                 "compiler artifact capability table allocation failed");
      }
      lainir_free_subroutines(module);
      return LAINIR_EXEC_FAILED;
    }
    for (L1Subroutine *sub = module; sub; sub = sub->next) {
      if (sub->is_extern && !sub->blocks) {
        const char *cap_name = sub->link_name ? sub->link_name : sub->name;
        if (!capability_is_allowed(request, cap_name)) {
          if (diagnostic) {
            diagnostic->code = 3006;
            snprintf(diagnostic->message, sizeof(diagnostic->message),
                     "compiler artifact requests unauthorized capability `%s`",
                     cap_name ? cap_name : "<unnamed>");
          }
          for (uint32_t i = 0; i < caps->count; i++)
            free(caps->entries[i].user_data);
          lainir_caps_free(caps);
          lainir_free_subroutines(module);
          return LAINIR_EXEC_FAILED;
        }
        LainirCapability *cap = malloc(sizeof(LainirCapability));
        if (!cap || !lainir_caps_add(
              caps, cap_name,
              scheme_host_call, cap)) {
          free(cap);
          if (diagnostic) {
            diagnostic->code = 3005;
            snprintf(diagnostic->message, sizeof(diagnostic->message),
                     "compiler artifact capability registration failed");
          }
          for (uint32_t i = 0; i < caps->count; i++)
            free(caps->entries[i].user_data);
          lainir_caps_free(caps);
          lainir_free_subroutines(module);
          return LAINIR_EXEC_FAILED;
        }
        cap->name = sub->link_name ? sub->link_name : sub->name;
        cap->fn = scheme_host_call;
        cap->user_data = &scheme_env;
      }
    }
    run_request.caps = caps;
  }
  run_status = lainir_run(&run_request, result_out, &run_error);
  if (run_status == LAINIR_RUN_OK && result_out &&
      result_out->kind == LAINIR_VALUE_STRING) {
    char *stable = strdup(result_out->as.string ? result_out->as.string : "");
    if (!stable) {
      run_status = LAINIR_RUN_TRAP;
      run_error = "could not retain LAIN-IR string result";
    } else {
      result_out->as.string = stable;
    }
  }
  if (caps) {
    for (uint32_t i = 0; i < caps->count; i++)
      free(caps->entries[i].user_data);
    lainir_caps_free(caps);
  }
  scheme_env_release_strings(&scheme_env);
  lainir_free_subroutines(module);

  if (run_status == LAINIR_RUN_OK)
    return LAINIR_EXEC_OK;
  if (diagnostic) {
    diagnostic->code = run_status == LAINIR_RUN_NO_ENTRY ? 3002 : 3003;
    snprintf(diagnostic->message, sizeof(diagnostic->message), "%s",
             run_error ? run_error : "generated LAIN-IR execution failed");
  }
  return run_status == LAINIR_RUN_NO_ENTRY ? LAINIR_EXEC_UNAVAILABLE
                                           : LAINIR_EXEC_FAILED;
}

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
    static char missing_capability[256];
    snprintf(missing_capability, sizeof(missing_capability),
             "extern capability not found in Scheme environment: %s",
             cap->name ? cap->name : "<unnamed>");
    *error_out = missing_capability;
    return LAINIR_RUN_BAD_CALL;
  }

  for (int i = (int)arg_count - 1; i >= 0; i--)
    arg_list = vm_cons(ctx, lainir_value_to_sexp(ctx, args[i]), arg_list);

  result = vm_apply(ctx, proc, arg_list);
  if (vm_is_exception(result)) {
    vm_print_exception(ctx, result);
    *error_out = "Scheme extern capability raised exception";
    return LAINIR_RUN_BAD_CALL;
  }
  if (!sexp_to_lainir_value(ctx, result, result_out)) {
    *error_out = "unsupported Scheme result value for lainir extern";
    return LAINIR_RUN_BAD_CALL;
  }
  if (!scheme_env_own_string(scheme_env, result_out)) {
    *error_out = "could not retain Scheme extern string result";
    return LAINIR_RUN_BAD_CALL;
  }
  if (strcmp(cap->name, "core.string-append-linear!") == 0) {
    for (uint32_t i = 0; i < arg_count; i++) {
      if (args[i].kind == LAINIR_VALUE_STRING)
        scheme_env_release_owned_pointer(scheme_env, args[i].as.string);
    }
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
  scheme_env_release_strings(&scheme_env);
  free(args);
  return exec_status;
}
