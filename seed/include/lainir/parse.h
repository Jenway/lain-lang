#ifndef LAINIR_PARSE_H
#define LAINIR_PARSE_H

#include "lainir/core.h"

L1Subroutine *lainir_parse_module(const char *source);
int lainir_parse_module_checked(
  const char *source,
  L1Subroutine **module_out,
  L1Diagnostic *diagnostic);

/* Strict mode accepts only canonical physical type spellings. */
int lainir_parse_module_checked_strict(
  const char *source,
  L1Subroutine **module_out,
  L1Diagnostic *diagnostic);

#endif /* LAINIR_PARSE_H */
