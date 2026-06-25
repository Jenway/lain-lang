#ifndef LAINIR_EXEC_H
#define LAINIR_EXEC_H

#include "vm_api.h"
#include "lainir/interpreter.h"

typedef enum {
  LAINIR_EXEC_OK = 0,
  LAINIR_EXEC_UNAVAILABLE = 1,
  LAINIR_EXEC_FAILED = 2
} LainirExecStatus;

typedef struct {
  const char *entry_name;
  vm_value *args;
} LainirExecRequest;

LainirExecStatus lainir_exec_request(
  vm_context *ctx,
  vm_value *env,
  const LainirExecRequest *request,
  vm_value **result_out);

#endif
