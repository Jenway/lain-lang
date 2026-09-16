#ifndef LAINVM_CONTROL_H
#define LAINVM_CONTROL_H

/* LAINVM control plane: TCBs, the single-current scheduler, rendezvous
 * endpoints, slices and Trap records.
 *
 * These are execution state, not LAINIR types, values or instructions: no
 * instruction refers to a TCB, and a TCB address never enters a program.
 * Nothing here reaches the backend — the backend input is a verified Artifact.
 *
 * The header was separated from lainir/interpreter.h by step 2 of
 * seed/REFACTOR_PLAN.md ("保持行为的机械拆分"); the declarations are unchanged. */

#include "lainvm/execute.h"

typedef struct LainirVmEndpoint LainirVmEndpoint;
typedef struct LainirVmScheduler LainirVmScheduler;
typedef void (*LainirVmBackendStateFree)(void *state);

typedef struct {
  LainirVmEndpoint *endpoint;
  LainirVmControl *control;
  uint64_t owner;
} LainirVmEndpointBinding;

/* Opaque VM control plane.  The execution backend owns this object; it is
 * deliberately separate from LainirRunRequest, which remains one-shot. */
typedef enum {
  LAINIR_VM_READY = 0,
  LAINIR_VM_RUNNING = 1,
  LAINIR_VM_BLOCKED = 2,
  LAINIR_VM_DEAD = 3
} LainirVmState;

typedef enum {
  LAINIR_VM_RUNNABLE = 0,
  LAINIR_VM_BLOCKED_RESULT = 1,
  LAINIR_VM_DONE = 2,
  LAINIR_VM_TRAPPED = 3
} LainirVmSliceResult;

typedef enum {
  LAINIR_VM_TRAP_NONE = 0,
  LAINIR_VM_TRAP_INTERPRETER = 1,
  LAINIR_VM_TRAP_CAPABILITY = 2,
  LAINIR_VM_TRAP_STATE = 3
} LainirVmTrapKind;

typedef struct {
  LainirVmTrapKind kind;
  int32_t status;
  uint64_t procedure;
  uint64_t region;
  uint64_t position;
  uint32_t line;
  uint32_t column;
  uint64_t source_start;
  uint64_t source_end;
  int active;
} LainirVmTrap;

enum {
  LAINIR_VM_SUSPEND_YIELD = 1,
  LAINIR_VM_SUSPEND_ENDPOINT = 2
};

enum {
  LAINIR_VM_ENDPOINT_RESULT_SEND = 1,
  LAINIR_VM_ENDPOINT_RESULT_RECEIVE = 2,
  LAINIR_VM_ENDPOINT_RESULT_CANCELLED = 3
};

typedef struct {
  uint64_t procedure;
  uint64_t region;
  uint64_t position;
  uint64_t activation;
  uint64_t return_procedure;
  uint64_t return_region;
  uint64_t return_position;
} LainirVmFrame;

LainirVmControl *lainir_vm_control_new(uint64_t max_steps);
void lainir_vm_control_free(LainirVmControl *control);

LainirVmState lainir_vm_control_state(const LainirVmControl *control);
uint32_t lainir_vm_control_suspend_reason(const LainirVmControl *control);
uint64_t lainir_vm_control_steps(const LainirVmControl *control);
int lainir_vm_control_start(LainirVmControl *control, uint64_t owner);
int lainir_vm_control_suspend(LainirVmControl *control, uint64_t owner,
                               uint32_t reason);
int lainir_vm_control_resume(LainirVmControl *control, uint64_t owner);
int lainir_vm_control_begin_slice(LainirVmControl *control, uint64_t owner,
                                   uint64_t fuel);
int lainir_vm_control_consume_step(LainirVmControl *control, uint64_t owner);
int lainir_vm_control_slice_exhausted(const LainirVmControl *control);
LainirVmSliceResult lainir_vm_control_slice_result(
    const LainirVmControl *control);
int lainir_vm_control_push_frame(LainirVmControl *control, uint64_t owner,
                                 uint64_t procedure, uint64_t region,
                                 uint64_t activation);
int lainir_vm_control_set_position(LainirVmControl *control, uint64_t owner,
                                   uint64_t region, uint64_t position);
int lainir_vm_control_pop_frame(LainirVmControl *control, uint64_t owner);
const LainirVmFrame *lainir_vm_control_current_frame(
    const LainirVmControl *control);
const LainirVmTrap *lainir_vm_control_trap(const LainirVmControl *control);
/* Copy and acknowledge the terminal Trap for scheduler consumption.  The
 * TCB remains DEAD/TRAPPED after acknowledgement; only the diagnostic record
 * is cleared from the control plane. */
int lainir_vm_control_take_trap(LainirVmControl *control, uint64_t owner,
                                LainirVmTrap *trap_out);
int lainir_vm_control_record_trap(LainirVmControl *control, uint64_t owner,
                                   LainirVmTrapKind kind, int32_t status);
int lainir_vm_control_record_trap_at(
    LainirVmControl *control, uint64_t owner, LainirVmTrapKind kind,
    int32_t status, uint32_t line, uint32_t column);
int lainir_vm_control_record_trap_span(
    LainirVmControl *control, uint64_t owner, LainirVmTrapKind kind,
    int32_t status, uint32_t line, uint32_t column,
    uint64_t source_start, uint64_t source_end);
void *lainir_vm_control_backend_state(const LainirVmControl *control);
int lainir_vm_control_set_backend_state(LainirVmControl *control,
                                         uint64_t owner, void *state);
int lainir_vm_control_set_backend_state_destructor(
    LainirVmControl *control, uint64_t owner, LainirVmBackendStateFree destroy);
int lainir_vm_control_take_endpoint_result(LainirVmControl *control,
                                            uint64_t owner, uint32_t *kind_out,
                                            uint64_t *value_out);
int lainir_vm_control_has_endpoint_result(const LainirVmControl *control,
                                           uint64_t owner,
                                           uint32_t *kind_out);
LainirVmSliceResult lainir_vm_control_finish(LainirVmControl *control,
                                             uint64_t owner);
LainirVmSliceResult lainir_vm_control_abort(LainirVmControl *control,
                                            uint64_t owner);

/* Opaque single-current scheduler control plane.  It owns no TCB memory; it
 * only enforces the current-slot and owner/state transitions. */
LainirVmScheduler *lainir_vm_scheduler_new(uint64_t owner);
void lainir_vm_scheduler_free(LainirVmScheduler *scheduler);
int lainir_vm_scheduler_attach(LainirVmScheduler *scheduler, uint64_t owner,
                               LainirVmControl *control, uint64_t control_owner);
LainirVmControl *lainir_vm_scheduler_current(
    const LainirVmScheduler *scheduler);
int lainir_vm_scheduler_start(LainirVmScheduler *scheduler, uint64_t owner,
                              LainirVmControl *control, uint64_t control_owner);
int lainir_vm_scheduler_suspend(LainirVmScheduler *scheduler, uint64_t owner,
                                uint32_t reason);
int lainir_vm_scheduler_resume(LainirVmScheduler *scheduler, uint64_t owner,
                               LainirVmControl *control, uint64_t control_owner);
int lainir_vm_scheduler_release(LainirVmScheduler *scheduler, uint64_t owner);
int lainir_vm_scheduler_admit(LainirVmScheduler *scheduler, uint64_t owner,
                              LainirVmControl *control, uint64_t control_owner);
/* Select the next attached READY or awakened RUNNING TCB.  Selection is
 * round-robin over the attachment order and leaves BLOCKED/DEAD TCBs alone. */
LainirVmControl *lainir_vm_scheduler_select(
    LainirVmScheduler *scheduler, uint64_t owner, uint64_t *control_owner_out);
/* Yield a RUNNING current TCB after its slice fuel is exhausted.  The TCB
 * remains RUNNING and can be selected again; this only releases current. */
int lainir_vm_scheduler_yield(LainirVmScheduler *scheduler, uint64_t owner);
/* Run one evaluator slice for the scheduler's current TCB.  The scheduler
 * supplies the opaque control object to the backend and automatically
 * releases current when the slice blocks or terminates. */
LainirRunStatus lainir_vm_scheduler_run(
    LainirVmScheduler *scheduler, uint64_t owner, uint64_t fuel,
    const LainirRunRequest *request, LainirValue *result_out,
    const char **error_out);
/* Run one slice and expose the scheduler-level outcome.  run_status_out is
 * the underlying evaluator status; the return value is the TCB transition
 * result used by scheduler policy. */
LainirVmSliceResult lainir_vm_scheduler_run_slice(
    LainirVmScheduler *scheduler, uint64_t owner, uint64_t fuel,
    const LainirRunRequest *request, LainirRunStatus *run_status_out,
    LainirValue *result_out, const char **error_out);
LainirVmSliceResult lainir_vm_scheduler_finish(
    LainirVmScheduler *scheduler, uint64_t owner);

/* Provider-owned single-sender/single-receiver rendezvous.  Waiting entries
 * retain only the TCB control object, its owner token, and a scalar payload;
 * they never retain an activation address.  send/receive return 0 when the
 * caller blocks, 1 when a rendezvous completes, and -1 on rejection. */
LainirVmEndpoint *lainir_vm_endpoint_new(uint64_t owner);
/* Freeing an endpoint cancels any blocked sender/receiver and publishes a
 * CANCELLED result before releasing the endpoint object. */
void lainir_vm_endpoint_free(LainirVmEndpoint *endpoint);
int lainir_vm_endpoint_send(LainirVmEndpoint *endpoint, uint64_t owner,
                            LainirVmControl *sender, uint64_t sender_owner,
                            uint64_t value);
int lainir_vm_endpoint_receive(LainirVmEndpoint *endpoint, uint64_t owner,
                               LainirVmControl *receiver,
                               uint64_t receiver_owner, uint64_t *value_out);
int lainir_vm_endpoint_cancel(LainirVmEndpoint *endpoint, uint64_t owner,
                              LainirVmControl *control, uint64_t control_owner);
int lainir_vm_endpoint_bind(LainirCapabilityTable *caps,
                            const char *send_name, const char *receive_name,
                            LainirVmEndpointBinding *binding);

#endif /* LAINVM_CONTROL_H */
