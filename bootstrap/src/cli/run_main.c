#include "lainir/interpreter.h"
#include "lainir/parse.h"
#include "lainir/verify.h"
#include "host_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_arg_value(const char *text, LainirValue *out) {
  char *end = NULL;
  unsigned long long v = strtoull(text, &end, 10);
  if (!text[0] || (end && *end))
    return 0;
  *out = lainir_value_bits((uint64_t)v, 32);
  return 1;
}

static int parse_limit(const char *text, uint64_t *out) {
  char *end = NULL;
  unsigned long long value = strtoull(text, &end, 10);
  if (!text[0] || (end && *end)) return 0;
  *out = (uint64_t)value;
  return 1;
}

int main(int argc, char **argv) {
  const char *entry;
  int input_index = 1;
  uint64_t max_steps = 0;
  uint64_t max_call_depth = 0;
  uint64_t max_alloc_bytes = 0;
  unsigned char *src;
  size_t src_length;
  L1Subroutine *module;
  LainirValue *args = NULL;
  LainirValue result = lainir_value_unit();
  LainirRunRequest request;
  const char *error = NULL;
  LainirRunStatus status;
  LainirCapabilityTable *caps = NULL;
  L1Diagnostic diagnostic;

  while (input_index < argc && argv[input_index][0] == '-') {
    const char *option = argv[input_index++];
    if (strcmp(option, "--max-steps") == 0 && input_index < argc) {
      if (!parse_limit(argv[input_index++], &max_steps)) return 1;
    } else if (strcmp(option, "--max-call-depth") == 0 && input_index < argc) {
      if (!parse_limit(argv[input_index++], &max_call_depth)) return 1;
    } else if (strcmp(option, "--max-alloc-bytes") == 0 && input_index < argc) {
      if (!parse_limit(argv[input_index++], &max_alloc_bytes)) return 1;
    } else {
      fprintf(stderr, "unknown or incomplete option: %s\n", option);
      return 1;
    }
  }
  if (argc < input_index + 2) {
    fprintf(stderr, "usage: lainir-run [--max-steps N] [--max-call-depth N] [--max-alloc-bytes N] <input.l1> <entry> [arg ...]\n");
    return 1;
  }

  src = lainir_host_read_file(argv[input_index], &src_length);
  if (!src)
    return 1;
  if (!lainir_parse_module_checked(
          (const char *)src, &module, &diagnostic)) {
    fprintf(stderr, "lainir parse error [%d] line %d: %s\n", diagnostic.code,
            diagnostic.line, diagnostic.message);
    free(src);
    return 1;
  }
  free(src);

  if (!lainir_verify_module(module, argv[input_index + 1], &diagnostic)) {
    fprintf(stderr, "lainir verify error [%d]: %s\n", diagnostic.code,
            diagnostic.message);
    lainir_free_subroutines(module);
    return 1;
  }

  if (argc > input_index + 2) {
    args = calloc((size_t)(argc - input_index - 2), sizeof(LainirValue));
    if (!args) {
      lainir_free_subroutines(module);
      return 1;
    }
    for (int i = input_index + 2; i < argc; i++) {
      if (!parse_arg_value(argv[i], &args[i - input_index - 2])) {
        fprintf(stderr, "invalid integer argument: %s\n", argv[i]);
        free(args);
        lainir_free_subroutines(module);
        return 1;
      }
    }
  }

  entry = argv[input_index + 1];
  request.module = module;
  request.entry_name = entry;
  request.args = args;
  request.arg_count = argc > input_index + 2 ?
      (uint32_t)(argc - input_index - 2) : 0;
  if (max_steps || max_call_depth || max_alloc_bytes) {
    caps = lainir_caps_new();
    if (!caps) {
      free(args);
      lainir_free_subroutines(module);
      return 1;
    }
    lainir_caps_set_limits(caps, max_steps, (uint32_t)max_call_depth,
                           max_alloc_bytes);
  }
  request.caps = caps;

  status = lainir_run(&request, &result, &error);
  lainir_caps_free(caps);
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
