#include "lainir/emit.h"
#include "lainir/parse.h"
#include "lainir/verify.h"
#include "host_io.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
  unsigned char *source;
  size_t source_length;
  L1Subroutine *module = NULL;
  L1Diagnostic diagnostic;
  const char *entry = NULL;
  if (argc < 2 || argc > 3) {
    fprintf(stderr, "usage: l1check <input.l1> [entry]\n");
    return 2;
  }
  if (argc == 3) entry = argv[2];
  source = lainir_host_read_file(argv[1], &source_length);
  if (!source) {
    fprintf(stderr, "cannot read %s\n", argv[1]);
    return 2;
  }
  if (!lainir_parse_module_checked(
          (const char *)source, &module, &diagnostic)) {
    fprintf(stderr, "parse[%d] line %d: %s\n", diagnostic.code,
            diagnostic.line, diagnostic.message);
    free(source);
    return 1;
  }
  free(source);
  if (!lainir_verify_module(module, entry, &diagnostic)) {
    fprintf(stderr, "verify[%d]: %s\n", diagnostic.code, diagnostic.message);
    lainir_free_subroutines(module);
    return 1;
  }
  if (!lainir_emit_text_module(
          lainir_host_file_writer(stdout), module, &diagnostic)) {
    fprintf(stderr, "emit[%d]: %s\n", diagnostic.code, diagnostic.message);
    lainir_free_subroutines(module);
    return 1;
  }
  lainir_free_subroutines(module);
  return 0;
}
