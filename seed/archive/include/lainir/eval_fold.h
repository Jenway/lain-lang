#ifndef LAINIR_EVAL_FOLD_H
#define LAINIR_EVAL_FOLD_H

/* The LAINIR compile-time evaluation stage: `#eval` blocks are executed and
 * their results written back into the Artifact.
 *
 * This is a LAINIR concept, not a VM concept — "this already-lowered code runs
 * at compile time" is a property of the Artifact, and the VM only provides the
 * primitives used to carry it out.  The stage therefore sits above the VM in
 * the dependency order (parse/verify/Artifact -> eval_fold -> VM execute) and
 * must not include lainvm/.
 *
 * Transitional: the entry points below take the host-capability table directly
 * and the implementation currently sits in src/interpreter/interpreter.c
 * (step 1 of seed/REFACTOR_PLAN.md drafts the replacement, fold_evals(Artifact,
 * VmExecutor, authority, diagnostic); step 2 moves the implementation to
 * src/core/eval_fold.c). */

#include "lainir/artifact.h"
#include "lainir/builder.h"

/* Optional observer invoked immediately after each scalar #eval is
 * materialized by lainir_fold_module.  The value is borrowed by the caller
 * and remains valid for the duration of the callback. */
typedef void (*LainirEvalSink)(const LainirValue *value, void *user_data);

/* Execute one already-parsed block in an explicit compile-time context.
 * The block is borrowed; the interpreter does not free it.  This is the
 * entry point a compiler uses for #eval. */
LainirRunStatus lainir_eval_block(
  L1Subroutine *module,
  L1Block *block,
  L1Type *return_type,
  LainirCapabilityTable *caps,
  LainirValue *result_out,
  const char **error_out);

/* Fold all #eval expressions in a module, replacing each with the LAINIR
 * expression its result materializes to (see fold_value_expr in
 * src/interpreter/interpreter.c):
 *
 *   #bits   -> EXPR_CONST
 *   #string -> EXPR_STRING
 *   #proc   -> EXPR_PROC_ADDR
 *   #addr   -> EXPR_DATA_ADDR, but only when the address is exactly a #data
 *              object's bytes; any other address has no stable LAINIR
 *              representation and fails the fold
 *   #unit   -> the evaluated node is left in place for the backend
 *
 * The earlier comment here claimed address and function results were rejected
 * in this phase; the implementation has materialized both for some time.
 *
 * Folding rewrites the artifact, so the materialized nodes come from the
 * builder that owns it: the builder is an input, not an ambient owner. */
LainirRunStatus lainir_fold_module(
  L1Builder *builder,
  L1Subroutine *module,
  LainirCapabilityTable *caps,
  const char **error_out);

LainirRunStatus lainir_fold_module_with_sink(
  L1Builder *builder,
  L1Subroutine *module,
  LainirCapabilityTable *caps,
  const char **error_out,
  LainirEvalSink sink,
  void *sink_user_data);

#endif /* LAINIR_EVAL_FOLD_H */
