#ifndef LAINIR_VERIFY_H
#define LAINIR_VERIFY_H

#include "lainir/core.h"

int lainir_verify_module(
    L1Subroutine *module,
    const char *entry_name,
    L1Diagnostic *diagnostic);

#endif /* LAINIR_VERIFY_H */
