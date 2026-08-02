#include "lainir/emit.h"
#include "lainir/parse.h"
#include "lainir/verify.h"
#include "lainir/interpreter.h"
#include "host_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
  int strict = 0;
  int fold_eval = 0;
  int input_index = 1;
  unsigned char *source;
  size_t source_length;
  L1Subroutine *module = NULL;
  L1Diagnostic diagnostic;
  const char *entry = NULL;
  while (input_index < argc && argv[input_index][0] == '-') {
    if (strcmp(argv[input_index], "--strict") == 0) strict = 1;
    else if (strcmp(argv[input_index], "--fold-eval") == 0) fold_eval = 1;
    else break;
    input_index++;
  }
  if (argc < input_index + 1 || argc > input_index + 2) {
    fprintf(stderr, "usage: l1check [--strict] [--fold-eval] <input.l1> [entry]\n");
    return 2;
  }
  if (argc == input_index + 2) entry = argv[input_index + 1];
  source = lainir_host_read_file(argv[input_index], &source_length);
  if (!source) {
    fprintf(stderr, "cannot read %s\n", argv[input_index]);
    return 2;
  }
  if (!(strict ? lainir_parse_module_checked_strict(
                  (const char *)source, &module, &diagnostic)
              : lainir_parse_module_checked(
                  (const char *)source, &module, &diagnostic))) {
    fprintf(stderr, "parse[%d] line %d: %s\n", diagnostic.code,
            diagnostic.line, diagnostic.message);
    free(source);
    return 1;
  }
  free(source);
  if (!lainir_verify_module(module, entry, &diagnostic)) {
    fprintf(stderr, "verify[%d] line %d:%d: %s\n", diagnostic.code,
            diagnostic.line, diagnostic.column, diagnostic.message);
    lainir_free_subroutines(module);
    return 1;
  }
  if (fold_eval) {
    const char *error = NULL;
    if (lainir_fold_module(module, NULL, &error) != LAINIR_RUN_OK) {
      fprintf(stderr, "fold-eval: %s\n", error ? error : "failed");
      lainir_free_subroutines(module);
      return 1;
    }
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
