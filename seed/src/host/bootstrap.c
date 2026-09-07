#include "lainir/interpreter.h"
#include "lainir/eval_source.h"
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
  uint64_t *eval_values;
  size_t eval_value_count;
  size_t eval_value_index;
  int trace_allocations;
  LainirCapabilityTable *caps;
  LainirModuleHandle *prepared_module;
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
/* Run the seed parser, verifier and compile-time evaluator on a complete
 * LAIN-IR source buffer.  The compiler front-end uses this as its single
 * #eval execution path; it does not interpret expressions itself. */
static LainirRunStatus eval_source(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  BootstrapContext *context = user_data;
  L1Diagnostic diagnostic = {0};
  size_t length;
  if (count != 2 || args[0].kind != LAINIR_VALUE_ADDR ||
      args[1].kind != LAINIR_VALUE_BITS) {
    *error = "bootstrap.eval-source expects source address and length";
    return LAINIR_RUN_BAD_CALL;
  }
  if (!context) {
    *error = "bootstrap.eval-source has no host context";
    return LAINIR_RUN_BAD_CALL;
  }
  if (!args[0].as.addr) {
    *error = "bootstrap.eval-source received a null source";
    return LAINIR_RUN_BAD_CALL;
  }
  if (args[1].as.bits > SIZE_MAX - 1) {
    *error = "bootstrap.eval-source length is too large";
    return LAINIR_RUN_BAD_CALL;
  }
  length = (size_t)args[1].as.bits;
  free(context->eval_values);
  context->eval_values = NULL;
  context->eval_value_count = 0;
  context->eval_value_index = 0;
  if (context && context->source_count == 1 && context->sources &&
      args[0].as.addr == context->sources[0].bytes &&
      length == context->sources[0].length && context->prepared_module) {
    if (lainir_module_handle_eval_values(
            context->prepared_module, context->caps,
            &context->eval_values, &context->eval_value_count,
            &diagnostic, error) != LAINIR_RUN_OK)
      return LAINIR_RUN_BAD_CALL;
  } else {
    char *source = malloc(length + 1);
    if (!source) {
      *error = "bootstrap.eval-source allocation failed";
      return LAINIR_RUN_TRAP;
    }
    memcpy(source, args[0].as.addr, length);
    source[length] = '\0';
    if (lainir_eval_source_values(source, context ? context->caps : NULL,
                                  &context->eval_values,
                                  &context->eval_value_count,
                                  &diagnostic, error) != LAINIR_RUN_OK) {
      free(source);
      return LAINIR_RUN_BAD_CALL;
    }
    free(source);
  }
  *result = lainir_value_bits(1, 32);
  return LAINIR_RUN_OK;
}

static LainirRunStatus eval_next(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  BootstrapContext *context = user_data;
  (void)error;
  if (count != 0) {
    *error = "bootstrap.eval-next expects no arguments";
    return LAINIR_RUN_BAD_CALL;
  }
  if (!context || context->eval_value_index >= context->eval_value_count) {
    *result = lainir_value_bits(0, 64);
    return LAINIR_RUN_OK;
  }
  *result = lainir_value_bits(
      context->eval_values[context->eval_value_index++], 64);
  return LAINIR_RUN_OK;
}

/* Validate a complete source buffer with the seed parser and verifier.  The
 * result is deliberately only a status: the module remains owned by seed and
 * is released before this capability returns. */
static LainirRunStatus validate_source(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  BootstrapContext *context = user_data;
  L1Subroutine *module = NULL;
  L1Diagnostic diagnostic = {0};
  char *copy;
  size_t length;
  if (count != 2 || args[0].kind != LAINIR_VALUE_ADDR ||
      args[1].kind != LAINIR_VALUE_BITS) {
    *error = "bootstrap.validate-source expects source address and length";
    return LAINIR_RUN_BAD_CALL;
  }
  if (!args[0].as.addr) {
    *error = "bootstrap.validate-source received a null source";
    return LAINIR_RUN_BAD_CALL;
  }
  if (args[1].as.bits > SIZE_MAX - 1) {
    *error = "bootstrap.validate-source length is too large";
    return LAINIR_RUN_BAD_CALL;
  }
  length = (size_t)args[1].as.bits;
  if (memchr(args[0].as.addr, '\0', length) != NULL) {
    *error = "bootstrap.validate-source received an embedded NUL";
    return LAINIR_RUN_BAD_CALL;
  }
  /* Normal compiler calls refer to the source that bootstrap_run_cli already
   * parsed and verified.  Reuse that owned handle so validation does not
   * perform a second full parse. */
  if (context && context->source_count == 1 && context->sources &&
      args[0].as.addr == context->sources[0].bytes &&
      length == context->sources[0].length && context->prepared_module) {
    *result = lainir_value_bits(1, 1);
    return LAINIR_RUN_OK;
  }
  /* Keep the capability useful for callers that provide an independent
   * buffer; this path remains owned and released entirely inside the call. */
  copy = malloc(length + 1);
  if (!copy) {
    *error = "bootstrap.validate-source allocation failed";
    return LAINIR_RUN_TRAP;
  }
  memcpy(copy, args[0].as.addr, length);
  copy[length] = '\0';
  if (!lainir_parse_module_checked(copy, &module, &diagnostic) ||
      !lainir_verify_module(module, NULL, &diagnostic)) {
    free(copy);
    lainir_free_subroutines(module);
    *error = diagnostic.message[0] ? diagnostic.message
                                   : "seed parser/verifier rejected source";
    return LAINIR_RUN_BAD_CALL;
  }
  free(copy);
  lainir_free_subroutines(module);
  *result = lainir_value_bits(1, 1);
  return LAINIR_RUN_OK;
}

static LainirRunStatus ir_module_id(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  BootstrapContext *context = user_data;
  (void)args;
  (void)error;
  if (count != 2 || !context || !context->prepared_module) {
    if (error) *error = "bootstrap.ir-module-id has no prepared module";
    return LAINIR_RUN_BAD_CALL;
  }
  *result = lainir_value_bits(
      (uint64_t)(uintptr_t)context->prepared_module, 64);
  return LAINIR_RUN_OK;
}

static LainirRunStatus ir_module_id_release(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  BootstrapContext *context = user_data;
  uint64_t id;
  (void)result;
  if (count != 1 || args[0].kind != LAINIR_VALUE_BITS || !context) {
    *error = "bootstrap.ir-module-id-release expects an id";
    return LAINIR_RUN_BAD_CALL;
  }
  id = args[0].as.bits;
  if (id != (uint64_t)(uintptr_t)context->prepared_module) {
    *error = "bootstrap.ir-module-id-release received an unknown id";
    return LAINIR_RUN_BAD_CALL;
  }
  /* The prepared module is owned by the host and is released at cleanup. */
  return LAINIR_RUN_OK;
}

static int ir_id_matches_prepared(const BootstrapContext *context,
                                  uint64_t id) {
  return context && context->prepared_module &&
      id == (uint64_t)(uintptr_t)context->prepared_module;
}

/* Procedure ids are borrowed pointer values exposed as opaque integers to
 * L1.  Validate membership before dereferencing one so a stale or forged id
 * becomes a normal bad-call result instead of a host crash. */
static const L1Subroutine *ir_find_prepared_procedure(
    const BootstrapContext *context, uint64_t id) {
  const L1Subroutine *procedure;
  if (!context || !context->prepared_module || id == 0)
    return NULL;
  for (procedure = lainir_module_handle_first(context->prepared_module);
       procedure; procedure = lainir_procedure_next(procedure)) {
    if ((uint64_t)(uintptr_t)procedure == id)
      return procedure;
  }
  return NULL;
}

static const L1Block *ir_find_block_tree(const L1Block *block, uint64_t id) {
  for (; block; block = lainir_block_next(block)) {
    const L1Instruction *instruction;
    if ((uint64_t)(uintptr_t)block == id) return block;
    for (instruction = lainir_block_first_instruction(block);
         instruction; instruction = lainir_instruction_next(instruction)) {
      const L1Block *nested = lainir_instruction_then_block(instruction);
      const L1Block *found;
      found = ir_find_block_tree(nested, id);
      if (found) return found;
      nested = lainir_instruction_else_block(instruction);
      found = ir_find_block_tree(nested, id);
      if (found) return found;
      nested = lainir_instruction_loop_block(instruction);
      found = ir_find_block_tree(nested, id);
      if (found) return found;
    }
  }
  return NULL;
}

static const L1Block *ir_find_prepared_block(
    const BootstrapContext *context, uint64_t id) {
  const L1Subroutine *procedure;
  if (!context || !context->prepared_module || id == 0) return NULL;
  for (procedure = lainir_module_handle_first(context->prepared_module);
       procedure; procedure = lainir_procedure_next(procedure)) {
    const L1Block *found = ir_find_block_tree(
        lainir_procedure_first_block(procedure), id);
    if (found) return found;
  }
  return NULL;
}

static const L1Instruction *ir_find_instruction_tree(
    const L1Block *block, uint64_t id) {
  for (; block; block = lainir_block_next(block)) {
    const L1Instruction *instruction;
    for (instruction = lainir_block_first_instruction(block);
         instruction; instruction = lainir_instruction_next(instruction)) {
      const L1Block *nested;
      const L1Instruction *found;
      if ((uint64_t)(uintptr_t)instruction == id) return instruction;
      nested = lainir_instruction_then_block(instruction);
      found = ir_find_instruction_tree(nested, id);
      if (found) return found;
      nested = lainir_instruction_else_block(instruction);
      found = ir_find_instruction_tree(nested, id);
      if (found) return found;
      nested = lainir_instruction_loop_block(instruction);
      found = ir_find_instruction_tree(nested, id);
      if (found) return found;
    }
  }
  return NULL;
}

static const L1Instruction *ir_find_prepared_instruction(
    const BootstrapContext *context, uint64_t id) {
  const L1Subroutine *procedure;
  if (!context || !context->prepared_module || id == 0) return NULL;
  for (procedure = lainir_module_handle_first(context->prepared_module);
       procedure; procedure = lainir_procedure_next(procedure)) {
    const L1Instruction *found = ir_find_instruction_tree(
        lainir_procedure_first_block(procedure), id);
    if (found) return found;
  }
  return NULL;
}

/* Expressions are borrowed pointers nested in instructions.  Resolve an
 * opaque expression id by walking the prepared module before dereferencing
 * it; forged or stale ids therefore become a normal bad-call result. */
static const L1Expr *ir_find_expression_tree(const L1Expr *expression,
                                              uint64_t id, unsigned depth);

static const L1Expr *ir_find_expression_in_block(const L1Block *block,
                                                  uint64_t id,
                                                  unsigned depth) {
  for (; block; block = lainir_block_next(block)) {
    const L1Instruction *instruction;
    for (instruction = lainir_block_first_instruction(block); instruction;
         instruction = lainir_instruction_next(instruction)) {
      const L1Expr *found;
      if ((found = ir_find_expression_tree(
              lainir_instruction_value(instruction), id, depth + 1))) return found;
      if ((found = ir_find_expression_tree(
              lainir_instruction_destination(instruction), id, depth + 1))) return found;
      if ((found = ir_find_expression_tree(
              lainir_instruction_condition(instruction), id, depth + 1))) return found;
      if ((found = ir_find_expression_in_block(
              lainir_instruction_then_block(instruction), id, depth + 1))) return found;
      if ((found = ir_find_expression_in_block(
              lainir_instruction_else_block(instruction), id, depth + 1))) return found;
      if ((found = ir_find_expression_in_block(
              lainir_instruction_loop_block(instruction), id, depth + 1))) return found;
    }
  }
  return NULL;
}

static const L1Expr *ir_find_expression_tree(const L1Expr *expression,
                                              uint64_t id, unsigned depth) {
  uint32_t index, count;
  const L1Expr *found;
  if (!expression || depth > 1024) return NULL;
  if ((uint64_t)(uintptr_t)expression == id) return expression;
  if ((found = ir_find_expression_tree(lainir_expr_left(expression), id, depth + 1))) return found;
  if ((found = ir_find_expression_tree(lainir_expr_right(expression), id, depth + 1))) return found;
  if ((found = ir_find_expression_tree(lainir_expr_operand(expression), id, depth + 1))) return found;
  if ((found = ir_find_expression_in_block(lainir_expr_block(expression), id, depth + 1))) return found;
  count = lainir_expr_argument_count(expression);
  for (index = 0; index < count; ++index) {
    if ((found = ir_find_expression_tree(
            lainir_expr_argument_at(expression, index), id, depth + 1))) return found;
  }
  return NULL;
}

static const L1Expr *ir_find_prepared_expression(
    const BootstrapContext *context, uint64_t id) {
  const L1Subroutine *procedure;
  if (!context || !context->prepared_module || id == 0) return NULL;
  for (procedure = lainir_module_handle_first(context->prepared_module);
       procedure; procedure = lainir_procedure_next(procedure)) {
    const L1Expr *found = ir_find_expression_in_block(
        lainir_procedure_first_block(procedure), id, 0);
    if (found) return found;
  }
  return NULL;
}

static LainirRunStatus ir_module_first(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  BootstrapContext *context = user_data;
  const L1Subroutine *procedure;
  if (count != 1 || args[0].kind != LAINIR_VALUE_BITS ||
      !ir_id_matches_prepared(context, args[0].as.bits)) {
    *error = "bootstrap.ir-module-first expects a valid module id";
    return LAINIR_RUN_BAD_CALL;
  }
  procedure = lainir_module_handle_first(context->prepared_module);
  *result = lainir_value_bits((uint64_t)(uintptr_t)procedure, 64);
  return LAINIR_RUN_OK;
}

static LainirRunStatus ir_procedure_find(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  BootstrapContext *context = user_data;
  const L1Subroutine *procedure;
  const char *name;
  if (count != 2 || args[0].kind != LAINIR_VALUE_BITS ||
      args[1].kind != LAINIR_VALUE_ADDR ||
      !context || !context->prepared_module ||
      args[0].as.bits != (uint64_t)(uintptr_t)context->prepared_module ||
      !(name = (const char *)args[1].as.addr)) {
    *error = "bootstrap.ir-procedure-find expects a module id and name";
    return LAINIR_RUN_BAD_CALL;
  }
  for (procedure = lainir_module_handle_first(context->prepared_module);
       procedure; procedure = lainir_procedure_next(procedure)) {
    if (lainir_procedure_name(procedure) &&
        strcmp(lainir_procedure_name(procedure), name) == 0) {
      *result = lainir_value_bits((uint64_t)(uintptr_t)procedure, 64);
      return LAINIR_RUN_OK;
    }
  }
  *result = lainir_value_bits(0, 64);
  return LAINIR_RUN_OK;
}

static LainirRunStatus ir_procedure_next(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  const L1Subroutine *procedure;
  const L1Subroutine *next;
  BootstrapContext *context = user_data;
  if (count != 1 || args[0].kind != LAINIR_VALUE_BITS) {
    *error = "bootstrap.ir-procedure-next expects a procedure id";
    return LAINIR_RUN_BAD_CALL;
  }
  procedure = ir_find_prepared_procedure(context, args[0].as.bits);
  if (!procedure) {
    *error = "bootstrap.ir-procedure-next received an unknown id";
    return LAINIR_RUN_BAD_CALL;
  }
  next = lainir_procedure_next(procedure);
  *result = lainir_value_bits((uint64_t)(uintptr_t)next, 64);
  return LAINIR_RUN_OK;
}

static LainirRunStatus ir_procedure_name(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  const L1Subroutine *procedure;
  BootstrapContext *context = user_data;
  if (count != 1 || args[0].kind != LAINIR_VALUE_BITS) {
    *error = "bootstrap.ir-procedure-name expects a procedure id";
    return LAINIR_RUN_BAD_CALL;
  }
  procedure = ir_find_prepared_procedure(context, args[0].as.bits);
  if (!procedure) {
    *error = "bootstrap.ir-procedure-name received an unknown id";
    return LAINIR_RUN_BAD_CALL;
  }
  *result = lainir_value_addr((void *)lainir_procedure_name(procedure));
  return LAINIR_RUN_OK;
}

static LainirRunStatus ir_procedure_name_length(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  BootstrapContext *context = user_data;
  const L1Subroutine *procedure;
  if (count != 1 || args[0].kind != LAINIR_VALUE_BITS ||
      !(procedure = ir_find_prepared_procedure(context, args[0].as.bits))) {
    *error = "bootstrap.ir-procedure-name-length expects a valid procedure id";
    return LAINIR_RUN_BAD_CALL;
  }
  *result = lainir_value_bits(lainir_procedure_name_length(procedure), 32);
  return LAINIR_RUN_OK;
}

static LainirRunStatus ir_procedure_link_name(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  const L1Subroutine *procedure;
  BootstrapContext *context = user_data;
  const char *name;
  if (count != 1 || args[0].kind != LAINIR_VALUE_BITS ||
      !(procedure = ir_find_prepared_procedure(context, args[0].as.bits))) {
    *error = "bootstrap.ir-procedure-link-name expects a procedure id";
    return LAINIR_RUN_BAD_CALL;
  }
  name = lainir_procedure_link_name(procedure);
  if (!name) {
    *error = "bootstrap.ir-procedure-link-name is unavailable";
    return LAINIR_RUN_BAD_CALL;
  }
  *result = lainir_value_addr((void *)name);
  return LAINIR_RUN_OK;
}

static LainirRunStatus ir_procedure_return_width(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  const L1Subroutine *procedure;
  BootstrapContext *context = user_data;
  if (count != 1 || args[0].kind != LAINIR_VALUE_BITS) {
    *error = "bootstrap.ir-procedure-return-width expects a procedure id";
    return LAINIR_RUN_BAD_CALL;
  }
  procedure = ir_find_prepared_procedure(context, args[0].as.bits);
  if (!procedure) {
    *error = "bootstrap.ir-procedure-return-width received an unknown id";
    return LAINIR_RUN_BAD_CALL;
  }
  *result = lainir_value_bits(
      lainir_type_width(lainir_procedure_return_type(procedure)), 32);
  return LAINIR_RUN_OK;
}

static LainirRunStatus ir_procedure_return_kind(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  const L1Subroutine *procedure;
  const L1Type *type;
  BootstrapContext *context = user_data;
  if (count != 1 || args[0].kind != LAINIR_VALUE_BITS ||
      !(procedure = ir_find_prepared_procedure(context, args[0].as.bits))) {
    *error = "bootstrap.ir-procedure-return-kind expects a procedure id";
    return LAINIR_RUN_BAD_CALL;
  }
  type = lainir_procedure_return_type(procedure);
  *result = lainir_value_bits(type ? (uint64_t)lainir_type_kind(type) : 0, 32);
  return LAINIR_RUN_OK;
}

static LainirRunStatus ir_procedure_parameter_count(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  const L1Subroutine *procedure;
  BootstrapContext *context = user_data;
  if (count != 1 || args[0].kind != LAINIR_VALUE_BITS) {
    *error = "bootstrap.ir-procedure-parameter-count expects a procedure id";
    return LAINIR_RUN_BAD_CALL;
  }
  procedure = ir_find_prepared_procedure(context, args[0].as.bits);
  if (!procedure) {
    *error = "bootstrap.ir-procedure-parameter-count received an unknown id";
    return LAINIR_RUN_BAD_CALL;
  }
  *result = lainir_value_bits(lainir_procedure_parameter_count(procedure), 32);
  return LAINIR_RUN_OK;
}

static LainirRunStatus ir_procedure_external(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  const L1Subroutine *procedure;
  BootstrapContext *context = user_data;
  if (count != 1 || args[0].kind != LAINIR_VALUE_BITS) {
    *error = "bootstrap.ir-procedure-external expects a procedure id";
    return LAINIR_RUN_BAD_CALL;
  }
  procedure = ir_find_prepared_procedure(context, args[0].as.bits);
  if (!procedure) {
    *error = "bootstrap.ir-procedure-external received an unknown id";
    return LAINIR_RUN_BAD_CALL;
  }
  *result = lainir_value_bits(lainir_procedure_is_external(procedure), 1);
  return LAINIR_RUN_OK;
}

static LainirRunStatus ir_procedure_data(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  BootstrapContext *context = user_data;
  const L1Subroutine *item;
  if (count != 1 || args[0].kind != LAINIR_VALUE_BITS ||
      !(item = ir_find_prepared_procedure(context, args[0].as.bits))) {
    *error = "bootstrap.ir-procedure-data expects a valid module item id";
    return LAINIR_RUN_BAD_CALL;
  }
  *result = lainir_value_bits(lainir_procedure_is_data(item), 1);
  return LAINIR_RUN_OK;
}

static LainirRunStatus ir_data_size(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  BootstrapContext *context = user_data;
  const L1Subroutine *item;
  if (count != 1 || args[0].kind != LAINIR_VALUE_BITS ||
      !(item = ir_find_prepared_procedure(context, args[0].as.bits)) ||
      !lainir_procedure_is_data(item)) {
    *error = "bootstrap.ir-data-size expects a valid data item id";
    return LAINIR_RUN_BAD_CALL;
  }
  *result = lainir_value_bits(lainir_data_size(item), 64);
  return LAINIR_RUN_OK;
}

static LainirRunStatus ir_data_alignment(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  BootstrapContext *context = user_data;
  const L1Subroutine *item;
  if (count != 1 || args[0].kind != LAINIR_VALUE_BITS ||
      !(item = ir_find_prepared_procedure(context, args[0].as.bits)) ||
      !lainir_procedure_is_data(item)) {
    *error = "bootstrap.ir-data-alignment expects a valid data item id";
    return LAINIR_RUN_BAD_CALL;
  }
  *result = lainir_value_bits(lainir_data_alignment(item), 64);
  return LAINIR_RUN_OK;
}

static LainirRunStatus ir_data_byte(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  BootstrapContext *context = user_data;
  const L1Subroutine *item;
  const uint8_t *bytes;
  if (count != 2 || args[0].kind != LAINIR_VALUE_BITS ||
      args[1].kind != LAINIR_VALUE_BITS ||
      !(item = ir_find_prepared_procedure(context, args[0].as.bits)) ||
      !lainir_procedure_is_data(item) ||
      args[1].as.bits >= lainir_data_size(item) ||
      !(bytes = lainir_data_bytes(item))) {
    *error = "bootstrap.ir-data-byte expects a valid data item and byte index";
    return LAINIR_RUN_BAD_CALL;
  }
  *result = lainir_value_bits(bytes[args[1].as.bits], 8);
  return LAINIR_RUN_OK;
}

static LainirRunStatus ir_procedure_parameter_name(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  BootstrapContext *context = user_data;
  const L1Subroutine *procedure;
  const char *name;
  if (count != 2 || args[0].kind != LAINIR_VALUE_BITS ||
      args[1].kind != LAINIR_VALUE_BITS ||
      !(procedure = ir_find_prepared_procedure(context, args[0].as.bits))) {
    *error = "bootstrap.ir-procedure-parameter-name expects a valid procedure and index";
    return LAINIR_RUN_BAD_CALL;
  }
  if (args[1].as.bits > UINT32_MAX) {
    *error = "bootstrap.ir-procedure-parameter-name index is too large";
    return LAINIR_RUN_BAD_CALL;
  }
  name = lainir_procedure_parameter_name(procedure, (uint32_t)args[1].as.bits);
  if (!name) {
    *error = "bootstrap.ir-procedure-parameter-name received an out-of-range index";
    return LAINIR_RUN_BAD_CALL;
  }
  *result = lainir_value_addr((void *)name);
  return LAINIR_RUN_OK;
}

static LainirRunStatus ir_procedure_parameter_width(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  BootstrapContext *context = user_data;
  const L1Subroutine *procedure;
  const L1Type *type;
  if (count != 2 || args[0].kind != LAINIR_VALUE_BITS ||
      args[1].kind != LAINIR_VALUE_BITS ||
      !(procedure = ir_find_prepared_procedure(context, args[0].as.bits))) {
    *error = "bootstrap.ir-procedure-parameter-width expects a valid procedure and index";
    return LAINIR_RUN_BAD_CALL;
  }
  if (args[1].as.bits > UINT32_MAX) {
    *error = "bootstrap.ir-procedure-parameter-width index is too large";
    return LAINIR_RUN_BAD_CALL;
  }
  type = lainir_procedure_parameter_type(procedure, (uint32_t)args[1].as.bits);
  if (!type) {
    *error = "bootstrap.ir-procedure-parameter-width received an out-of-range index";
    return LAINIR_RUN_BAD_CALL;
  }
  *result = lainir_value_bits(lainir_type_width(type), 32);
  return LAINIR_RUN_OK;
}

static LainirRunStatus ir_procedure_parameter_kind(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  BootstrapContext *context = user_data;
  const L1Subroutine *procedure;
  const L1Type *type;
  if (count != 2 || args[0].kind != LAINIR_VALUE_BITS ||
      args[1].kind != LAINIR_VALUE_BITS ||
      !(procedure = ir_find_prepared_procedure(context, args[0].as.bits)) ||
      args[1].as.bits > UINT32_MAX) {
    *error = "bootstrap.ir-procedure-parameter-kind expects a valid procedure and index";
    return LAINIR_RUN_BAD_CALL;
  }
  type = lainir_procedure_parameter_type(procedure, (uint32_t)args[1].as.bits);
  if (!type) {
    *error = "bootstrap.ir-procedure-parameter-kind received an out-of-range index";
    return LAINIR_RUN_BAD_CALL;
  }
  *result = lainir_value_bits((uint64_t)lainir_type_kind(type), 32);
  return LAINIR_RUN_OK;
}

static LainirRunStatus ir_procedure_first_block(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  BootstrapContext *context = user_data;
  const L1Subroutine *procedure;
  const L1Block *block;
  if (count != 1 || args[0].kind != LAINIR_VALUE_BITS ||
      !(procedure = ir_find_prepared_procedure(context, args[0].as.bits))) {
    *error = "bootstrap.ir-procedure-first-block expects a valid procedure id";
    return LAINIR_RUN_BAD_CALL;
  }
  block = lainir_procedure_first_block(procedure);
  *result = lainir_value_bits((uint64_t)(uintptr_t)block, 64);
  return LAINIR_RUN_OK;
}

static LainirRunStatus ir_block_first_instruction(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  BootstrapContext *context = user_data;
  const L1Block *block;
  if (count != 1 || args[0].kind != LAINIR_VALUE_BITS ||
      !(block = ir_find_prepared_block(context, args[0].as.bits))) {
    *error = "bootstrap.ir-block-first-instruction expects a valid block id";
    return LAINIR_RUN_BAD_CALL;
  }
  *result = lainir_value_bits(
      (uint64_t)(uintptr_t)lainir_block_first_instruction(block), 64);
  return LAINIR_RUN_OK;
}

static LainirRunStatus ir_block_next(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  BootstrapContext *context = user_data;
  const L1Block *block;
  const L1Block *next;
  if (count != 1 || args[0].kind != LAINIR_VALUE_BITS ||
      !(block = ir_find_prepared_block(context, args[0].as.bits))) {
    *error = "bootstrap.ir-block-next expects a valid block id";
    return LAINIR_RUN_BAD_CALL;
  }
  next = lainir_block_next(block);
  *result = lainir_value_bits((uint64_t)(uintptr_t)next, 64);
  return LAINIR_RUN_OK;
}

static LainirRunStatus ir_instruction_next(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  BootstrapContext *context = user_data;
  const L1Instruction *instruction;
  if (count != 1 || args[0].kind != LAINIR_VALUE_BITS ||
      !(instruction = ir_find_prepared_instruction(context, args[0].as.bits))) {
    *error = "bootstrap.ir-instruction-next expects a valid instruction id";
    return LAINIR_RUN_BAD_CALL;
  }
  *result = lainir_value_bits(
      (uint64_t)(uintptr_t)lainir_instruction_next(instruction), 64);
  return LAINIR_RUN_OK;
}

static LainirRunStatus ir_instruction_kind(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  BootstrapContext *context = user_data;
  const L1Instruction *instruction;
  if (count != 1 || args[0].kind != LAINIR_VALUE_BITS ||
      !(instruction = ir_find_prepared_instruction(context, args[0].as.bits))) {
    *error = "bootstrap.ir-instruction-kind expects a valid instruction id";
    return LAINIR_RUN_BAD_CALL;
  }
  *result = lainir_value_bits(lainir_instruction_kind(instruction), 32);
  return LAINIR_RUN_OK;
}

static LainirRunStatus ir_instruction_value(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  BootstrapContext *context = user_data;
  const L1Instruction *instruction;
  if (count != 1 || args[0].kind != LAINIR_VALUE_BITS ||
      !(instruction = ir_find_prepared_instruction(context, args[0].as.bits))) {
    *error = "bootstrap.ir-instruction-value expects a valid instruction id";
    return LAINIR_RUN_BAD_CALL;
  }
  *result = lainir_value_bits(
      (uint64_t)(uintptr_t)lainir_instruction_value(instruction), 64);
  return LAINIR_RUN_OK;
}

static LainirRunStatus ir_instruction_destination(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  BootstrapContext *context = user_data;
  const L1Instruction *instruction;
  if (count != 1 || args[0].kind != LAINIR_VALUE_BITS ||
      !(instruction = ir_find_prepared_instruction(context, args[0].as.bits))) {
    *error = "bootstrap.ir-instruction-destination expects a valid instruction id";
    return LAINIR_RUN_BAD_CALL;
  }
  *result = lainir_value_bits(
      (uint64_t)(uintptr_t)lainir_instruction_destination(instruction), 64);
  return LAINIR_RUN_OK;
}

static LainirRunStatus ir_instruction_name(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  BootstrapContext *context = user_data;
  const L1Instruction *instruction;
  const char *name;
  if (count != 1 || args[0].kind != LAINIR_VALUE_BITS ||
      !(instruction = ir_find_prepared_instruction(context, args[0].as.bits))) {
    *error = "bootstrap.ir-instruction-name expects a valid instruction id";
    return LAINIR_RUN_BAD_CALL;
  }
  name = lainir_instruction_name(instruction);
  if (!name) {
    *error = "bootstrap.ir-instruction-name is unavailable for this instruction";
    return LAINIR_RUN_BAD_CALL;
  }
  *result = lainir_value_addr((void *)name);
  return LAINIR_RUN_OK;
}

static LainirRunStatus ir_instruction_label(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  BootstrapContext *context = user_data;
  const L1Instruction *instruction;
  const char *label;
  if (count != 1 || args[0].kind != LAINIR_VALUE_BITS ||
      !(instruction = ir_find_prepared_instruction(context, args[0].as.bits))) {
    *error = "bootstrap.ir-instruction-label expects a valid instruction id";
    return LAINIR_RUN_BAD_CALL;
  }
  label = lainir_instruction_label(instruction);
  *result = lainir_value_addr((void *)label);
  return LAINIR_RUN_OK;
}

static LainirRunStatus ir_instruction_type_width(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  BootstrapContext *context = user_data;
  const L1Instruction *instruction;
  const L1Type *type;
  if (count != 1 || args[0].kind != LAINIR_VALUE_BITS ||
      !(instruction = ir_find_prepared_instruction(context, args[0].as.bits))) {
    *error = "bootstrap.ir-instruction-type-width expects a valid instruction id";
    return LAINIR_RUN_BAD_CALL;
  }
  type = lainir_instruction_type(instruction);
  *result = lainir_value_bits(type ? lainir_type_width(type) : 0, 32);
  return LAINIR_RUN_OK;
}

static LainirRunStatus ir_instruction_type_kind(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  BootstrapContext *context = user_data;
  const L1Instruction *instruction;
  const L1Type *type;
  if (count != 1 || args[0].kind != LAINIR_VALUE_BITS ||
      !(instruction = ir_find_prepared_instruction(context, args[0].as.bits))) {
    *error = "bootstrap.ir-instruction-type-kind expects a valid instruction id";
    return LAINIR_RUN_BAD_CALL;
  }
  type = lainir_instruction_type(instruction);
  *result = lainir_value_bits(type ? (uint64_t)lainir_type_kind(type) : 0, 32);
  return LAINIR_RUN_OK;
}

static LainirRunStatus ir_instruction_condition(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  BootstrapContext *context = user_data;
  const L1Instruction *instruction;
  if (count != 1 || args[0].kind != LAINIR_VALUE_BITS ||
      !(instruction = ir_find_prepared_instruction(context, args[0].as.bits))) {
    *error = "bootstrap.ir-instruction-condition expects a valid instruction id";
    return LAINIR_RUN_BAD_CALL;
  }
  *result = lainir_value_bits(
      (uint64_t)(uintptr_t)lainir_instruction_condition(instruction), 64);
  return LAINIR_RUN_OK;
}

static LainirRunStatus ir_instruction_then_block(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  BootstrapContext *context = user_data;
  const L1Instruction *instruction;
  if (count != 1 || args[0].kind != LAINIR_VALUE_BITS ||
      !(instruction = ir_find_prepared_instruction(context, args[0].as.bits))) {
    *error = "bootstrap.ir-instruction-then-block expects a valid instruction id";
    return LAINIR_RUN_BAD_CALL;
  }
  *result = lainir_value_bits(
      (uint64_t)(uintptr_t)lainir_instruction_then_block(instruction), 64);
  return LAINIR_RUN_OK;
}

static LainirRunStatus ir_instruction_else_block(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  BootstrapContext *context = user_data;
  const L1Instruction *instruction;
  if (count != 1 || args[0].kind != LAINIR_VALUE_BITS ||
      !(instruction = ir_find_prepared_instruction(context, args[0].as.bits))) {
    *error = "bootstrap.ir-instruction-else-block expects a valid instruction id";
    return LAINIR_RUN_BAD_CALL;
  }
  *result = lainir_value_bits(
      (uint64_t)(uintptr_t)lainir_instruction_else_block(instruction), 64);
  return LAINIR_RUN_OK;
}

static LainirRunStatus ir_instruction_loop_block(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  BootstrapContext *context = user_data;
  const L1Instruction *instruction;
  if (count != 1 || args[0].kind != LAINIR_VALUE_BITS ||
      !(instruction = ir_find_prepared_instruction(context, args[0].as.bits))) {
    *error = "bootstrap.ir-instruction-loop-block expects a valid instruction id";
    return LAINIR_RUN_BAD_CALL;
  }
  *result = lainir_value_bits(
      (uint64_t)(uintptr_t)lainir_instruction_loop_block(instruction), 64);
  return LAINIR_RUN_OK;
}

static LainirRunStatus ir_expression_kind(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  const L1Expr *expression;
  (void)user_data;
  if (count != 1 || args[0].kind != LAINIR_VALUE_BITS ||
      !(expression = ir_find_prepared_expression((const BootstrapContext *)user_data, args[0].as.bits))) {
    *error = "bootstrap.ir-expression-kind expects an expression id";
    return LAINIR_RUN_BAD_CALL;
  }
  *result = lainir_value_bits(lainir_expr_kind(expression), 32);
  return LAINIR_RUN_OK;
}

static LainirRunStatus ir_expression_const(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  const L1Expr *expression;
  (void)user_data;
  if (count != 1 || args[0].kind != LAINIR_VALUE_BITS ||
      !(expression = ir_find_prepared_expression((const BootstrapContext *)user_data, args[0].as.bits))) {
    *error = "bootstrap.ir-expression-const expects an expression id";
    return LAINIR_RUN_BAD_CALL;
  }
  *result = lainir_value_bits((uint64_t)lainir_expr_const_value(expression), 64);
  return LAINIR_RUN_OK;
}

static LainirRunStatus ir_expression_left(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  const L1Expr *expression;
  (void)user_data;
  if (count != 1 || args[0].kind != LAINIR_VALUE_BITS ||
      !(expression = ir_find_prepared_expression((const BootstrapContext *)user_data, args[0].as.bits))) {
    *error = "bootstrap.ir-expression-left expects an expression id";
    return LAINIR_RUN_BAD_CALL;
  }
  *result = lainir_value_bits(
      (uint64_t)(uintptr_t)lainir_expr_left(expression), 64);
  return LAINIR_RUN_OK;
}

static LainirRunStatus ir_expression_right(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  const L1Expr *expression;
  (void)user_data;
  if (count != 1 || args[0].kind != LAINIR_VALUE_BITS ||
      !(expression = ir_find_prepared_expression((const BootstrapContext *)user_data, args[0].as.bits))) {
    *error = "bootstrap.ir-expression-right expects an expression id";
    return LAINIR_RUN_BAD_CALL;
  }
  *result = lainir_value_bits(
      (uint64_t)(uintptr_t)lainir_expr_right(expression), 64);
  return LAINIR_RUN_OK;
}

static LainirRunStatus ir_expression_arg_index(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  const L1Expr *expression;
  (void)user_data;
  if (count != 1 || args[0].kind != LAINIR_VALUE_BITS ||
      !(expression = ir_find_prepared_expression((const BootstrapContext *)user_data, args[0].as.bits))) {
    *error = "bootstrap.ir-expression-arg-index expects an expression id";
    return LAINIR_RUN_BAD_CALL;
  }
  *result = lainir_value_bits(lainir_expr_arg_index(expression), 32);
  return LAINIR_RUN_OK;
}

static LainirRunStatus ir_expression_callee_name(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  const L1Expr *expression;
  const char *name;
  (void)user_data;
  if (count != 1 || args[0].kind != LAINIR_VALUE_BITS ||
      !(expression = ir_find_prepared_expression((const BootstrapContext *)user_data, args[0].as.bits))) {
    *error = "bootstrap.ir-expression-callee-name expects an expression id";
    return LAINIR_RUN_BAD_CALL;
  }
  name = lainir_expr_callee_name(expression);
  if (!name) {
    *error = "bootstrap.ir-expression-callee-name is unavailable for this expression";
    return LAINIR_RUN_BAD_CALL;
  }
  *result = lainir_value_addr((void *)name);
  return LAINIR_RUN_OK;
}

static LainirRunStatus ir_expression_name(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  const L1Expr *expression;
  const char *name;
  (void)user_data;
  if (count != 1 || args[0].kind != LAINIR_VALUE_BITS ||
      !(expression = ir_find_prepared_expression((const BootstrapContext *)user_data, args[0].as.bits))) {
    *error = "bootstrap.ir-expression-name expects an expression id";
    return LAINIR_RUN_BAD_CALL;
  }
  name = lainir_expr_name(expression);
  if (!name) {
    *error = "bootstrap.ir-expression-name is unavailable for this expression";
    return LAINIR_RUN_BAD_CALL;
  }
  *result = lainir_value_addr((void *)name);
  return LAINIR_RUN_OK;
}

static LainirRunStatus ir_expression_argument_count(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  const L1Expr *expression;
  (void)user_data;
  if (count != 1 || args[0].kind != LAINIR_VALUE_BITS ||
      !(expression = ir_find_prepared_expression((const BootstrapContext *)user_data, args[0].as.bits))) {
    *error = "bootstrap.ir-expression-argument-count expects an expression id";
    return LAINIR_RUN_BAD_CALL;
  }
  *result = lainir_value_bits(lainir_expr_argument_count(expression), 32);
  return LAINIR_RUN_OK;
}

static LainirRunStatus ir_expression_argument_at(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  const L1Expr *expression;
  const L1Expr *argument;
  (void)user_data;
  if (count != 2 || args[0].kind != LAINIR_VALUE_BITS ||
      args[1].kind != LAINIR_VALUE_BITS ||
      !(expression = ir_find_prepared_expression((const BootstrapContext *)user_data, args[0].as.bits))) {
    *error = "bootstrap.ir-expression-argument-at expects an expression and index";
    return LAINIR_RUN_BAD_CALL;
  }
  if (args[1].as.bits > UINT32_MAX) {
    *error = "bootstrap.ir-expression-argument-at index is too large";
    return LAINIR_RUN_BAD_CALL;
  }
  argument = lainir_expr_argument_at(expression, (uint32_t)args[1].as.bits);
  if (!argument) {
    *error = "bootstrap.ir-expression-argument-at received an out-of-range index";
    return LAINIR_RUN_BAD_CALL;
  }
  *result = lainir_value_bits((uint64_t)(uintptr_t)argument, 64);
  return LAINIR_RUN_OK;
}

static LainirRunStatus ir_expression_type_width(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  const L1Expr *expression;
  const L1Type *type;
  (void)user_data;
  if (count != 1 || args[0].kind != LAINIR_VALUE_BITS ||
      !(expression = ir_find_prepared_expression((const BootstrapContext *)user_data, args[0].as.bits))) {
    *error = "bootstrap.ir-expression-type-width expects an expression id";
    return LAINIR_RUN_BAD_CALL;
  }
  type = lainir_expr_type(expression);
  *result = lainir_value_bits(type ? lainir_type_width(type) : 0, 32);
  return LAINIR_RUN_OK;
}

static LainirRunStatus ir_expression_type_kind(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  const L1Expr *expression;
  const L1Type *type;
  (void)user_data;
  if (count != 1 || args[0].kind != LAINIR_VALUE_BITS ||
      !(expression = ir_find_prepared_expression((const BootstrapContext *)user_data, args[0].as.bits))) {
    *error = "bootstrap.ir-expression-type-kind expects an expression id";
    return LAINIR_RUN_BAD_CALL;
  }
  type = lainir_expr_type(expression);
  *result = lainir_value_bits(type ? (uint64_t)lainir_type_kind(type) : 0, 32);
  return LAINIR_RUN_OK;
}

static LainirRunStatus ir_expression_operand(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  const L1Expr *expression;
  (void)user_data;
  if (count != 1 || args[0].kind != LAINIR_VALUE_BITS ||
      !(expression = ir_find_prepared_expression((const BootstrapContext *)user_data, args[0].as.bits))) {
    *error = "bootstrap.ir-expression-operand expects an expression id";
    return LAINIR_RUN_BAD_CALL;
  }
  *result = lainir_value_bits(
      (uint64_t)(uintptr_t)lainir_expr_operand(expression), 64);
  return LAINIR_RUN_OK;
}

static LainirRunStatus ir_expression_block(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  const L1Expr *expression;
  (void)user_data;
  if (count != 1 || args[0].kind != LAINIR_VALUE_BITS ||
      !(expression = ir_find_prepared_expression((const BootstrapContext *)user_data, args[0].as.bits))) {
    *error = "bootstrap.ir-expression-block expects an expression id";
    return LAINIR_RUN_BAD_CALL;
  }
  *result = lainir_value_bits(
      (uint64_t)(uintptr_t)lainir_expr_block(expression), 64);
  return LAINIR_RUN_OK;
}

static LainirRunStatus ir_expression_scale(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  const L1Expr *expression;
  (void)user_data;
  if (count != 1 || args[0].kind != LAINIR_VALUE_BITS ||
      !(expression = ir_find_prepared_expression((const BootstrapContext *)user_data, args[0].as.bits))) {
    *error = "bootstrap.ir-expression-scale expects an expression id";
    return LAINIR_RUN_BAD_CALL;
  }
  *result = lainir_value_bits(lainir_expr_scale(expression), 32);
  return LAINIR_RUN_OK;
}

static LainirRunStatus ir_expression_offset(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  const L1Expr *expression;
  (void)user_data;
  if (count != 1 || args[0].kind != LAINIR_VALUE_BITS ||
      !(expression = ir_find_prepared_expression((const BootstrapContext *)user_data, args[0].as.bits))) {
    *error = "bootstrap.ir-expression-offset expects an expression id";
    return LAINIR_RUN_BAD_CALL;
  }
  *result = lainir_value_bits(lainir_expr_offset(expression), 32);
  return LAINIR_RUN_OK;
}

static LainirRunStatus ir_expression_byte_size(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  const L1Expr *expression;
  (void)user_data;
  if (count != 1 || args[0].kind != LAINIR_VALUE_BITS ||
      !(expression = ir_find_prepared_expression((const BootstrapContext *)user_data, args[0].as.bits))) {
    *error = "bootstrap.ir-expression-byte-size expects an expression id";
    return LAINIR_RUN_BAD_CALL;
  }
  *result = lainir_value_bits(lainir_expr_byte_size(expression), 32);
  return LAINIR_RUN_OK;
}

static LainirRunStatus ir_expression_string(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  const L1Expr *expression;
  const char *string;
  (void)user_data;
  if (count != 1 || args[0].kind != LAINIR_VALUE_BITS ||
      !(expression = ir_find_prepared_expression((const BootstrapContext *)user_data, args[0].as.bits))) {
    *error = "bootstrap.ir-expression-string expects an expression id";
    return LAINIR_RUN_BAD_CALL;
  }
  string = lainir_expr_string(expression);
  if (!string) {
    *error = "bootstrap.ir-expression-string is unavailable for this expression";
    return LAINIR_RUN_BAD_CALL;
  }
  *result = lainir_value_addr((void *)string);
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

static LainirRunStatus set_allocation_limit(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  BootstrapContext *context = user_data;
  if (count != 1 || args[0].kind != LAINIR_VALUE_BITS) {
    *error = "bootstrap.set-allocation-limit expects one integer";
    return LAINIR_RUN_BAD_CALL;
  }
  if (!context) {
    *error = "bootstrap.set-allocation-limit has no context";
    return LAINIR_RUN_BAD_CALL;
  }
  context->max_allocated_page_bytes = args[0].as.bits;
  *result = lainir_value_unit();
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

/* Emit a source identifier in the C ABI spelling used by generated code.
 * Keeping this byte loop in the host avoids interpreting one L1 call for
 * every character while bootstrapping the compiler itself. */
static LainirRunStatus artifact_write_identifier(
    const LainirValue *args, uint32_t count, LainirValue *result,
    const char **error, void *user_data) {
  BootstrapContext *context = user_data;
  const unsigned char *name;
  if (count != 1 || args[0].kind != LAINIR_VALUE_ADDR || !context->artifact) {
    *error = "bootstrap.artifact-write-identifier has invalid arguments or state";
    return LAINIR_RUN_BAD_CALL;
  }
  name = (const unsigned char *)args[0].as.addr;
  if (!name) {
    *error = "bootstrap.artifact-write-identifier received null";
    return LAINIR_RUN_BAD_CALL;
  }
  for (; *name; ++name) {
    const char *replacement = NULL;
    if (*name == '.') replacement = "_dot_";
    else if (*name == '-') replacement = "_dash_";
    else if (*name == '!') replacement = "_bang_";
    if (replacement) fputs(replacement, context->artifact);
    else fputc(*name, context->artifact);
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
  if (context.source_count == 1) {
    if (lainir_module_parse_handle(
            (const char *)context.sources[0].bytes,
            &context.prepared_module, &diagnostic) != LAINIR_RUN_OK ||
        lainir_module_handle_verify(context.prepared_module, &diagnostic) !=
            LAINIR_RUN_OK) {
      fprintf(stderr, "input LAIN-IR rejected [%d] line %d: %s\n",
              diagnostic.code, diagnostic.line, diagnostic.message);
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
      !add_capability(caps, "bootstrap.eval_source", eval_source, &context) ||
      !add_capability(caps, "bootstrap.eval-next", eval_next, &context) ||
      !add_capability(caps, "bootstrap.validate-source", validate_source,
                      &context) ||
      !add_capability(caps, "bootstrap.ir-module-id", ir_module_id,
                      &context) ||
      !add_capability(caps, "bootstrap.ir-module-id-release", ir_module_id_release,
                      &context) ||
      !add_capability(caps, "bootstrap.ir-module-first", ir_module_first,
                      &context) ||
      !add_capability(caps, "bootstrap.ir-procedure-find", ir_procedure_find,
                      &context) ||
      !add_capability(caps, "bootstrap.ir-procedure-next", ir_procedure_next,
                      &context) ||
      !add_capability(caps, "bootstrap.ir-procedure-name", ir_procedure_name,
                      &context) ||
      !add_capability(caps, "bootstrap.ir-procedure-name-length",
                      ir_procedure_name_length, &context) ||
      !add_capability(caps, "bootstrap.ir-procedure-link-name",
                      ir_procedure_link_name, &context) ||
      !add_capability(caps, "bootstrap.ir-procedure-return-width",
                      ir_procedure_return_width, &context) ||
      !add_capability(caps, "bootstrap.ir-procedure-return-kind",
                      ir_procedure_return_kind, &context) ||
      !add_capability(caps, "bootstrap.ir-procedure-parameter-count",
                      ir_procedure_parameter_count, &context) ||
      !add_capability(caps, "bootstrap.ir-procedure-external",
                      ir_procedure_external, &context) ||
      !add_capability(caps, "bootstrap.ir-procedure-data",
                      ir_procedure_data, &context) ||
      !add_capability(caps, "bootstrap.ir-data-size", ir_data_size, &context) ||
      !add_capability(caps, "bootstrap.ir-data-alignment", ir_data_alignment,
                      &context) ||
      !add_capability(caps, "bootstrap.ir-data-byte", ir_data_byte, &context) ||
      !add_capability(caps, "bootstrap.ir-procedure-parameter-name",
                      ir_procedure_parameter_name, &context) ||
      !add_capability(caps, "bootstrap.ir-procedure-parameter-width",
                      ir_procedure_parameter_width, &context) ||
      !add_capability(caps, "bootstrap.ir-procedure-parameter-kind",
                      ir_procedure_parameter_kind, &context) ||
      !add_capability(caps, "bootstrap.ir-procedure-first-block",
                      ir_procedure_first_block, &context) ||
      !add_capability(caps, "bootstrap.ir-block-first-instruction",
                      ir_block_first_instruction, &context) ||
      !add_capability(caps, "bootstrap.ir-block-next",
                      ir_block_next, &context) ||
      !add_capability(caps, "bootstrap.ir-instruction-next",
                      ir_instruction_next, &context) ||
      !add_capability(caps, "bootstrap.ir-instruction-kind",
                      ir_instruction_kind, &context) ||
      !add_capability(caps, "bootstrap.ir-instruction-value",
                      ir_instruction_value, &context) ||
      !add_capability(caps, "bootstrap.ir-instruction-destination",
                      ir_instruction_destination, &context) ||
      !add_capability(caps, "bootstrap.ir-instruction-name",
                      ir_instruction_name, &context) ||
      !add_capability(caps, "bootstrap.ir-instruction-label",
                      ir_instruction_label, &context) ||
      !add_capability(caps, "bootstrap.ir-instruction-type-width",
                      ir_instruction_type_width, &context) ||
      !add_capability(caps, "bootstrap.ir-instruction-type-kind",
                      ir_instruction_type_kind, &context) ||
      !add_capability(caps, "bootstrap.ir-instruction-condition",
                      ir_instruction_condition, &context) ||
      !add_capability(caps, "bootstrap.ir-instruction-then-block",
                      ir_instruction_then_block, &context) ||
      !add_capability(caps, "bootstrap.ir-instruction-else-block",
                      ir_instruction_else_block, &context) ||
      !add_capability(caps, "bootstrap.ir-instruction-loop-block",
                      ir_instruction_loop_block, &context) ||
      !add_capability(caps, "bootstrap.ir-expression-kind",
                      ir_expression_kind, &context) ||
      !add_capability(caps, "bootstrap.ir-expression-const",
                      ir_expression_const, &context) ||
      !add_capability(caps, "bootstrap.ir-expression-left",
                      ir_expression_left, &context) ||
      !add_capability(caps, "bootstrap.ir-expression-right",
                      ir_expression_right, &context) ||
      !add_capability(caps, "bootstrap.ir-expression-arg-index",
                      ir_expression_arg_index, &context) ||
      !add_capability(caps, "bootstrap.ir-expression-callee-name",
                      ir_expression_callee_name, &context) ||
      !add_capability(caps, "bootstrap.ir-expression-name",
                      ir_expression_name, &context) ||
      !add_capability(caps, "bootstrap.ir-expression-argument-count",
                      ir_expression_argument_count, &context) ||
      !add_capability(caps, "bootstrap.ir-expression-argument-at",
                      ir_expression_argument_at, &context) ||
      !add_capability(caps, "bootstrap.ir-expression-type-width",
                      ir_expression_type_width, &context) ||
      !add_capability(caps, "bootstrap.ir-expression-type-kind",
                      ir_expression_type_kind, &context) ||
      !add_capability(caps, "bootstrap.ir-expression-operand",
                      ir_expression_operand, &context) ||
      !add_capability(caps, "bootstrap.ir-expression-block",
                      ir_expression_block, &context) ||
      !add_capability(caps, "bootstrap.ir-expression-scale",
                      ir_expression_scale, &context) ||
      !add_capability(caps, "bootstrap.ir-expression-offset",
                      ir_expression_offset, &context) ||
      !add_capability(caps, "bootstrap.ir-expression-byte-size",
                      ir_expression_byte_size, &context) ||
      !add_capability(caps, "bootstrap.ir-expression-string",
                      ir_expression_string, &context) ||
      !add_capability(caps, "bootstrap.copy-bytes", copy_bytes, &context) ||
      !add_capability(caps, "bootstrap.allocate-pages", allocate_pages,
                      &context) ||
      !add_capability(caps, "bootstrap.set-allocation-limit",
                      set_allocation_limit, &context) ||
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
      !add_capability(caps, "bootstrap.artifact-write-identifier",
                      artifact_write_identifier, &context) ||
      !add_capability(caps, "bootstrap.artifact-finish", artifact_finish,
                      &context) ||
      /* Backend ABI v1 uses logical names.  Keep the provider-side bootstrap
       * names above for compiler compatibility, and bind the same operations
       * under the capability names consumed by backend_c.lain. */
      !add_capability(caps, "backend.source_count", source_count, &context) ||
      !add_capability(caps, "backend.source_data", source_data, &context) ||
      !add_capability(caps, "backend.source_length", source_length, &context) ||
      !add_capability(caps, "backend.allocate", allocate_pages, &context) ||
      !add_capability(caps, "backend.copy_bytes", copy_bytes, &context) ||
      !add_capability(caps, "backend.artifact_begin", artifact_begin,
                      &context) ||
      !add_capability(caps, "backend.artifact_write_byte",
                      artifact_write_byte, &context) ||
      !add_capability(caps, "backend.artifact_finish", artifact_finish,
                      &context)) {
    fprintf(stderr, "could not initialize bootstrap capabilities\n");
    goto cleanup;
  }
  context.caps = caps;

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
  lainir_module_handle_destroy(&context.prepared_module);
  free(context.eval_values);
  lainir_caps_free(caps);
  lainir_free_subroutines(module);
  free(compiler_text);
  return exit_code;
}
