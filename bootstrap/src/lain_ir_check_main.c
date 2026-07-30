#include "lainir.h"

#include <stdio.h>
#include <stdlib.h>

static char *read_file(const char *path) {
  FILE *file = fopen(path, "rb");
  long size;
  size_t actual;
  char *text;
  if (!file) return NULL;
  fseek(file, 0, SEEK_END);
  size = ftell(file);
  fseek(file, 0, SEEK_SET);
  text = malloc((size_t)size + 1);
  if (!text) { fclose(file); return NULL; }
  actual = fread(text, 1, (size_t)size, file);
  text[actual] = '\0';
  fclose(file);
  return text;
}

int main(int argc, char **argv) {
  char *source;
  L1Subroutine *module = NULL;
  L1Diagnostic diagnostic;
  const char *entry = NULL;
  if (argc < 2 || argc > 3) {
    fprintf(stderr, "usage: l1check <input.l1> [entry]\n");
    return 2;
  }
  if (argc == 3) entry = argv[2];
  source = read_file(argv[1]);
  if (!source) {
    fprintf(stderr, "cannot read %s\n", argv[1]);
    return 2;
  }
  if (!lainir_parse_module_checked(source, &module, &diagnostic)) {
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
  lainir_emit_text_module(stdout, module);
  lainir_free_subroutines(module);
  return 0;
}
