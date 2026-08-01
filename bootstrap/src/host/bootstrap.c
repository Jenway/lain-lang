#include "lainir/interpreter.h"
#include "lainir/parse.h"
#include "lainir/verify.h"
#include "bootstrap_host.h"
#include "host_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const char *path;
  unsigned char *bytes;
  size_t length;
} BootstrapSource;

typedef struct {
  BootstrapSource *sources;
  size_t source_count;
  const char *artifact_path;
  FILE *artifact;
  const char *error;
} BootstrapContext;

static unsigned char *read_file_bytes(const char *path, size_t *length_out) {
  return lainir_host_read_file(path, length_out);
}

static int value_index(
    const LainirValue *args, uint32_t count, size_t limit, size_t *out) {
  if (count != 1 || args[0].kind != LAINIR_VALUE_BITS)
    return 0;
  *out = (size_t)args[0].as.bits;
  return *out < limit;
}

static LainirRunStatus source_count(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  BootstrapContext *context = user_data;
  (void)args;
  if (count != 0) {
    *error = "bootstrap.source-count expects no arguments";
    return LAINIR_RUN_BAD_CALL;
  }
  *result = lainir_value_bits(context->source_count, 64);
  return LAINIR_RUN_OK;
}

static LainirRunStatus source_path_data(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  BootstrapContext *context = user_data;
  size_t index;
  if (!value_index(args, count, context->source_count, &index)) {
    *error = "bootstrap.source-path-data received an invalid source index";
    return LAINIR_RUN_BAD_CALL;
  }
  *result = lainir_value_addr((void *)context->sources[index].path);
  return LAINIR_RUN_OK;
}

static LainirRunStatus source_path_length(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  BootstrapContext *context = user_data;
  size_t index;
  if (!value_index(args, count, context->source_count, &index)) {
    *error = "bootstrap.source-path-length received an invalid source index";
    return LAINIR_RUN_BAD_CALL;
  }
  *result = lainir_value_bits(strlen(context->sources[index].path), 64);
  return LAINIR_RUN_OK;
}

static LainirRunStatus source_data(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  BootstrapContext *context = user_data;
  size_t index;
  if (!value_index(args, count, context->source_count, &index)) {
    *error = "bootstrap.source-data received an invalid source index";
    return LAINIR_RUN_BAD_CALL;
  }
  *result = lainir_value_addr(context->sources[index].bytes);
  return LAINIR_RUN_OK;
}

static LainirRunStatus source_length(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  BootstrapContext *context = user_data;
  size_t index;
  if (!value_index(args, count, context->source_count, &index)) {
    *error = "bootstrap.source-length received an invalid source index";
    return LAINIR_RUN_BAD_CALL;
  }
  *result = lainir_value_bits(context->sources[index].length, 64);
  return LAINIR_RUN_OK;
}

static LainirRunStatus allocate_pages(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  size_t size;
  void *memory;
  (void)user_data;
  if (count != 1 || args[0].kind != LAINIR_VALUE_BITS) {
    *error = "bootstrap.allocate-pages expects one integer size";
    return LAINIR_RUN_BAD_CALL;
  }
  size = (size_t)args[0].as.bits;
  memory = calloc(size ? size : 1, 1);
  if (!memory) {
    *error = "bootstrap.allocate-pages failed";
    return LAINIR_RUN_TRAP;
  }
  *result = lainir_value_addr(memory);
  return LAINIR_RUN_OK;
}

static LainirRunStatus release_pages(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  (void)user_data;
  if (count != 1 || args[0].kind != LAINIR_VALUE_ADDR) {
    *error = "bootstrap.release-pages expects one address";
    return LAINIR_RUN_BAD_CALL;
  }
  free(args[0].as.addr);
  *result = lainir_value_unit();
  return LAINIR_RUN_OK;
}

static LainirRunStatus write_bytes(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  BootstrapContext *context = user_data;
  FILE *file;
  size_t length;
  if (count != 2 || args[0].kind != LAINIR_VALUE_ADDR ||
      args[1].kind != LAINIR_VALUE_BITS) {
    *error = "bootstrap.write-artifact expects address and length";
    return LAINIR_RUN_BAD_CALL;
  }
  length = (size_t)args[1].as.bits;
  file = fopen(context->artifact_path, "wb");
  if (!file) {
    *error = "bootstrap.write-artifact could not open output";
    return LAINIR_RUN_BAD_CALL;
  }
  if (fwrite(args[0].as.addr, 1, length, file) != length) {
    fclose(file);
    *error = "bootstrap.write-artifact could not write output";
    return LAINIR_RUN_BAD_CALL;
  }
  fclose(file);
  *result = lainir_value_unit();
  return LAINIR_RUN_OK;
}

static LainirRunStatus write_diagnostic(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  const void *bytes;
  (void)user_data;
  if (count != 2 ||
      (args[0].kind != LAINIR_VALUE_ADDR &&
       args[0].kind != LAINIR_VALUE_STRING) ||
      args[1].kind != LAINIR_VALUE_BITS) {
    *error = "bootstrap.write-diagnostic expects address and length";
    return LAINIR_RUN_BAD_CALL;
  }
  bytes = args[0].kind == LAINIR_VALUE_STRING
              ? (const void *)args[0].as.string
              : args[0].as.addr;
  fwrite(bytes, 1, (size_t)args[1].as.bits, stderr);
  fputc('\n', stderr);
  *result = lainir_value_unit();
  return LAINIR_RUN_OK;
}

static LainirRunStatus artifact_begin(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  BootstrapContext *context = user_data;
  (void)args;
  if (count != 0 || context->artifact) {
    *error = "bootstrap.artifact-begin has invalid state";
    return LAINIR_RUN_BAD_CALL;
  }
  context->artifact = fopen(context->artifact_path, "wb");
  if (!context->artifact) {
    *error = "bootstrap.artifact-begin could not open output";
    return LAINIR_RUN_BAD_CALL;
  }
  *result = lainir_value_unit();
  return LAINIR_RUN_OK;
}

static LainirRunStatus artifact_write_byte(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  BootstrapContext *context = user_data;
  if (count != 1 || args[0].kind != LAINIR_VALUE_BITS ||
      !context->artifact) {
    *error = "bootstrap.artifact-write-byte has invalid arguments or state";
    return LAINIR_RUN_BAD_CALL;
  }
  if (fputc((int)(args[0].as.bits & 255), context->artifact) == EOF) {
    *error = "bootstrap.artifact-write-byte failed";
    return LAINIR_RUN_BAD_CALL;
  }
  *result = lainir_value_unit();
  return LAINIR_RUN_OK;
}

static LainirRunStatus artifact_write_literal(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  BootstrapContext *context = user_data;
  const char *text;
  if (count != 1 ||
      (args[0].kind != LAINIR_VALUE_STRING &&
       args[0].kind != LAINIR_VALUE_ADDR) ||
      !context->artifact) {
    *error = "bootstrap.artifact-write-literal has invalid arguments or state";
    return LAINIR_RUN_BAD_CALL;
  }
  text = args[0].kind == LAINIR_VALUE_STRING
             ? args[0].as.string
             : (const char *)args[0].as.addr;
  if (fputs(text ? text : "", context->artifact) == EOF) {
    *error = "bootstrap.artifact-write-literal failed";
    return LAINIR_RUN_BAD_CALL;
  }
  *result = lainir_value_unit();
  return LAINIR_RUN_OK;
}

static LainirRunStatus artifact_finish(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  BootstrapContext *context = user_data;
  (void)args;
  if (count != 0 || !context->artifact) {
    *error = "bootstrap.artifact-finish has invalid state";
    return LAINIR_RUN_BAD_CALL;
  }
  if (fclose(context->artifact) != 0) {
    context->artifact = NULL;
    *error = "bootstrap.artifact-finish failed";
    return LAINIR_RUN_BAD_CALL;
  }
  context->artifact = NULL;
  *result = lainir_value_unit();
  return LAINIR_RUN_OK;
}

static int add_capability(
    LainirCapabilityTable *table, const char *name, LainirHostFn function,
    BootstrapContext *context) {
  return lainir_caps_add(table, name, function, context);
}

int bootstrap_run_cli(int argc, char **argv) {
  BootstrapContext context = {0};
  LainirCapabilityTable *caps = NULL;
  LainirRunRequest request = {0};
  LainirValue result = lainir_value_unit();
  L1Subroutine *module = NULL;
  L1Diagnostic diagnostic = {0};
  unsigned char *compiler_text = NULL;
  size_t compiler_length = 0;
  const char *run_error = NULL;
  const char *compiler_path;
  const char *entry_name;
  const char *artifact_path;
  int source_start;
  int source_arg_count;
  int exit_code = 1;

  if (argc < 5) {
    fprintf(stderr,
            "usage: l1bootstrap <compiler.l1> <entry> <output.l1> "
            "<source> [source ...]\n");
    return 1;
  }
  compiler_path = argv[1];
  entry_name = argv[2];
  artifact_path = argv[3];
  source_start = 4;
  source_arg_count = argc - 4;

  compiler_text = read_file_bytes(compiler_path, &compiler_length);
  if (!compiler_text) {
    fprintf(stderr, "could not read compiler artifact: %s\n", compiler_path);
    goto cleanup;
  }
  if (!lainir_parse_module_checked(
          (const char *)compiler_text, &module, &diagnostic)) {
    fprintf(stderr, "LAIN-IR parse error [%d] line %d: %s\n",
            diagnostic.code, diagnostic.line, diagnostic.message);
    goto cleanup;
  }
  if (!lainir_verify_module(module, entry_name, &diagnostic)) {
    fprintf(stderr, "LAIN-IR verify error [%d]: %s\n",
            diagnostic.code, diagnostic.message);
    goto cleanup;
  }

  context.artifact_path = artifact_path;
  context.source_count = (size_t)source_arg_count;
  context.sources = calloc(context.source_count, sizeof(BootstrapSource));
  if (!context.sources)
    goto cleanup;
  for (size_t index = 0; index < context.source_count; ++index) {
    context.sources[index].path = argv[index + (size_t)source_start];
    context.sources[index].bytes = read_file_bytes(
        argv[index + (size_t)source_start], &context.sources[index].length);
    if (!context.sources[index].bytes) {
      fprintf(stderr, "could not read source: %s\n",
              argv[index + (size_t)source_start]);
      goto cleanup;
    }
  }

  caps = lainir_caps_new();
  if (!caps ||
      !add_capability(caps, "bootstrap.source-count", source_count, &context) ||
      !add_capability(caps, "bootstrap.source-path-data", source_path_data,
                      &context) ||
      !add_capability(caps, "bootstrap.source-path-length", source_path_length,
                      &context) ||
      !add_capability(caps, "bootstrap.source-data", source_data, &context) ||
      !add_capability(caps, "bootstrap.source-length", source_length, &context) ||
      !add_capability(caps, "bootstrap.allocate-pages", allocate_pages,
                      &context) ||
      !add_capability(caps, "bootstrap.release-pages", release_pages,
                      &context) ||
      !add_capability(caps, "bootstrap.write-artifact", write_bytes, &context) ||
      !add_capability(caps, "bootstrap.write-diagnostic", write_diagnostic,
                      &context) ||
      !add_capability(caps, "bootstrap.artifact-begin", artifact_begin,
                      &context) ||
      !add_capability(caps, "bootstrap.artifact-write-byte",
                      artifact_write_byte, &context) ||
      !add_capability(caps, "bootstrap.artifact-write-literal",
                      artifact_write_literal, &context) ||
      !add_capability(caps, "bootstrap.artifact-finish", artifact_finish,
                      &context)) {
    fprintf(stderr, "could not initialize bootstrap capabilities\n");
    goto cleanup;
  }

  request.module = module;
  request.entry_name = entry_name;
  request.caps = caps;
  if (lainir_run(&request, &result, &run_error) != LAINIR_RUN_OK) {
    fprintf(stderr, "bootstrap compiler failed: %s\n",
            run_error ? run_error : "unknown error");
    goto cleanup;
  }
  if (result.kind != LAINIR_VALUE_BITS || result.as.bits != 0) {
    if (result.kind == LAINIR_VALUE_BITS)
      fprintf(stderr, "bootstrap compiler returned status %llu\n",
              (unsigned long long)result.as.bits);
    else
      fprintf(stderr, "bootstrap compiler returned a non-integer status\n");
    goto cleanup;
  }
  exit_code = 0;

cleanup:
  if (context.artifact)
    fclose(context.artifact);
  if (context.sources) {
    for (size_t index = 0; index < context.source_count; ++index)
      free(context.sources[index].bytes);
  }
  free(context.sources);
  lainir_caps_free(caps);
  lainir_free_subroutines(module);
  free(compiler_text);
  return exit_code;
}
