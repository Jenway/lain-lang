#ifndef LAINIR_EVAL_SOURCE_H
#define LAINIR_EVAL_SOURCE_H

/* Transitional: this header still declares parse, verify, fold and direct-run
 * entries together.  Step 1 of seed/REFACTOR_PLAN.md splits it so that a folded
 * Artifact and a direct procedure run stop looking like one request; until then
 * it only needs the artifact-side types. */
#include "lainir/artifact.h"

typedef struct LainirModuleHandle LainirModuleHandle;

LainirRunStatus lainir_module_parse_handle(
    const char *source, LainirModuleHandle **handle_out,
    L1Diagnostic *diagnostic);
void lainir_module_free_handle(LainirModuleHandle *handle);
void lainir_module_handle_destroy(LainirModuleHandle **handle_ptr);
const L1Subroutine *lainir_module_handle_first(
    const LainirModuleHandle *handle);
const L1Subroutine *lainir_module_handle_find_procedure(
    const LainirModuleHandle *handle, const char *name, size_t name_length);
LainirRunStatus lainir_module_handle_verify(
    LainirModuleHandle *handle, L1Diagnostic *diagnostic);
LainirRunStatus lainir_module_handle_verify_entry(
    LainirModuleHandle *handle, const char *entry_name,
    L1Diagnostic *diagnostic);
LainirRunStatus lainir_module_handle_fold(
    LainirModuleHandle *handle, LainirCapabilityTable *caps,
    const char **error_out);
LainirRunStatus lainir_module_handle_eval_block(
    LainirModuleHandle *handle, L1Block *block, L1Type *return_type,
    LainirCapabilityTable *caps, LainirValue *result_out,
    const char **error_out);
LainirRunStatus lainir_module_handle_eval_values(
    LainirModuleHandle *handle, LainirCapabilityTable *caps,
    uint64_t **values_out, size_t *count_out,
    L1Diagnostic *diagnostic, const char **error_out);
LainirRunStatus lainir_module_handle_run(
    LainirModuleHandle *handle, const L1Subroutine *procedure,
    const LainirValue *arguments, uint32_t argument_count,
    LainirCapabilityTable *caps, LainirValue *result_out,
    L1Diagnostic *diagnostic, const char **error_out);
void lainir_eval_values_free(uint64_t *values);

/* Parse, verify and fold a source module, returning the artifact handle that
 * owns it.  The caller releases it with lainir_module_free_handle(); the raw
 * module pointer is never handed out on its own, because the module must not
 * outlive its builder. */
LainirRunStatus lainir_eval_source(
    const char *source, LainirCapabilityTable *caps,
    LainirModuleHandle **handle_out, L1Diagnostic *diagnostic,
    const char **error_out);

LainirRunStatus lainir_eval_source_text(
    const char *source, LainirCapabilityTable *caps,
    char **text_out, size_t *length_out, L1Diagnostic *diagnostic,
    const char **error_out);

/* Evaluate and fold a source module, returning the scalar results of its
 * #eval expressions in execution order.  The caller owns *values_out. */
LainirRunStatus lainir_eval_source_values(
    const char *source, LainirCapabilityTable *caps,
    uint64_t **values_out, size_t *count_out,
    L1Diagnostic *diagnostic, const char **error_out);

#endif
