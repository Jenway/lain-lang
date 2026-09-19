#ifndef LAINVM_EXECUTE_H
#define LAINVM_EXECUTE_H

/* LAINVM execution surface: run one verified procedure of an Artifact.
 *
 * The VM may depend on the verified Artifact's read-only views (it receives a
 * module pointer) and must not call back into the fold stage or the emitter.
 * See seed/REFACTOR_PLAN.md section "对外接口" for the target shape; this file
 * is the mechanical split of what the VM entry point needs today.
 *
 * Transitional: `LainirRunRequest.caps` is the caller's whole capability table
 * and doubles as the execution authority.  Step 3 of the plan replaces it with
 * an explicit ExecutionAuthority from lainvm/authority.h and adds
 * execute_child for narrowed sub-executions. */

#include "lainir/artifact.h"

/* Defined by lainvm/control.h.  Forward-declared here because the one-shot
 * request names it while the control plane depends on this header. */
typedef struct LainirVmControl LainirVmControl;

typedef struct {
  L1Subroutine *module;
  const char *entry_name;
  const LainirValue *args;
  uint32_t arg_count;
  LainirCapabilityTable *caps;
  /* Optional backend-owned control plane.  A non-NULL value enables the
   * instruction fuel gate and may retain a resumable root continuation. */
  LainirVmControl *vm_control;
  uint64_t vm_owner;
} LainirRunRequest;

LainirRunStatus lainir_run(
    const LainirRunRequest *request,
    LainirValue *result_out,
    const char **error_out);

#endif /* LAINVM_EXECUTE_H */
