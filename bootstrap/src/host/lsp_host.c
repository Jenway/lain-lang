#include "lsp_host.h"

#include "lainir/interpreter.h"
#include "lainir/parse.h"
#include "lainir/verify.h"
#include "host_io.h"

#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

static LainirRunStatus lsp_read_byte(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  int byte;
  (void)args;
  (void)user_data;
  if (count != 0) {
    *error = "lsp.read-byte expects no arguments";
    return LAINIR_RUN_BAD_CALL;
  }
  byte = fgetc(stdin);
  *result = lainir_value_bits(byte == EOF ? 256 : (unsigned)byte, 16);
  return LAINIR_RUN_OK;
}

static LainirRunStatus lsp_write_byte(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  static int limit_initialized = 0;
  static size_t write_limit = 0;
  static size_t write_count = 0;
  (void)user_data;
  if (count != 1 || args[0].kind != LAINIR_VALUE_BITS) {
    *error = "lsp.write-byte expects one byte";
    return LAINIR_RUN_BAD_CALL;
  }
  // Optional deterministic write-failure injection for host-boundary tests.
  // The normal environment leaves the capability unchanged.
  if (!limit_initialized) {
    const char *value = getenv("LAINIR_LSP_WRITE_MAX");
    if (value && *value) {
      char *end = NULL;
      unsigned long long parsed = strtoull(value, &end, 10);
      if (end != value && *end == '\0') {
        write_limit = (size_t)parsed;
      }
    }
    limit_initialized = 1;
  }
  if (write_limit != 0 && write_count >= write_limit) {
    *error = "lsp.write-byte fault injection limit exceeded";
    return LAINIR_RUN_TRAP;
  }
  if (fputc((int)(args[0].as.bits & 255u), stdout) == EOF) {
    *error = "lsp.write-byte failed";
    return LAINIR_RUN_BAD_CALL;
  }
  write_count += 1;
  *result = lainir_value_unit();
  return LAINIR_RUN_OK;
}

static LainirRunStatus lsp_flush(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  (void)args;
  (void)user_data;
  if (count != 0) {
    *error = "lsp.flush expects no arguments";
    return LAINIR_RUN_BAD_CALL;
  }
  if (fflush(stdout) != 0) {
    *error = "lsp.flush failed";
    return LAINIR_RUN_BAD_CALL;
  }
  *result = lainir_value_unit();
  return LAINIR_RUN_OK;
}

static LainirRunStatus lsp_allocate(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  static int limit_initialized = 0;
  static size_t allocation_limit = 0;
  size_t size;
  void *memory;
  (void)user_data;
  if (count != 1 || args[0].kind != LAINIR_VALUE_BITS) {
    *error = "lsp.allocate expects one size";
    return LAINIR_RUN_BAD_CALL;
  }
  size = (size_t)args[0].as.bits;
  // Fault injection for host-boundary tests.  This is deliberately confined
  // to the allocator capability; no protocol or editor behavior lives here.
  // A positive limit rejects allocations larger than the configured number
  // of bytes.  The environment is unset in normal operation.
  if (!limit_initialized) {
    const char *value = getenv("LAINIR_LSP_ALLOC_MAX");
    if (value && *value) {
      char *end = NULL;
      unsigned long long parsed = strtoull(value, &end, 10);
      if (end != value && *end == '\0') {
        allocation_limit = (size_t)parsed;
      }
    }
    limit_initialized = 1;
  }
  if (allocation_limit != 0 && size > allocation_limit) {
    *error = "lsp.allocate fault injection limit exceeded";
    return LAINIR_RUN_TRAP;
  }
  memory = calloc(size ? size : 1, 1);
  if (!memory) {
    *error = "lsp.allocate failed";
    return LAINIR_RUN_TRAP;
  }
  *result = lainir_value_addr(memory);
  return LAINIR_RUN_OK;
}

static int add_capability(
    LainirCapabilityTable *caps, const char *name, LainirHostFn fn) {
  return lainir_caps_add(caps, name, fn, NULL);
}

int lsp_run_cli(int argc, char **argv) {
  const char *entry = argc > 2 ? argv[2] : "lsp_run";
  unsigned char *source = NULL;
  size_t source_length = 0;
  L1Subroutine *module = NULL;
  L1Diagnostic diagnostic = {0};
  LainirCapabilityTable *caps = NULL;
  LainirRunRequest request = {0};
  LainirValue result = lainir_value_unit();
  const char *error = NULL;
  LainirRunStatus status;

  if (argc < 2) {
    fprintf(stderr, "usage: lainir-lsp <lsp.l1> [entry]\n");
    return 1;
  }
#ifdef _WIN32
  // LSP framing is byte-exact; CRT text mode would collapse CRLF to LF.
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  source = lainir_host_read_file(argv[1], &source_length);
  if (!source) {
    fprintf(stderr, "could not read LSP module: %s\n", argv[1]);
    return 1;
  }
  if (!lainir_parse_module_checked((const char *)source, &module, &diagnostic)) {
    fprintf(stderr, "LAIN-IR parse error [%d] line %d: %s\n",
            diagnostic.code, diagnostic.line, diagnostic.message);
    goto fail;
  }
  if (!lainir_verify_module(module, entry, &diagnostic)) {
    fprintf(stderr, "LAIN-IR verify error [%d]: %s\n",
            diagnostic.code, diagnostic.message);
    goto fail;
  }
  caps = lainir_caps_new();
  if (!caps || !add_capability(caps, "lsp.read-byte", lsp_read_byte) ||
      !add_capability(caps, "lsp.write-byte", lsp_write_byte) ||
      !add_capability(caps, "lsp.flush", lsp_flush) ||
      !add_capability(caps, "lsp.allocate", lsp_allocate) ||
      // source.l1/highlight.l1 are pure LAIN-IR modules, but their allocator
      // import keeps the generic bootstrap name.  Both names intentionally
      // share this one host callback.
      !add_capability(caps, "bootstrap.allocate-pages", lsp_allocate)) {
    fprintf(stderr, "could not initialize LSP capabilities\n");
    goto fail;
  }
  request.module = module;
  request.entry_name = entry;
  request.caps = caps;
  status = lainir_run(&request, &result, &error);
  if (status != LAINIR_RUN_OK) {
    fprintf(stderr, "LSP LAIN-IR failed: %s\n", error ? error : "unknown error");
    goto fail;
  }
  if (result.kind != LAINIR_VALUE_BITS || result.as.bits != 0) {
    fprintf(stderr, "LSP entry returned a non-zero status\n");
    goto fail;
  }
  lainir_caps_free(caps);
  lainir_free_subroutines(module);
  free(source);
  return 0;

fail:
  lainir_caps_free(caps);
  lainir_free_subroutines(module);
  free(source);
  return 1;
}
