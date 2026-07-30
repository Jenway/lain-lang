#ifndef LAINIR_INTERPRETER_H
#define LAINIR_INTERPRETER_H

#include "lainir/lainir.h"

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

LainirRunStatus lainir_run(
  const LainirRunRequest *request,
  LainirValue *result_out,
  const char **error_out);

LainirValue lainir_value_unit(void);
LainirValue lainir_value_bits(uint64_t bits, uint32_t bit_width);
LainirValue lainir_value_addr(void *addr);
LainirValue lainir_value_string(const char *string);
LainirValue lainir_value_func(L1Subroutine *func);

#endif
