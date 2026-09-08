#ifndef LAINIR_INTERPRETER_H
#define LAINIR_INTERPRETER_H

#include "lainir/core.h"

typedef enum {
  LAINIR_RUN_OK = 0,
  LAINIR_RUN_NO_ENTRY = 1,
  LAINIR_RUN_BAD_CALL = 2,
  LAINIR_RUN_TRAP = 3,
  LAINIR_RUN_SLICE = 4,
  LAINIR_RUN_BLOCKED = 5
} LainirRunStatus;

typedef enum {
  LAINIR_VALUE_UNIT = 0,
  LAINIR_VALUE_BITS = 1,
  LAINIR_VALUE_ADDR = 2,
  LAINIR_VALUE_STRING = 3,
  LAINIR_VALUE_FUNC = 4
} LainirValueKind;

typedef struct {
  LainirValueKind kind;
  uint32_t bit_width;
  union {
    uint64_t bits;
    void *addr;
    const char *string;
    L1Subroutine *func;
  } as;
} LainirValue;

typedef LainirRunStatus (*LainirHostFn)(
  const LainirValue *args,
  uint32_t arg_count,
  LainirValue *result_out,
  const char **error_out,
  void *user_data);

typedef struct {
  const char *name;
  LainirHostFn fn;
  void *user_data;
} LainirCapabilityEntry;

typedef struct {
  LainirCapabilityEntry *entries;
  uint32_t count;
  uint32_t cap;
  /* Optional execution limits. Zero means unlimited. */
  uint64_t max_steps;
  uint32_t max_call_depth;
  uint64_t max_alloc_bytes;
  uint64_t max_eval_blocks;
} LainirCapabilityTable;

typedef struct {
  const char *name;
  LainirHostFn fn;
  void *user_data;
} LainirCapability;

typedef struct LainirVmControl LainirVmControl;
typedef struct LainirVmEndpoint LainirVmEndpoint;
typedef struct LainirVmScheduler LainirVmScheduler;

typedef struct {
  LainirVmEndpoint *endpoint;
  LainirVmControl *control;
  uint64_t owner;
} LainirVmEndpointBinding;

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
LainirVmSliceResult lainir_vm_scheduler_finish(
    LainirVmScheduler *scheduler, uint64_t owner);

/* Provider-owned single-sender/single-receiver rendezvous.  Waiting entries
 * retain only the TCB control object, its owner token, and a scalar payload;
 * they never retain an activation address.  send/receive return 0 when the
 * caller blocks, 1 when a rendezvous completes, and -1 on rejection. */
LainirVmEndpoint *lainir_vm_endpoint_new(uint64_t owner);
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

/* Optional observer invoked immediately after each scalar #eval is
 * materialized by lainir_fold_module.  The value is borrowed by the caller
 * and remains valid for the duration of the callback. */
typedef void (*LainirEvalSink)(const LainirValue *value, void *user_data);

LainirCapabilityTable *lainir_caps_new(void);
void lainir_caps_free(LainirCapabilityTable *caps);
int lainir_caps_add(
  LainirCapabilityTable *caps,
  const char *name,
  LainirHostFn fn,
  void *user_data);

void lainir_caps_set_limits(
  LainirCapabilityTable *caps,
  uint64_t max_steps,
  uint32_t max_call_depth,
  uint64_t max_alloc_bytes);

void lainir_caps_set_eval_limit(
  LainirCapabilityTable *caps,
  uint64_t max_eval_blocks);

LainirRunStatus lainir_run(
  const LainirRunRequest *request,
  LainirValue *result_out,
  const char **error_out);

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

/* Fold all #eval expressions in a module.  Scalar bit results are materialized
 * as constants. Unit results remain as evaluated EXPR_EVAL nodes for the
 * backend to lower; address and function results are rejected in this phase. */
LainirRunStatus lainir_fold_module(
  L1Subroutine *module,
  LainirCapabilityTable *caps,
  const char **error_out);

LainirRunStatus lainir_fold_module_with_sink(
  L1Subroutine *module,
  LainirCapabilityTable *caps,
  const char **error_out,
  LainirEvalSink sink,
  void *sink_user_data);

/* Read-only views used by compiler adapters.  Returned pointers are borrowed
 * from the module and remain valid only while it is alive.  Every accessor
 * accepts NULL and returns a neutral failure value; indexed accessors return
 * NULL when the index is out of bounds.  In particular, enum accessors return
 * -1 for a NULL object, while scalar widths/counts return 0. */
const L1Subroutine *lainir_module_first_procedure(const L1Subroutine *module);
const L1Subroutine *lainir_procedure_next(const L1Subroutine *procedure);
const char *lainir_procedure_name(const L1Subroutine *procedure);
uint32_t lainir_procedure_name_length(const L1Subroutine *procedure);
const char *lainir_procedure_link_name(const L1Subroutine *procedure);
const L1Type *lainir_procedure_return_type(const L1Subroutine *procedure);
int lainir_procedure_is_external(const L1Subroutine *procedure);
int lainir_procedure_is_data(const L1Subroutine *procedure);
uint32_t lainir_data_size(const L1Subroutine *data);
uint32_t lainir_data_alignment(const L1Subroutine *data);
const uint8_t *lainir_data_bytes(const L1Subroutine *data);
uint32_t lainir_procedure_parameter_count(const L1Subroutine *procedure);
const L1Type *lainir_procedure_parameter_type(const L1Subroutine *procedure, uint32_t index);
const char *lainir_procedure_parameter_name(const L1Subroutine *procedure, uint32_t index);
const L1Block *lainir_procedure_first_block(const L1Subroutine *procedure);
const L1Block *lainir_block_next(const L1Block *block);
L1ExprKind lainir_expr_kind(const L1Expr *expr);
const L1Type *lainir_expr_type(const L1Expr *expr);
const L1Expr *lainir_expr_left(const L1Expr *expr);
const L1Expr *lainir_expr_right(const L1Expr *expr);
/* For LEA, left/right are the base/index operands; for binary expressions
 * they are the ordinary left/right operands. */
/* Expressions are stored as arrays for call operands, so this
 * legacy linked-list view always returns NULL.  Use argument_count and
 * argument_at for operand traversal. */
const L1Expr *lainir_expr_next(const L1Expr *expr);
uint32_t lainir_expr_argument_count(const L1Expr *expr);
const L1Expr *lainir_expr_argument_at(const L1Expr *expr, uint32_t index);
int64_t lainir_expr_const_value(const L1Expr *expr);
uint32_t lainir_expr_arg_index(const L1Expr *expr);
const char *lainir_expr_name(const L1Expr *expr);
const char *lainir_expr_callee_name(const L1Expr *expr);
const char *lainir_expr_string(const L1Expr *expr);
const L1Expr *lainir_expr_operand(const L1Expr *expr);
const L1Block *lainir_expr_block(const L1Expr *expr);
uint32_t lainir_expr_scale(const L1Expr *expr);
uint32_t lainir_expr_offset(const L1Expr *expr);
uint32_t lainir_expr_byte_size(const L1Expr *expr);
const char *lainir_diagnostic_message(const L1Diagnostic *diagnostic);
int lainir_diagnostic_code(const L1Diagnostic *diagnostic);
int lainir_diagnostic_line(const L1Diagnostic *diagnostic);
int lainir_diagnostic_column(const L1Diagnostic *diagnostic);
void lainir_diagnostic_clear(L1Diagnostic *diagnostic);
int lainir_type_kind(const L1Type *type);
uint32_t lainir_type_width(const L1Type *type);
const L1Instruction *lainir_block_first_instruction(const L1Block *block);
const L1Instruction *lainir_instruction_next(const L1Instruction *instruction);
L1InstKind lainir_instruction_kind(const L1Instruction *instruction);
const char *lainir_instruction_name(const L1Instruction *instruction);
const char *lainir_instruction_label(const L1Instruction *instruction);
const L1Type *lainir_instruction_type(const L1Instruction *instruction);
const L1Expr *lainir_instruction_value(const L1Instruction *instruction);
const L1Expr *lainir_instruction_destination(const L1Instruction *instruction);
const L1Expr *lainir_instruction_condition(const L1Instruction *instruction);
const L1Block *lainir_instruction_then_block(const L1Instruction *instruction);
const L1Block *lainir_instruction_else_block(const L1Instruction *instruction);
const L1Block *lainir_instruction_loop_block(const L1Instruction *instruction);

LainirValue lainir_value_unit(void);
LainirValue lainir_value_bits(uint64_t bits, uint32_t bit_width);
LainirValue lainir_value_addr(void *addr);
LainirValue lainir_value_string(const char *string);
LainirValue lainir_value_func(L1Subroutine *func);

#endif
