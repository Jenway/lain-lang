#ifndef LAINIR_INTERPRETER_H
#define LAINIR_INTERPRETER_H

#include "lainir/core.h"

typedef enum {
  LAINIR_RUN_OK = 0,
  LAINIR_RUN_NO_ENTRY = 1,
  LAINIR_RUN_BAD_CALL = 2,
  LAINIR_RUN_TRAP = 3
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

typedef struct {
  L1Subroutine *module;
  const char *entry_name;
  const LainirValue *args;
  uint32_t arg_count;
  LainirCapabilityTable *caps;
} LainirRunRequest;

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

/* Fold all #eval expressions in a module.  Only scalar bit results are
 * materialized; address, function and unit results remain invalid in value
 * positions and return LAINIR_RUN_BAD_CALL. */
LainirRunStatus lainir_fold_module(
  L1Subroutine *module,
  LainirCapabilityTable *caps,
  const char **error_out);

LainirValue lainir_value_unit(void);
LainirValue lainir_value_bits(uint64_t bits, uint32_t bit_width);
LainirValue lainir_value_addr(void *addr);
LainirValue lainir_value_string(const char *string);
LainirValue lainir_value_func(L1Subroutine *func);

#endif
