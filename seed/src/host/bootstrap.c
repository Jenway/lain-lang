#include "lainir/interpreter.h"
#include "lainir/parse.h"
#include "lainir/verify.h"
#include "bootstrap_host.h"
#include "host_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
/* Keep the host header independent from windows.h: the latter defines a
 * typedef named EXPR_EVAL, which collides with the interpreter's expression
 * enum. */
#define MEM_COMMIT 0x1000UL
#define MEM_RESERVE 0x2000UL
#define MEM_RELEASE 0x8000UL
#define PAGE_READWRITE 0x04UL
__declspec(dllimport) void *__stdcall VirtualAlloc(
    void *, size_t, unsigned long, unsigned long);
__declspec(dllimport) int __stdcall VirtualFree(
    void *, size_t, unsigned long);
#endif

typedef struct {
  const char *path;
  unsigned char *bytes;
  size_t length;
} BootstrapSource;

typedef struct {
  void *pointer;
  int virtual_alloc;
  size_t size;
} BootstrapAllocation;

typedef struct BootstrapArena BootstrapArena;

/* Every small arena allocation carries a header immediately before the
 * returned address.  The header lets bootstrap.release-pages validate and
 * retire an individual object without handing an interior pointer to free().
 * Arenas are reclaimed as soon as their last object is released. */
typedef struct BootstrapArenaObject {
  uint32_t magic;
  uint32_t released;
  BootstrapArena *arena;
  size_t size;
} BootstrapArenaObject;

#define BOOTSTRAP_ARENA_OBJECT_MAGIC 0x4c414f42u /* "LAOB" */

/* The compiler's physical IR builder asks for millions of tiny objects
 * (mostly 8/16 byte records).  Giving every one of those objects to the CRT
 * heap makes Windows retain gigabytes of virtual address space even though
 * the live data is only a few dozen MiB.  Small allocations are therefore
 * carved out of zeroed pages owned by this context. */
struct BootstrapArena {
  unsigned char *memory;
  size_t capacity;
  size_t used;
  size_t live_objects;
  int virtual_alloc;
  BootstrapArena *next;
};

#define BOOTSTRAP_SMALL_ALLOC_LIMIT 4096u
#define BOOTSTRAP_ARENA_BYTES (1024u * 1024u)

typedef struct {
  BootstrapSource *sources;
  size_t source_count;
  const char *artifact_path;
  FILE *artifact;
  const char *error;
  BootstrapAllocation *allocated_pages;
  size_t allocated_page_count;
  size_t allocated_page_capacity;
  BootstrapArena *small_arenas;
  uint64_t allocation_count;
  uint64_t arena_bytes;
  uint64_t diagnostic_count;
  uint64_t allocated_page_bytes;
  uint64_t max_allocated_page_bytes;
  int trace_allocations;
} BootstrapContext;

static void bootstrap_release_pages(BootstrapContext *context) {
  size_t index;
  if (!context) return;
  if (context->trace_allocations || getenv("LAINIR_TRACE_PROFILE")) {
    fprintf(stderr,
            "bootstrap summary allocations=%llu logical=%llu arena=%llu large=%zu diagnostics=%llu\n",
            (unsigned long long)context->allocation_count,
            (unsigned long long)context->allocated_page_bytes,
            (unsigned long long)context->arena_bytes,
            context->allocated_page_count,
            (unsigned long long)context->diagnostic_count);
  }
  for (index = 0; index < context->allocated_page_count; ++index) {
#ifdef _WIN32
    if (context->allocated_pages[index].virtual_alloc)
      VirtualFree(context->allocated_pages[index].pointer, 0, MEM_RELEASE);
    else
      free(context->allocated_pages[index].pointer);
#else
    free(context->allocated_pages[index].pointer);
#endif
  }
  free(context->allocated_pages);
  context->allocated_pages = NULL;
  context->allocated_page_count = 0;
  context->allocated_page_capacity = 0;
  while (context->small_arenas) {
    BootstrapArena *arena = context->small_arenas;
#ifdef _WIN32
    if (arena->virtual_alloc)
      VirtualFree(arena->memory, 0, MEM_RELEASE);
    else
      free(arena->memory);
#else
    free(arena->memory);
#endif
    context->small_arenas = arena->next;
    free(arena);
  }
  context->allocated_page_bytes = 0;
  context->arena_bytes = 0;
}

static int bootstrap_arena_contains(
    const BootstrapContext *context, const void *pointer) {
  const BootstrapArena *arena;
  uintptr_t address = (uintptr_t)pointer;
  if (!context || !pointer) return 0;
  for (arena = context->small_arenas; arena; arena = arena->next) {
    uintptr_t start = (uintptr_t)arena->memory;
    if (address >= start && address - start < arena->used)
      return 1;
  }
  return 0;
}

static int bootstrap_arena_release_object(
    BootstrapContext *context, void *pointer) {
  BootstrapArena *arena;
  BootstrapArena **slot;
  BootstrapArenaObject *object;
  uintptr_t address;
  uintptr_t start;
  if (!context || !pointer) return 0;
  if (!bootstrap_arena_contains(context, pointer)) return 0;
  address = (uintptr_t)pointer;
  for (arena = context->small_arenas; arena; arena = arena->next) {
    start = (uintptr_t)arena->memory;
    if (address >= start && address - start < arena->used) break;
  }
  if (!arena || address - start < sizeof(BootstrapArenaObject)) return -1;
  object = ((BootstrapArenaObject *)pointer) - 1;
  if (object->magic != BOOTSTRAP_ARENA_OBJECT_MAGIC ||
      object->arena != arena || object->arena->live_objects == 0)
    return -1;
  if (object->released) return -1;
  arena = object->arena;
  object->released = 1;
  arena->live_objects--;
  if (context->allocated_page_bytes >= (uint64_t)object->size)
    context->allocated_page_bytes -= (uint64_t)object->size;
  else
    context->allocated_page_bytes = 0;
  if (arena->live_objects != 0) return 1;

  /* No object in this arena remains live, so unlink and reclaim its backing
   * page.  The arena header itself is separate from that page. */
  slot = &context->small_arenas;
  while (*slot && *slot != arena) slot = &(*slot)->next;
  if (*slot == arena) *slot = arena->next;
#ifdef _WIN32
  if (arena->virtual_alloc)
    VirtualFree(arena->memory, 0, MEM_RELEASE);
  else
    free(arena->memory);
#else
  free(arena->memory);
#endif
  if (context->arena_bytes >= (uint64_t)arena->capacity)
    context->arena_bytes -= (uint64_t)arena->capacity;
  else
    context->arena_bytes = 0;
  free(arena);
  return 1;
}

static void *bootstrap_arena_allocate(
    BootstrapContext *context, size_t size) {
  BootstrapArena *arena;
  size_t aligned = (size + 7u) & ~(size_t)7u;
  size_t required = sizeof(BootstrapArenaObject) + aligned;
  if (!aligned) aligned = 8;
  required = sizeof(BootstrapArenaObject) + aligned;
  for (arena = context->small_arenas; arena; arena = arena->next) {
    if (required <= arena->capacity - arena->used) {
      BootstrapArenaObject *object =
          (BootstrapArenaObject *)(arena->memory + arena->used);
      arena->used += required;
      object->magic = BOOTSTRAP_ARENA_OBJECT_MAGIC;
      object->released = 0;
      object->arena = arena;
      object->size = size;
      memset(object + 1, 0, aligned);
      arena->live_objects++;
      return object + 1;
    }
  }
  arena = (BootstrapArena *)calloc(1, sizeof(*arena));
  if (!arena) return NULL;
  arena->capacity = required > BOOTSTRAP_ARENA_BYTES
      ? required : BOOTSTRAP_ARENA_BYTES;
#ifdef _WIN32
  if (arena->capacity >= 65536) {
    arena->memory = (unsigned char *)VirtualAlloc(
        NULL, arena->capacity, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    arena->virtual_alloc = 1;
  } else {
    arena->memory = (unsigned char *)calloc(arena->capacity, 1);
  }
#else
  arena->memory = (unsigned char *)calloc(arena->capacity, 1);
#endif
  if (!arena->memory) {
    free(arena);
    return NULL;
  }
  arena->next = context->small_arenas;
  context->small_arenas = arena;
  context->arena_bytes += (uint64_t)arena->capacity;
  BootstrapArenaObject *object = (BootstrapArenaObject *)arena->memory;
  arena->used = required;
  object->magic = BOOTSTRAP_ARENA_OBJECT_MAGIC;
  object->released = 0;
  object->arena = arena;
  object->size = size;
  memset(object + 1, 0, aligned);
  arena->live_objects = 1;
  return object + 1;
}

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

static LainirRunStatus copy_bytes(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  (void)user_data;
  if (count != 3 || args[0].kind != LAINIR_VALUE_ADDR ||
      args[1].kind != LAINIR_VALUE_ADDR || args[2].kind != LAINIR_VALUE_BITS) {
    *error = "bootstrap.copy-bytes expects destination, source and length";
    return LAINIR_RUN_BAD_CALL;
  }
  if (args[2].as.bits)
    memcpy(args[0].as.addr, args[1].as.addr, (size_t)args[2].as.bits);
  *result = lainir_value_unit();
  return LAINIR_RUN_OK;
}

static LainirRunStatus allocate_pages(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  BootstrapContext *context = user_data;
  size_t size;
  void *memory;
  int virtual_alloc = 0;
  if (count != 1 || args[0].kind != LAINIR_VALUE_BITS) {
    *error = "bootstrap.allocate-pages expects one integer size";
    return LAINIR_RUN_BAD_CALL;
  }
  size = (size_t)args[0].as.bits;
  if (context->max_allocated_page_bytes &&
      (context->allocated_page_bytes > context->max_allocated_page_bytes ||
       (uint64_t)size > context->max_allocated_page_bytes -
           context->allocated_page_bytes)) {
    *error = "bootstrap.allocate-pages exceeded host allocation budget";
    return LAINIR_RUN_TRAP;
  }
  context->allocation_count++;

  if (size <= BOOTSTRAP_SMALL_ALLOC_LIMIT) {
    memory = bootstrap_arena_allocate(context, size);
    if (!memory) {
      *error = "bootstrap.allocate-pages arena allocation failed";
      return LAINIR_RUN_TRAP;
    }
    context->allocated_page_bytes += (uint64_t)size;
    if (context->trace_allocations &&
        (context->allocation_count <= 16 ||
         (context->allocation_count % 100000) == 0)) {
      fprintf(stderr, "bootstrap alloc count=%llu size=%zu total=%llu (arena)\n",
              (unsigned long long)context->allocation_count, size,
              (unsigned long long)context->allocated_page_bytes);
    }
    *result = lainir_value_addr(memory);
    return LAINIR_RUN_OK;
  }
  /* VirtualAlloc returns demand-zero committed pages on Windows.  Unlike
   * calloc, it does not touch every byte up front, so the many 64 KiB/1 MiB
   * compiler buffers only consume physical memory for pages the compiler
   * actually uses. */
#ifdef _WIN32
  if (size >= 65536) {
    memory = VirtualAlloc(NULL, size ? size : 1,
                          MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    virtual_alloc = 1;
  } else {
    memory = calloc(size ? size : 1, 1);
  }
#else
  memory = calloc(size ? size : 1, 1);
#endif
  if (!memory) {
    *error = "bootstrap.allocate-pages failed";
    return LAINIR_RUN_TRAP;
  }
  if (context->allocated_page_count == context->allocated_page_capacity) {
    size_t next_capacity = context->allocated_page_capacity
        ? context->allocated_page_capacity * 2 : 256;
    BootstrapAllocation *next = realloc(
        context->allocated_pages,
        next_capacity * sizeof(BootstrapAllocation));
    if (!next) {
#ifdef _WIN32
      if (virtual_alloc)
        VirtualFree(memory, 0, MEM_RELEASE);
      else
        free(memory);
#else
      free(memory);
#endif
      *error = "bootstrap.allocate-pages tracking failed";
      return LAINIR_RUN_TRAP;
    }
    context->allocated_pages = next;
    context->allocated_page_capacity = next_capacity;
  }
  context->allocated_pages[context->allocated_page_count++] =
      (BootstrapAllocation){memory, virtual_alloc, size};
  context->allocated_page_bytes += (uint64_t)size;
  if (context->trace_allocations &&
      (context->allocation_count <= 16 ||
       (context->allocation_count % 100000) == 0 || size >= 65536)) {
    fprintf(stderr, "bootstrap alloc count=%llu size=%zu total=%llu\n",
            (unsigned long long)context->allocation_count, size,
            (unsigned long long)context->allocated_page_bytes);
  }
  *result = lainir_value_addr(memory);
  return LAINIR_RUN_OK;
}

static LainirRunStatus release_pages(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  BootstrapContext *context = user_data;
  size_t index;
  if (count != 1 || args[0].kind != LAINIR_VALUE_ADDR) {
    *error = "bootstrap.release-pages expects one address";
    return LAINIR_RUN_BAD_CALL;
  }
  for (index = 0; index < context->allocated_page_count; ++index) {
    if (context->allocated_pages[index].pointer == args[0].as.addr) {
      int virtual_alloc = context->allocated_pages[index].virtual_alloc;
      size_t released_size = context->allocated_pages[index].size;
#ifdef _WIN32
      if (virtual_alloc) {
        if (!VirtualFree(args[0].as.addr, 0, MEM_RELEASE)) {
          *error = "bootstrap.release-pages failed";
          return LAINIR_RUN_BAD_CALL;
        }
      } else {
        free(args[0].as.addr);
      }
#else
      (void)virtual_alloc;
      free(args[0].as.addr);
#endif
      context->allocated_pages[index] =
          context->allocated_pages[--context->allocated_page_count];
      context->allocated_page_bytes -= (uint64_t)released_size;
      *result = lainir_value_unit();
      return LAINIR_RUN_OK;
    }
  }
  /* Arena-owned objects are released through their in-band header.  The
   * backing page is reclaimed when its last object is gone; this keeps the
   * capability safe without passing an interior pointer to the CRT allocator. */
  {
    int arena_release = bootstrap_arena_release_object(
        context, args[0].as.addr);
    if (arena_release > 0) {
      *result = lainir_value_unit();
      return LAINIR_RUN_OK;
    }
    if (arena_release < 0) {
      *error = "bootstrap.release-pages received a stale arena address";
      return LAINIR_RUN_BAD_CALL;
    }
  }
  if (bootstrap_arena_contains(context, args[0].as.addr)) {
    *result = lainir_value_unit();
    return LAINIR_RUN_OK;
  }
  *error = "bootstrap.release-pages received an unknown address";
  return LAINIR_RUN_BAD_CALL;
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
  BootstrapContext *context = user_data;
  const void *bytes;
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
  context->diagnostic_count++;
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

static LainirRunStatus artifact_write_span(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  BootstrapContext *context = user_data;
  size_t length;
  if (count != 2 || args[0].kind != LAINIR_VALUE_ADDR ||
      args[1].kind != LAINIR_VALUE_BITS || !context->artifact) {
    *error = "bootstrap.artifact-write-span has invalid arguments or state";
    return LAINIR_RUN_BAD_CALL;
  }
  length = (size_t)args[1].as.bits;
  if (length && fwrite(args[0].as.addr, 1, length, context->artifact) != length) {
    *error = "bootstrap.artifact-write-span failed";
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
            "usage: lainir-seed interpreter <compiler.l1> <entry> <output.l1> "
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
      !add_capability(caps, "bootstrap.copy-bytes", copy_bytes, &context) ||
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
      !add_capability(caps, "bootstrap.artifact-write-span",
                      artifact_write_span, &context) ||
      !add_capability(caps, "bootstrap.artifact-write-literal",
                      artifact_write_literal, &context) ||
      !add_capability(caps, "bootstrap.artifact-finish", artifact_finish,
                      &context)) {
    fprintf(stderr, "could not initialize bootstrap capabilities\n");
    goto cleanup;
  }

  /* Keep long archive-library runs diagnosable without changing the normal
   * bootstrap contract.  A temporary step cap can be supplied by the host
   * while bisecting a multi-source lowering loop. */
  const char *step_limit_text = getenv("LAINIR_BOOTSTRAP_MAX_STEPS");
  if (step_limit_text && *step_limit_text) {
    char *step_limit_end = NULL;
    unsigned long long step_limit = strtoull(step_limit_text, &step_limit_end, 10);
    if (step_limit_end != step_limit_text && *step_limit_end == '\0' && step_limit > 0)
      lainir_caps_set_limits(caps, (uint64_t)step_limit, 256, 0);
  }
  const char *alloc_limit_text = getenv("LAINIR_BOOTSTRAP_MAX_ALLOC_BYTES");
  if (alloc_limit_text && *alloc_limit_text) {
    char *alloc_limit_end = NULL;
    unsigned long long alloc_limit =
        strtoull(alloc_limit_text, &alloc_limit_end, 10);
    if (alloc_limit_end != alloc_limit_text && *alloc_limit_end == '\0' &&
        alloc_limit > 0) {
      context.max_allocated_page_bytes = (uint64_t)alloc_limit;
      /* Apply the same ceiling to interpreter-owned #alloca memory.  Without
       * this, a malformed self-hosting loop can bypass the host-page budget
       * and still grow the process until the OS kills it. */
      lainir_caps_set_limits(caps, 0, 256, (uint64_t)alloc_limit);
    }
  }
  context.trace_allocations = getenv("LAINIR_TRACE_ALLOC") &&
      getenv("LAINIR_TRACE_ALLOC")[0] == '1';

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
  bootstrap_release_pages(&context);
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
