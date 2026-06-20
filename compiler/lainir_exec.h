#ifndef LAINIR_EXEC_H
#define LAINIR_EXEC_H

#include <chibi/eval.h>

typedef enum {
  LAINIR_EXEC_OK = 0,
  LAINIR_EXEC_UNAVAILABLE = 1,
  LAINIR_EXEC_FAILED = 2
} LainirExecStatus;

typedef struct {
  const char *entry_name;
  sexp args;
} LainirExecRequest;

LainirExecStatus lainir_exec_request(
  sexp ctx,
  sexp env,
  const LainirExecRequest *request,
  sexp *result_out);

#endif
