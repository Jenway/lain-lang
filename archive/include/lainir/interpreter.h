#ifndef LAINIR_INTERPRETER_H
#define LAINIR_INTERPRETER_H

/* Transitional umbrella over the four headers that replaced this file.
 *
 * This header used to declare four unrelated APIs at once — the physical value
 * model, the read-only Artifact views, the `#eval` fold stage and the whole
 * LAINVM control plane — so no include graph could express the layering that
 * seed/REFACTOR_PLAN.md requires
 * (`parse/verify/Artifact -> eval_fold -> VM execute`), and LAINIR tools such
 * as `lainir-print` pulled in the VM implementation file by including it.
 *
 * The declarations now live where their layer does:
 *
 *   lainir/artifact.h   value model, host-capability table, read-only views
 *   lainir/eval_fold.h  #eval execution and folding (a LAINIR concept)
 *   lainvm/execute.h    one-shot execution of one verified procedure
 *   lainvm/control.h    TCBs, scheduler, endpoints, slices, Trap
 *
 * This umbrella declares nothing of its own and is kept only so that
 * out-of-tree consumers keep compiling.  Do not add declarations here; include
 * the narrow header, and see seed/REFACTOR_PLAN.md section 2 for the schedule
 * that removes this file. */

#include "lainir/artifact.h"
#include "lainir/eval_fold.h"
#include "lainvm/execute.h"
#include "lainvm/control.h"

#endif /* LAINIR_INTERPRETER_H */
