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

typedef struct {
  const char *text;
  const char *entry_name;
  const LainirValue *args;
  uint32_t arg_count;
  vm_context *host_ctx;
  vm_value *host_env;
  const char *const *allowed_capabilities;
  uint32_t allowed_capability_count;
} LainirExecTextRequest;

/* Coarse-grained bootstrap capability.  The caller owns the source text;
 * parsing creates a private module which is always released before return.
 * A successful STRING result is heap-owned by the caller. */
LainirExecStatus lainir_exec_text_request(
  const LainirExecTextRequest *request,
  LainirValue *result_out,
  L1Diagnostic *diagnostic);

LainirExecStatus lainir_exec_request(
  vm_context *ctx,
  vm_value *env,
  const LainirExecRequest *request,
  vm_value **result_out);

#endif
