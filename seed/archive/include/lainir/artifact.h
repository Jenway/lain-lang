#ifndef LAINIR_ARTIFACT_H
#define LAINIR_ARTIFACT_H

/* LAINIR-side surface: the physical value model, the host-capability table a
 * caller must supply, and the read-only views over a parsed Artifact.
 *
 * This header owns everything a LAINIR consumer needs that is not the VM's
 * control plane: the canonical-text printer, the backend, and the fold stage
 * all include this and nothing below it.  See seed/REFACTOR_PLAN.md section
 * "对外接口" for the target shape; section 2 of that plan ("保持行为的机械拆分")
 * is the change that produced this file. */

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

/* Host capability registration.
 *
 * This table is the host-call surface today: the fold stage passes it through
 * to the VM so that a `#eval` block can reach an external operation, and the
 * VM entry point takes it as its authority.  Both stages therefore share the
 * type, which is why it lives here rather than in lainvm/.
 *
 * Transitional: step 3 of seed/REFACTOR_PLAN.md replaces the implicit
 * "whatever the caller's table holds" authority with the explicit
 * region/grant authority in lainvm/authority.h.  When that lands, this section
 * moves to the VM side and the fold stage stops naming it. */
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

/* The physical type an expression yields, as a pure query.
 *
 * The result is either borrowed from the expression (its own annotated type)
 * or one of a small set of immutable process-wide constants for the shapes the
 * IR synthesizes (#bits<64> for a literal, #bits<1> for a comparison, #addr for
 * an address-producing operator).  The caller never owns it and must not free
 * it.
 *
 * This is the query the verifier uses to decide operand compatibility and the
 * one the emitter and the C backend need in order to spell a value's physical
 * type.  Diagnostic codes 2015-2018 depend on its answers, so its return values
 * are observable IR behaviour. */
L1Type *infer_expr_type(L1Expr *expr);

#endif /* LAINIR_ARTIFACT_H */
