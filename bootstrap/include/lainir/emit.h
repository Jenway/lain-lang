#ifndef LAINIR_EMIT_H
#define LAINIR_EMIT_H

#include "lainir/core.h"

typedef int (*LainirWriteFn)(
    void *context,
    const char *data,
    size_t length);

typedef struct {
  void *context;
  LainirWriteFn write;
} LainirWriter;

int lainir_emit_text_module(
    LainirWriter writer,
    L1Subroutine *module,
    L1Diagnostic *diagnostic);

#endif /* LAINIR_EMIT_H */
