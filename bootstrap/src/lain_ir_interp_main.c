#include "interpreter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file(const char *path) {
  FILE *f = fopen(path, "r");
  char *buf;
  long sz;
  size_t read_sz;
  if (!f) {
    perror(path);
    return NULL;
  }
  fseek(f, 0, SEEK_END);
  sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  buf = malloc(sz + 1);
  if (!buf) {
    fclose(f);
    return NULL;
  }
  read_sz = fread(buf, 1, sz, f);
  buf[read_sz] = 0;
  fclose(f);
  return buf;
}

static int parse_arg_value(const char *text, LainirValue *out) {
  char *end = NULL;
  unsigned long long v = strtoull(text, &end, 10);
  if (!text[0] || (end && *end))
    return 0;
  *out = lainir_value_bits((uint64_t)v, 32);
  return 1;
}

int main(int argc, char **argv) {
  const char *entry;
  char *src;
  L1Subroutine *module;
  LainirValue *args = NULL;
  LainirValue result = lainir_value_unit();
  LainirRunRequest request;
  const char *error = NULL;
  LainirRunStatus status;
  L1Diagnostic diagnostic;

  if (argc < 3) {
    fprintf(stderr, "usage: l1i <input.l1> <entry> [arg ...]\n");
    return 1;
  }

  src = read_file(argv[1]);
  if (!src)
    return 1;
  if (!lainir_parse_module_checked(src, &module, &diagnostic)) {
    fprintf(stderr, "lainir parse error [%d] line %d: %s\n", diagnostic.code,
            diagnostic.line, diagnostic.message);
    free(src);
    return 1;
  }
  free(src);

  if (!lainir_verify_module(module, argv[2], &diagnostic)) {
    fprintf(stderr, "lainir verify error [%d]: %s\n", diagnostic.code,
            diagnostic.message);
    lainir_free_subroutines(module);
    return 1;
  }

  if (argc > 3) {
    args = calloc((size_t)(argc - 3), sizeof(LainirValue));
    if (!args) {
      lainir_free_subroutines(module);
      return 1;
    }
    for (int i = 3; i < argc; i++) {
      if (!parse_arg_value(argv[i], &args[i - 3])) {
        fprintf(stderr, "invalid integer argument: %s\n", argv[i]);
        free(args);
        lainir_free_subroutines(module);
        return 1;
      }
    }
  }

  entry = argv[2];
  request.module = module;
  request.entry_name = entry;
  request.args = args;
  request.arg_count = argc > 3 ? (uint32_t)(argc - 3) : 0;
  request.caps = NULL;

  status = lainir_run(&request, &result, &error);
  free(args);
  lainir_free_subroutines(module);

  if (status != LAINIR_RUN_OK) {
    fprintf(stderr, "lainir interpreter error: %s\n", error ? error : "unknown error");
    return 1;
  }

  switch (result.kind) {
  case LAINIR_VALUE_UNIT:
    printf("unit\n");
    return 0;
  case LAINIR_VALUE_BITS:
    printf("%llu\n", (unsigned long long)result.as.bits);
    return 0;
  case LAINIR_VALUE_STRING:
    printf("%s\n", result.as.string ? result.as.string : "");
    return 0;
  case LAINIR_VALUE_ADDR:
    printf("%p\n", result.as.addr);
    return 0;
  case LAINIR_VALUE_FUNC:
    printf("<func %s>\n", result.as.func ? result.as.func->name : "?");
    return 0;
  default:
    return 1;
  }
}
