#include "lainir/artifact.h"
#include "lainvm/execute.h"
#include "lainir/parse.h"
#include "lainir/verify.h"
#include "host_io.h"
#include "bootstrap_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
/* Declare the two allocator entry points locally.  Including windows.h here
 * collides with the LAIN-IR core's EXPR_EVAL enum on current Windows SDKs. */
#define MEM_COMMIT 0x00001000UL
#define MEM_RESERVE 0x00002000UL
#define MEM_RELEASE 0x00008000UL
#define PAGE_READWRITE 0x04UL
extern __declspec(dllimport) void *__stdcall VirtualAlloc(
    void *, size_t, unsigned long, unsigned long);
extern __declspec(dllimport) int __stdcall VirtualFree(
    void *, size_t, unsigned long);
#endif

/* seed_run: interpreter with the minimal allocator needed by generated
 * self-hosting compiler artifacts.  Other host capabilities remain opt-in;
 * this keeps ordinary programs deterministic while allowing archive products
 * to execute their in-process data-model allocations. */
typedef struct {
  struct {
    void *pointer;
    int virtual_alloc;
    size_t size;
  } *blocks;
  size_t count;
  size_t capacity;
  uint64_t bytes;
  uint64_t max_bytes;
  int trace_allocations;
} SeedAllocator;

/* Optional artifact sink for programs that exercise the compiler API from
 * inside `lainir-seed run`.  Ordinary runs do not get this capability; a
 * caller opts in by setting LAINIR_RUN_ARTIFACT to an output path.  This
 * keeps the seed interpreter small while allowing an API result to be fed
 * back into the parser/verifier in a second process. */
typedef struct {
  const char *path;
  FILE *file;
} SeedArtifact;

static LainirRunStatus seed_artifact_begin(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  SeedArtifact *artifact = user_data;
  (void)args;
  if (count != 0 || !artifact || artifact->file) {
    if (error) *error = "bootstrap.artifact-begin has invalid state";
    return LAINIR_RUN_BAD_CALL;
  }
  artifact->file = fopen(artifact->path, "wb");
  if (!artifact->file) {
    if (error) *error = "bootstrap.artifact-begin could not open output";
    return LAINIR_RUN_BAD_CALL;
  }
  *result = lainir_value_unit();
  return LAINIR_RUN_OK;
}

static LainirRunStatus seed_artifact_write_byte(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  SeedArtifact *artifact = user_data;
  if (count != 1 || args[0].kind != LAINIR_VALUE_BITS ||
      !artifact || !artifact->file) {
    if (error) *error = "bootstrap.artifact-write-byte has invalid arguments or state";
    return LAINIR_RUN_BAD_CALL;
  }
  if (fputc((int)(args[0].as.bits & 255u), artifact->file) == EOF) {
    if (error) *error = "bootstrap.artifact-write-byte failed";
    return LAINIR_RUN_BAD_CALL;
  }
  *result = lainir_value_unit();
  return LAINIR_RUN_OK;
}

static LainirRunStatus seed_artifact_finish(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  SeedArtifact *artifact = user_data;
  (void)args;
  if (count != 0 || !artifact || !artifact->file) {
    if (error) *error = "bootstrap.artifact-finish has invalid state";
    return LAINIR_RUN_BAD_CALL;
  }
  if (fclose(artifact->file) != 0) {
    artifact->file = NULL;
    if (error) *error = "bootstrap.artifact-finish failed";
    return LAINIR_RUN_BAD_CALL;
  }
  artifact->file = NULL;
  *result = lainir_value_unit();
  return LAINIR_RUN_OK;
}

static void seed_allocator_release(SeedAllocator *allocator) {
  size_t index;
  if (!allocator) return;
  for (index = 0; index < allocator->count; ++index) {
#ifdef _WIN32
    if (allocator->blocks[index].virtual_alloc)
      VirtualFree(allocator->blocks[index].pointer, 0, MEM_RELEASE);
    else
      free(allocator->blocks[index].pointer);
#else
    free(allocator->blocks[index].pointer);
#endif
  }
  free(allocator->blocks);
  allocator->blocks = NULL;
  allocator->count = 0;
  allocator->capacity = 0;
  allocator->bytes = 0;
}

static LainirRunStatus seed_allocate_pages(
    const LainirValue *args,
    uint32_t arg_count,
    LainirValue *result_out,
    const char **error_out,
    void *user_data) {
  SeedAllocator *allocator = user_data;
  if (arg_count != 1 || args[0].kind != LAINIR_VALUE_BITS) {
    if (error_out) *error_out = "bootstrap.allocate-pages expects one integer";
    return LAINIR_RUN_BAD_CALL;
  }
  size_t size = (size_t)args[0].as.bits;
  int virtual_alloc = 0;
  if (allocator->max_bytes &&
      (uint64_t)size > allocator->max_bytes - allocator->bytes) {
    if (error_out) *error_out = "bootstrap.allocate-pages exceeded run allocation budget";
    return LAINIR_RUN_TRAP;
  }
  void *memory;
#ifdef _WIN32
  if (size >= 65536) {
    memory = VirtualAlloc(NULL, size ? size : 1,
                          MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    virtual_alloc = 1;
  } else {
    memory = calloc(1, size ? size : 1);
  }
#else
  memory = calloc(1, size ? size : 1);
#endif
  if (!memory) {
    if (error_out) *error_out = "bootstrap.allocate-pages failed";
    return LAINIR_RUN_TRAP;
  }
  if (allocator->count == allocator->capacity) {
    size_t next_capacity = allocator->capacity ? allocator->capacity * 2 : 64;
    void *next = realloc(allocator->blocks,
                         next_capacity * sizeof(*allocator->blocks));
    if (!next) {
#ifdef _WIN32
      if (virtual_alloc)
        VirtualFree(memory, 0, MEM_RELEASE);
      else
        free(memory);
#else
      free(memory);
#endif
      if (error_out) *error_out = "bootstrap.allocate-pages tracking failed";
      return LAINIR_RUN_TRAP;
    }
    allocator->blocks = next;
    allocator->capacity = next_capacity;
  }
  allocator->blocks[allocator->count].pointer = memory;
  allocator->blocks[allocator->count].virtual_alloc = virtual_alloc;
  allocator->blocks[allocator->count].size = size;
  allocator->count++;
  allocator->bytes += (uint64_t)size;
  if (allocator->trace_allocations &&
      (allocator->count <= 16 || (allocator->count % 1000) == 0)) {
    fprintf(stderr, "seed alloc count=%zu size=%zu total=%llu\n",
            allocator->count, size, (unsigned long long)allocator->bytes);
  }
  *result_out = lainir_value_addr(memory);
  return LAINIR_RUN_OK;
}

static LainirRunStatus seed_release_pages(
    const LainirValue *args,
    uint32_t arg_count,
    LainirValue *result_out,
    const char **error_out,
    void *user_data) {
  SeedAllocator *allocator = user_data;
  if (arg_count != 1 || args[0].kind != LAINIR_VALUE_ADDR) {
    if (error_out) *error_out = "bootstrap.release-pages expects one address";
    return LAINIR_RUN_BAD_CALL;
  }
  for (size_t index = 0; index < allocator->count; ++index) {
    if (allocator->blocks[index].pointer == args[0].as.addr) {
#ifdef _WIN32
      if (allocator->blocks[index].virtual_alloc) {
        if (!VirtualFree(args[0].as.addr, 0, MEM_RELEASE)) {
          if (error_out) *error_out = "bootstrap.release-pages failed";
          return LAINIR_RUN_BAD_CALL;
        }
      } else {
        free(args[0].as.addr);
      }
#else
      free(args[0].as.addr);
#endif
      /* Keep the tracking table compact.  Swapping with the final entry also
       * makes a second release reliably report an unknown address. */
      allocator->bytes -= (uint64_t)allocator->blocks[index].size;
      allocator->blocks[index] = allocator->blocks[--allocator->count];
      *result_out = lainir_value_unit();
      return LAINIR_RUN_OK;
    }
  }
  if (error_out) *error_out = "bootstrap.release-pages received an unknown address";
  return LAINIR_RUN_BAD_CALL;
}
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

static int seed_run(int argc, char **argv) {
  const char *entry;
  int input_index = 1;
  uint64_t max_steps = 0;
  uint64_t max_call_depth = 0;
  uint64_t max_alloc_bytes = 0;
  unsigned char *src;
  size_t src_length;
  L1Subroutine *module;
  L1Builder *builder = NULL;
  LainirValue *args = NULL;
  LainirValue result = lainir_value_unit();
  LainirRunRequest request = {0};
  const char *error = NULL;
  LainirRunStatus status;
  LainirCapabilityTable *caps = NULL;
  SeedAllocator allocator = {0};
  SeedArtifact artifact = {0};
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
    fprintf(stderr, "usage: lainir-seed run [--max-steps N] [--max-call-depth N] [--max-alloc-bytes N] <input.l1> <entry> [arg ...]\n");
    return 1;
  }

  src = lainir_host_read_file(argv[input_index], &src_length);
  if (!src)
    return 1;
  builder = lainir_builder_new(NULL);
  if (!builder)
    return 1;
  if (!lainir_parse_module_checked(builder, (const char *)src, &module,
                                   &diagnostic)) {
    fprintf(stderr, "lainir parse error [%d] line %d: %s\n", diagnostic.code,
            diagnostic.line, diagnostic.message);
    lainir_builder_free(builder);
    free(src);
    return 1;
  }
  free(src);

  if (!lainir_verify_module(module, argv[input_index + 1], &diagnostic)) {
    fprintf(stderr, "lainir verify error [%d]: %s\n", diagnostic.code,
            diagnostic.message);
    lainir_builder_free(builder);
    return 1;
  }

  if (argc > input_index + 2) {
    args = calloc((size_t)(argc - input_index - 2), sizeof(LainirValue));
    if (!args) {
      lainir_builder_free(builder);
      return 1;
    }
    for (int i = input_index + 2; i < argc; i++) {
      if (!parse_arg_value(argv[i], &args[i - input_index - 2])) {
        fprintf(stderr, "invalid integer argument: %s\n", argv[i]);
        free(args);
        lainir_builder_free(builder);
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
  caps = lainir_caps_new();
  artifact.path = getenv("LAINIR_RUN_ARTIFACT");
  allocator.max_bytes = max_alloc_bytes;
  allocator.trace_allocations = getenv("LAINIR_TRACE_ALLOC") &&
      getenv("LAINIR_TRACE_ALLOC")[0] == '1';
  if (!caps || !lainir_caps_add(caps, "bootstrap.allocate-pages",
                                seed_allocate_pages, &allocator) ||
      !lainir_caps_add(caps, "bootstrap.release-pages",
                       seed_release_pages, &allocator)) {
    seed_allocator_release(&allocator);
    lainir_caps_free(caps);
    free(args);
    lainir_builder_free(builder);
    return 1;
  }
  if (artifact.path && *artifact.path &&
      (!lainir_caps_add(caps, "bootstrap.artifact-begin",
                        seed_artifact_begin, &artifact) ||
       !lainir_caps_add(caps, "bootstrap.artifact-write-byte",
                        seed_artifact_write_byte, &artifact) ||
       !lainir_caps_add(caps, "bootstrap.artifact-finish",
                        seed_artifact_finish, &artifact))) {
    seed_allocator_release(&allocator);
    lainir_caps_free(caps);
    free(args);
    lainir_builder_free(builder);
    return 1;
  }
  if (max_steps || max_call_depth || max_alloc_bytes) {
    lainir_caps_set_limits(caps, max_steps, (uint32_t)max_call_depth,
                           max_alloc_bytes);
  }
  request.caps = caps;

  status = lainir_run(&request, &result, &error);
  if (artifact.file) {
    fclose(artifact.file);
    artifact.file = NULL;
  }
  lainir_caps_free(caps);
  free(args);
  lainir_builder_free(builder);

  if (status != LAINIR_RUN_OK) {
    seed_allocator_release(&allocator);
    fprintf(stderr, "lainir interpreter error: %s\n", error ? error : "unknown error");
    return 1;
  }

  int exit_code = 0;
  switch (result.kind) {
  case LAINIR_VALUE_UNIT:
    printf("unit\n");
    break;
  case LAINIR_VALUE_BITS:
    printf("%llu\n", (unsigned long long)result.as.bits);
    break;
  case LAINIR_VALUE_STRING:
    printf("%s\n", result.as.string ? result.as.string : "");
    break;
  case LAINIR_VALUE_ADDR:
    printf("%p\n", result.as.addr);
    break;
  case LAINIR_VALUE_FUNC:
    printf("<func %s>\n", result.as.func ? result.as.func->name : "?");
    break;
  default:
    exit_code = 1;
    break;
  }
  seed_allocator_release(&allocator);
  return exit_code;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: lainir-seed <interpreter|run> [args...]\n");
    fprintf(stderr, "  interpreter <compiler.l1> <entry> <output.l1> <source...>\n");
    fprintf(stderr, "  run         <input.l1> <entry> [arg ...]\n");
    return 1;
  }
  if (strcmp(argv[1], "run") == 0) {
    return seed_run(argc - 1, argv + 1);
  }
  if (strcmp(argv[1], "interpreter") == 0) {
    return bootstrap_run_cli(argc - 1, argv + 1);
  }
  /* Historical default: interpret argv[1] as the compiler artifact path
   * (previous l1bootstrap shape), for callers that do not pass a
   * subcommand yet. */
  return bootstrap_run_cli(argc, argv);
}
