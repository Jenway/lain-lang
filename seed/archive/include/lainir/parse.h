#ifndef LAINIR_PARSE_H
#define LAINIR_PARSE_H

#include "lainir/builder.h"

/* Parse one module into `builder`.  Every node and name becomes owned by that
 * builder; the caller reads the result through lainir_builder_root() and
 * releases everything with lainir_builder_free().  On failure nothing is
 * published and the builder stays usable (the caller still owns it). */
L1Subroutine *lainir_parse_module(L1Builder *builder, const char *source);
int lainir_parse_module_checked(
  L1Builder *builder,
  const char *source,
  L1Subroutine **module_out,
  L1Diagnostic *diagnostic);

#endif /* LAINIR_PARSE_H */
