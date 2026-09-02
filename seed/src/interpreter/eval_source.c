#include "lainir/eval_source.h"
#include "lainir/parse.h"
#include "lainir/verify.h"
#include "lainir/emit.h"
#include <stdlib.h>
#include <string.h>

typedef struct { char *data; size_t length; size_t capacity; } EvalTextBuffer;
static int eval_text_write(void *context, const char *data, size_t length) {
  EvalTextBuffer *buffer = context;
  if (length > SIZE_MAX - buffer->length) return 0;
  size_t needed = buffer->length + length;
  if (needed > buffer->capacity) {
    size_t capacity = buffer->capacity ? buffer->capacity * 2 : 1024;
    while (capacity < needed) capacity *= 2;
    char *next = realloc(buffer->data, capacity);
    if (!next) return 0;
    buffer->data = next; buffer->capacity = capacity;
  }
  memcpy(buffer->data + buffer->length, data, length);
  buffer->length = needed;
  return 1;
}

struct LainirModuleHandle { L1Subroutine *module; };

typedef struct {
  uint64_t *values;
  size_t count;
  size_t capacity;
  int failed;
} EvalValues;

static void collect_eval_value(const LainirValue *value, void *user_data) {
  EvalValues *results = user_data;
  if (!results || !value || value->kind != LAINIR_VALUE_BITS) return;
  if (results->count == results->capacity) {
    size_t next_capacity = results->capacity ? results->capacity * 2 : 8;
    uint64_t *next = realloc(results->values, next_capacity * sizeof(*next));
    if (!next) { results->failed = 1; return; }
    results->values = next;
    results->capacity = next_capacity;
  }
  results->values[results->count++] = value->as.bits;
}

void lainir_eval_values_free(uint64_t *values) { free(values); }

static LainirRunStatus collect_module_eval_values(
    LainirModuleHandle *handle, LainirCapabilityTable *caps,
    uint64_t **values_out, size_t *count_out,
    L1Diagnostic *diagnostic, const char **error_out) {
  EvalValues results = {0};
  if (values_out) *values_out = NULL;
  if (count_out) *count_out = 0;
  if (error_out) *error_out = NULL;
  if (!handle || !handle->module || !values_out || !count_out) {
    if (error_out) *error_out = "invalid module eval arguments";
    return LAINIR_RUN_BAD_CALL;
  }
  if (lainir_fold_module_with_sink(handle->module, caps, error_out,
                                   collect_eval_value, &results) != LAINIR_RUN_OK ||
      results.failed) {
    if (results.failed && error_out) *error_out = "out of memory";
    free(results.values);
    return results.failed ? LAINIR_RUN_TRAP : LAINIR_RUN_BAD_CALL;
  }
  (void)diagnostic;
  *values_out = results.values;
  *count_out = results.count;
  return LAINIR_RUN_OK;
}

LainirRunStatus lainir_module_parse_handle(
    const char *source, LainirModuleHandle **handle_out,
    L1Diagnostic *diagnostic) {
  LainirModuleHandle *handle;
  if (handle_out) *handle_out = NULL;
  if (!source || !handle_out) return LAINIR_RUN_BAD_CALL;
  handle = calloc(1, sizeof(*handle));
  if (!handle) return LAINIR_RUN_TRAP;
  if (!lainir_parse_module_checked(source, &handle->module, diagnostic)) {
    free(handle);
    return LAINIR_RUN_BAD_CALL;
  }
  *handle_out = handle;
  return LAINIR_RUN_OK;
}

void lainir_module_free_handle(LainirModuleHandle *handle) {
  if (!handle) return;
  lainir_free_subroutines(handle->module);
  free(handle);
}

void lainir_module_handle_destroy(LainirModuleHandle **handle_ptr) {
  if (!handle_ptr || !*handle_ptr) return;
  lainir_module_free_handle(*handle_ptr);
  *handle_ptr = NULL;
}

const L1Subroutine *lainir_module_handle_first(
    const LainirModuleHandle *handle) {
  return handle ? handle->module : NULL;
}

LainirRunStatus lainir_module_handle_verify(
    LainirModuleHandle *handle, L1Diagnostic *diagnostic) {
  return lainir_module_handle_verify_entry(handle, NULL, diagnostic);
}

LainirRunStatus lainir_module_handle_verify_entry(
    LainirModuleHandle *handle, const char *entry_name,
    L1Diagnostic *diagnostic) {
  if (!handle || !handle->module) return LAINIR_RUN_BAD_CALL;
  return lainir_verify_module(handle->module, entry_name, diagnostic)
      ? LAINIR_RUN_OK : LAINIR_RUN_BAD_CALL;
}

LainirRunStatus lainir_module_handle_fold(
    LainirModuleHandle *handle, LainirCapabilityTable *caps,
    const char **error_out) {
  if (!handle || !handle->module) return LAINIR_RUN_BAD_CALL;
  return lainir_fold_module(handle->module, caps, error_out);
}

LainirRunStatus lainir_module_handle_eval_block(
    LainirModuleHandle *handle, L1Block *block, L1Type *return_type,
    LainirCapabilityTable *caps, LainirValue *result_out,
    const char **error_out) {
  if (!handle || !handle->module || !block || !result_out)
    return LAINIR_RUN_BAD_CALL;
  return lainir_eval_block(handle->module, block, return_type, caps,
                           result_out, error_out);
}

LainirRunStatus lainir_module_handle_eval_values(
    LainirModuleHandle *handle, LainirCapabilityTable *caps,
    uint64_t **values_out, size_t *count_out,
    L1Diagnostic *diagnostic, const char **error_out) {
  return collect_module_eval_values(handle, caps, values_out, count_out,
                                    diagnostic, error_out);
}

LainirRunStatus lainir_eval_source(
    const char *source, LainirCapabilityTable *caps,
    L1Subroutine **module_out, L1Diagnostic *diagnostic,
    const char **error_out) {
  L1Subroutine *module = NULL;
  if (module_out) *module_out = NULL;
  if (error_out) *error_out = NULL;
  if (!source || !module_out) {
    if (error_out) *error_out = "invalid eval source arguments";
    return LAINIR_RUN_BAD_CALL;
  }
  if (!lainir_parse_module_checked(source, &module, diagnostic)) {
    if (error_out && diagnostic) *error_out = diagnostic->message;
    return LAINIR_RUN_BAD_CALL;
  }
  if (!lainir_verify_module(module, NULL, diagnostic)) {
    if (error_out && diagnostic) *error_out = diagnostic->message;
    lainir_free_subroutines(module);
    return LAINIR_RUN_BAD_CALL;
  }
  if (lainir_fold_module(module, caps, error_out) != LAINIR_RUN_OK) {
    lainir_free_subroutines(module);
    return LAINIR_RUN_BAD_CALL;
  }
  *module_out = module;
  return LAINIR_RUN_OK;
}

LainirRunStatus lainir_eval_source_values(
    const char *source, LainirCapabilityTable *caps,
    uint64_t **values_out, size_t *count_out,
    L1Diagnostic *diagnostic, const char **error_out) {
  LainirModuleHandle *handle = NULL;
  LainirRunStatus status;
  if (values_out) *values_out = NULL;
  if (count_out) *count_out = 0;
  if (!values_out || !count_out) return LAINIR_RUN_BAD_CALL;
  if (!source) {
    if (error_out) *error_out = "invalid eval source arguments";
    return LAINIR_RUN_BAD_CALL;
  }
  status = lainir_module_parse_handle(source, &handle, diagnostic);
  if (status != LAINIR_RUN_OK) {
    if (error_out && diagnostic) *error_out = diagnostic->message;
    return status;
  }
  status = lainir_module_handle_verify(handle, diagnostic);
  if (status != LAINIR_RUN_OK) {
    if (error_out && diagnostic) *error_out = diagnostic->message;
    lainir_module_handle_destroy(&handle);
    return status;
  }
  status = lainir_module_handle_eval_values(handle, caps, values_out,
                                            count_out, diagnostic, error_out);
  lainir_module_handle_destroy(&handle);
  return status;
}

LainirRunStatus lainir_eval_source_text(
    const char *source, LainirCapabilityTable *caps,
    char **text_out, size_t *length_out, L1Diagnostic *diagnostic,
    const char **error_out) {
  L1Subroutine *module = NULL;
  EvalTextBuffer buffer = {0};
  if (text_out) *text_out = NULL;
  if (length_out) *length_out = 0;
  if (lainir_eval_source(source, caps, &module, diagnostic, error_out) != LAINIR_RUN_OK)
    return LAINIR_RUN_BAD_CALL;
  if (!text_out || !length_out || !lainir_emit_text_module(
          (LainirWriter){.context = &buffer, .write = eval_text_write},
          module, diagnostic)) {
    lainir_free_subroutines(module); free(buffer.data);
    if (error_out) *error_out = "failed to serialize evaluated module";
    return LAINIR_RUN_TRAP;
  }
  lainir_free_subroutines(module);
  *text_out = buffer.data; *length_out = buffer.length;
  return LAINIR_RUN_OK;
}
