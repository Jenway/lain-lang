#include "lainir/interpreter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  char *name;
  LainirValue value;
} LainirBinding;

typedef struct {
  const char *name;
  uint32_t binding_index;
} LainirLocalIndexEntry;

typedef struct {
  uint8_t *data;
  uint32_t size;
} LainirBuffer;

typedef struct LainirFrame {
  L1Subroutine *sub;
  LainirValue *args;
  uint32_t arg_count;
  LainirValue args_inline[8];
  int args_heap;
  LainirBinding *locals;
  uint32_t local_count;
  uint32_t local_cap;
  LainirBinding locals_inline[16];
  int locals_heap;
  LainirLocalIndexEntry *local_index;
  uint32_t local_index_cap;
  LainirBuffer *allocas;
  uint32_t alloca_count;
  uint32_t alloca_cap;
  struct LainirFrame *caller;
} LainirFrame;

typedef struct LainirNestedContinuation LainirNestedContinuation;
typedef struct {
  LainirFrame *frame;
  L1Expr *expr;
  LainirValue value;
} LainirExprCacheEntry;

typedef struct LainirPendingNestedResult LainirPendingNestedResult;
struct LainirPendingNestedResult {
  LainirFrame *frame;
  L1Subroutine *target;
  LainirValue value;
  LainirPendingNestedResult *next;
};

/* A resumable root invocation owns its frame storage across calls to
 * lainir_run.  Nested frames are kept as a linked stack when an Endpoint
 * blocks, so each completed child can return a pending value to its caller. */
struct LainirNestedContinuation {
  LainirFrame *frame;
  L1Subroutine *sub;
  L1Block *block;
  L1Instruction *inst;
  LainirNestedContinuation *parent;
};

typedef struct {
  L1Subroutine *module;
  L1Subroutine *entry;
  LainirFrame frame;
  L1Block *next_block;
  L1Instruction *next_inst;
  LainirNestedContinuation *nested_top;
  LainirPendingNestedResult *pending_nested_results;
  LainirExprCacheEntry *expr_cache;
  uint32_t expr_cache_count;
  uint32_t expr_cache_capacity;
} LainirContinuation;

typedef struct {
  const char *name;
  L1Subroutine *sub;
} LainirSubIndexEntry;

typedef struct {
  L1Subroutine *sub;
  uint64_t calls;
  uint64_t steps;
} LainirTraceProcedure;

typedef struct {
  L1Subroutine *module;
  LainirSubIndexEntry *sub_index;
  uint32_t sub_index_cap;
  LainirCapabilityTable *caps;
  const char *error;
  int should_return;
  int should_break;
  int should_continue;
  LainirValue return_value;
  uint64_t steps;
  uint32_t call_depth;
  LainirFrame *active_frame;
  uint64_t allocated_bytes;
  int trace_enabled;
  int trace_live_enabled;
  int fast_builtins_enabled;
  int fast_leaves_enabled;
  int fast_scanners_enabled;
  int fast_meta_scan_enabled;
  int trace_meta_enabled;
  int trace_fast_names_enabled;
  const char *trace_call_name;
  const char *trace_return_name;
  const char *trace_path;
  LainirTraceProcedure *trace_procedures;
  uint32_t trace_procedure_count;
  LainirTraceProcedure *trace_active;
  const char *fast_active_name;
  LainirVmControl *vm_control;
  uint64_t vm_owner;
  int vm_slice_yielded;
  int vm_blocked;
  uint32_t current_line;
  uint32_t current_column;
  uint64_t current_source_start;
  uint64_t current_source_end;
  LainirContinuation *continuation;
  int continuation_tracking;
  L1Block *active_block;
  L1Instruction *active_inst;
  int instruction_boundary;
  uint64_t trace_expr_kinds[64];
  uint64_t trace_inst_kinds[16];
  uint64_t trace_meta_cache_hits;
  uint64_t trace_meta_cache_misses;
} LainirInterpreter;

/* ── forward declarations for mutual recursion ── */
static LainirValue interp_eval_expr(LainirInterpreter *, LainirFrame *, L1Expr *);
static void interp_destroy_continuation(LainirContinuation *continuation);
static void interp_exec_block(LainirInterpreter *, LainirFrame *, L1Block *);
static LainirValue interp_call_sub(LainirInterpreter *, L1Subroutine *,
                                    const LainirValue *, uint32_t);
static LainirValue interp_call_root_resumable(
    LainirInterpreter *, L1Subroutine *, const LainirValue *, uint32_t);
static void interp_trap(LainirInterpreter *, const char *);
static void interp_trace_finish(LainirInterpreter *);
static int interp_try_fast_builtin(LainirInterpreter *, L1Subroutine *,
                                    const LainirValue *, uint32_t,
                                    LainirValue *);

/* ═══════════════════════════════════════════════════════════════
 * Value constructors
 * ═══════════════════════════════════════════════════════════════ */

LainirValue lainir_value_unit(void) {
  LainirValue value; memset(&value, 0, sizeof(value));
  value.kind = LAINIR_VALUE_UNIT; return value;
}

LainirValue lainir_value_bits(uint64_t bits, uint32_t bit_width) {
  LainirValue value; memset(&value, 0, sizeof(value));
  value.kind = LAINIR_VALUE_BITS;
  value.bit_width = bit_width ? bit_width : 64;
  if (value.bit_width < 64)
    bits &= (UINT64_C(1) << value.bit_width) - 1;
  value.as.bits = bits; return value;
}

LainirValue lainir_value_addr(void *addr) {
  LainirValue value; memset(&value, 0, sizeof(value));
  value.kind = LAINIR_VALUE_ADDR; value.as.addr = addr; return value;
}

LainirValue lainir_value_string(const char *string) {
  LainirValue value; memset(&value, 0, sizeof(value));
  value.kind = LAINIR_VALUE_STRING; value.as.string = string; return value;
}

LainirValue lainir_value_func(L1Subroutine *func) {
  LainirValue value; memset(&value, 0, sizeof(value));
  value.kind = LAINIR_VALUE_FUNC; value.as.func = func; return value;
}

/* ═══════════════════════════════════════════════════════════════
 * Capability table
 * ═══════════════════════════════════════════════════════════════ */

LainirCapabilityTable *lainir_caps_new(void) {
  return calloc(1, sizeof(LainirCapabilityTable));
}

void lainir_caps_free(LainirCapabilityTable *caps) {
  if (!caps) return;
  free(caps->entries);
  free(caps);
}

int lainir_caps_add(LainirCapabilityTable *caps, const char *name,
                    LainirHostFn fn, void *user_data) {
  if (!caps || !name || !fn) return 0;
  if (caps->count == caps->cap) {
    uint32_t next_cap = caps->cap ? caps->cap * 2 : 8;
    LainirCapabilityEntry *next = realloc(caps->entries, sizeof(LainirCapabilityEntry) * next_cap);
    if (!next) return 0;
    caps->entries = next; caps->cap = next_cap;
  }
  caps->entries[caps->count].name = name;
  caps->entries[caps->count].fn = fn;
  caps->entries[caps->count].user_data = user_data;
  caps->count++;
  return 1;
}

void lainir_caps_set_limits(LainirCapabilityTable *caps,
                            uint64_t max_steps,
                            uint32_t max_call_depth,
                            uint64_t max_alloc_bytes) {
  if (!caps) return;
  caps->max_steps = max_steps;
  caps->max_call_depth = max_call_depth;
  caps->max_alloc_bytes = max_alloc_bytes;
}

void lainir_caps_set_eval_limit(LainirCapabilityTable *caps,
                                uint64_t max_eval_blocks) {
  if (!caps) return;
  caps->max_eval_blocks = max_eval_blocks;
}

/* ═══════════════════════════════════════════════════════════════
 * Helpers
 * ═══════════════════════════════════════════════════════════════ */

static uint32_t interp_type_bits(L1Type *ty) {
  if (!ty) return 32;
  if (ty->kind == TY_BITS && ty->width) return ty->width;
  return 64;
}

static uint32_t interp_type_size(L1Type *ty) {
  if (!ty) return 4;
  switch (ty->kind) {
  case TY_BITS: return ty->width <= 8 ? 1 : ty->width <= 16 ? 2 : ty->width <= 32 ? 4 : 8;
  case TY_ADDR: return (uint32_t)sizeof(void *);
  case TY_UNIT: return 0;
  default:      return 8;
  }
}

static L1Subroutine *interp_find_sub_linear(L1Subroutine *head,
                                            const char *name) {
  for (L1Subroutine *sub = head; sub; sub = sub->next) {
    if (sub->is_data) continue;
    if (strcmp(sub->name, name) == 0) return sub;
    if (sub->link_name && strcmp(sub->link_name, name) == 0) return sub;
  }
  return NULL;
}

static uint32_t interp_hash_name(const char *name) {
  uint32_t hash = 2166136261u;
  const unsigned char *p = (const unsigned char *)name;
  while (*p) {
    hash ^= *p++;
    hash *= 16777619u;
  }
  return hash;
}

static void interp_sub_index_insert(LainirInterpreter *interp,
                                    const char *name,
                                    L1Subroutine *sub) {
  if (!interp->sub_index || !name) return;
  uint32_t slot = interp_hash_name(name) & (interp->sub_index_cap - 1);
  while (interp->sub_index[slot].name) {
    /* Preserve the linked-list lookup contract: the first matching symbol
     * wins when malformed/unverified input contains duplicate names. */
    if (strcmp(interp->sub_index[slot].name, name) == 0) return;
    slot = (slot + 1) & (interp->sub_index_cap - 1);
  }
  interp->sub_index[slot].name = name;
  interp->sub_index[slot].sub = sub;
}

static void interp_build_sub_index(LainirInterpreter *interp) {
  uint32_t key_count = 0;
  uint32_t cap = 16;
  for (L1Subroutine *sub = interp->module; sub; sub = sub->next) {
    key_count++;
    if (sub->link_name) key_count++;
  }
  while (cap < key_count * 2) cap *= 2;
  interp->sub_index = calloc(cap, sizeof(LainirSubIndexEntry));
  if (!interp->sub_index) return;
  interp->sub_index_cap = cap;
  for (L1Subroutine *sub = interp->module; sub; sub = sub->next) {
    interp_sub_index_insert(interp, sub->name, sub);
    if (sub->link_name) interp_sub_index_insert(interp, sub->link_name, sub);
  }
}

static L1Subroutine *interp_find_sub(LainirInterpreter *interp,
                                     const char *name) {
  if (!interp->sub_index || !name)
    return interp_find_sub_linear(interp->module, name);
  uint32_t slot = interp_hash_name(name) & (interp->sub_index_cap - 1);
  while (interp->sub_index[slot].name) {
    if (strcmp(interp->sub_index[slot].name, name) == 0)
      return interp->sub_index[slot].sub;
    slot = (slot + 1) & (interp->sub_index_cap - 1);
  }
  return NULL;
}

static int interp_tick(LainirInterpreter *interp) {
  if (interp->vm_control && interp->instruction_boundary &&
      interp->call_depth <= 1 &&
      !lainir_vm_control_consume_step(interp->vm_control, interp->vm_owner)) {
    if (lainir_vm_control_slice_exhausted(interp->vm_control)) {
      interp->vm_slice_yielded = 1;
      return 0;
    }
    interp_trap(interp, "VM control step rejected");
    return 0;
  }
  uint64_t limit = interp->caps ? interp->caps->max_steps : 0;
  if (limit && interp->steps >= limit) {
    interp_trap(interp, "interpreter step limit exceeded");
    return 0;
  }
  interp->steps++;
  if (interp->trace_active) interp->trace_active->steps++;
  if (interp->trace_live_enabled && (interp->steps % 100) == 0) {
    const char *name = "<unknown>";
    if (interp->active_frame && interp->active_frame->sub &&
        interp->active_frame->sub->name)
      name = interp->active_frame->sub->name;
    fprintf(stderr, "lainir live: step=%llu procedure=%s\\n",
            (unsigned long long)interp->steps, name);
    fflush(stderr);
  }
  return 1;
}

static void interp_trace_init(LainirInterpreter *interp) {
  const char *path = getenv("LAINIR_TRACE_PROFILE");
  uint32_t count = 0;
  if (!path || !path[0]) return;
  for (L1Subroutine *sub = interp->module; sub; sub = sub->next) count++;
  interp->trace_procedures = calloc(count, sizeof(LainirTraceProcedure));
  if (!interp->trace_procedures) return;
  interp->trace_enabled = 1;
  interp->trace_path = path;
  interp->trace_procedure_count = count;
  uint32_t index = 0;
  for (L1Subroutine *sub = interp->module; sub; sub = sub->next) {
    interp->trace_procedures[index++].sub = sub;
  }
}

static LainirTraceProcedure *interp_trace_procedure(
    LainirInterpreter *interp, L1Subroutine *sub) {
  if (!interp->trace_enabled) return NULL;
  for (uint32_t i = 0; i < interp->trace_procedure_count; i++)
    if (interp->trace_procedures[i].sub == sub)
      return &interp->trace_procedures[i];
  return NULL;
}

static L1Subroutine *interp_find_data(LainirInterpreter *interp,
                                      const char *name) {
  for (L1Subroutine *item = interp->module; item; item = item->next)
    if (item->is_data && item->name && name && strcmp(item->name, name) == 0)
      return item;
  return NULL;
}

static int interp_addr_is_readonly_data(LainirInterpreter *interp,
                                        const void *address) {
  uintptr_t value = (uintptr_t)address;
  for (L1Subroutine *item = interp->module; item; item = item->next) {
    uintptr_t begin;
    if (!item->is_data || !item->data_bytes) continue;
    begin = (uintptr_t)item->data_bytes;
    if (value >= begin && value < begin + item->data_size) return 1;
  }
  return 0;
}

/* Validate accesses into interpreter-owned activation storage.  Host-owned
 * capability buffers remain opaque to the interpreter; accesses into an
 * active #alloca are checked against the complete allocation range. */
static int interp_validate_memory_access(
    LainirInterpreter *interp, LainirFrame *frame, const void *address,
    uint32_t width, int write) {
  uintptr_t value = (uintptr_t)address;
  if (!address) {
    interp_trap(interp, write ? "store to null" : "load from null");
    return 0;
  }
  for (LainirFrame *owner = frame; owner; owner = owner->caller) {
    for (uint32_t i = 0; i < owner->alloca_count; i++) {
      uintptr_t begin = (uintptr_t)owner->allocas[i].data;
      uintptr_t end = begin + (owner->allocas[i].size ? owner->allocas[i].size : 1);
      if (value >= begin && value <= end) {
        if (value == end || width > end - value) {
          interp_trap(interp, "memory access out of bounds");
          return 0;
        }
        return 1;
      }
    }
  }
  /* #data write protection is handled by interp_addr_is_readonly_data at the
   * store site.  Do not scan the complete module for every load: compiler
   * Meta walks perform millions of host-owned reads, which must stay on the
   * existing opaque capability path. */
  return 1;
}

static int interp_trace_compare_steps(const void *left, const void *right) {
  const LainirTraceProcedure *const *a = left;
  const LainirTraceProcedure *const *b = right;
  if ((*a)->steps < (*b)->steps) return 1;
  if ((*a)->steps > (*b)->steps) return -1;
  return 0;
}

static void interp_trace_finish(LainirInterpreter *interp) {
  static const char *inst_names[] = {
      "let", "set", "store", "if", "loop", "break", "continue",
      "return", "call"};
  static const char *expr_names[] = {
      "var", "const", "arg", "load", "lea", "add", "sub", "mul",
      "eq", "ne", "popcount", "clz", "rotl", "int2ptr", "ptr2int",
      "fadd", "fsub", "fmul", "fdiv", "feq", "flt", "call",
      "call_indirect", "string", "alloca", "eval", "sdiv", "udiv", "slt", "sle", "sgt",
      "sge", "ult", "ule", "ugt", "uge", "zext", "sext", "trunc",
      "proc_addr", "bitcast"};
  FILE *out;
  LainirTraceProcedure **ranked;
  if (!interp->trace_enabled) return;
  out = strcmp(interp->trace_path, "-") == 0
            ? stderr : fopen(interp->trace_path, "w");
  if (!out) out = stderr;
  fprintf(out, "section\tname\tcalls\tsteps\n");
  ranked = calloc(interp->trace_procedure_count, sizeof(*ranked));
  if (ranked) {
    for (uint32_t i = 0; i < interp->trace_procedure_count; i++)
      ranked[i] = &interp->trace_procedures[i];
    qsort(ranked, interp->trace_procedure_count, sizeof(*ranked),
          interp_trace_compare_steps);
    for (uint32_t i = 0; i < interp->trace_procedure_count; i++) {
      LainirTraceProcedure *entry = ranked[i];
      if (!entry->calls && !entry->steps) continue;
      fprintf(out, "procedure\t%s\t%llu\t%llu\n", entry->sub->name,
              (unsigned long long)entry->calls,
              (unsigned long long)entry->steps);
    }
    free(ranked);
  }
  for (uint32_t i = 0; i < sizeof(inst_names) / sizeof(inst_names[0]); i++)
    if (interp->trace_inst_kinds[i])
      fprintf(out, "instruction\t%s\t%llu\t0\n", inst_names[i],
              (unsigned long long)interp->trace_inst_kinds[i]);
  for (uint32_t i = 0; i < sizeof(expr_names) / sizeof(expr_names[0]); i++)
    if (interp->trace_expr_kinds[i])
      fprintf(out, "expression\t%s\t%llu\t0\n", expr_names[i],
              (unsigned long long)interp->trace_expr_kinds[i]);
  fprintf(out, "cache\tmeta_lookup_hits\t%llu\t0\n",
          (unsigned long long)interp->trace_meta_cache_hits);
  fprintf(out, "cache\tmeta_lookup_misses\t%llu\t0\n",
          (unsigned long long)interp->trace_meta_cache_misses);
  fprintf(out, "summary\ttotal\t0\t%llu\n",
          (unsigned long long)interp->steps);
  if (out != stderr) fclose(out);
  free(interp->trace_procedures);
  interp->trace_procedures = NULL;
  interp->trace_enabled = 0;
}

static void interp_local_index_insert(LainirFrame *frame,
                                      const char *name,
                                      uint32_t binding_index) {
  if (!frame->local_index || !name) return;
  uint32_t slot = interp_hash_name(name) & (frame->local_index_cap - 1);
  while (frame->local_index[slot].name) {
    /* Keep the first binding, matching the old linear lookup behavior if a
     * malformed module emits duplicate `let` names. */
    if (strcmp(frame->local_index[slot].name, name) == 0) return;
    slot = (slot + 1) & (frame->local_index_cap - 1);
  }
  frame->local_index[slot].name = name;
  frame->local_index[slot].binding_index = binding_index;
}

static int interp_local_index_rebuild(LainirFrame *frame, uint32_t cap) {
  LainirLocalIndexEntry *index = calloc(cap, sizeof(*index));
  if (!index) return 0;
  free(frame->local_index);
  frame->local_index = index;
  frame->local_index_cap = cap;
  for (uint32_t i = 0; i < frame->local_count; i++)
    interp_local_index_insert(frame, frame->locals[i].name, i);
  return 1;
}

static LainirBinding *interp_lookup_local(LainirFrame *frame, const char *name) {
  if (frame->local_index && frame->local_index_cap && name) {
    uint32_t slot = interp_hash_name(name) & (frame->local_index_cap - 1);
    while (frame->local_index[slot].name) {
      if (strcmp(frame->local_index[slot].name, name) == 0)
        return &frame->locals[frame->local_index[slot].binding_index];
      slot = (slot + 1) & (frame->local_index_cap - 1);
    }
    return NULL;
  }
  for (uint32_t i = 0; i < frame->local_count; i++)
    if (strcmp(frame->locals[i].name, name) == 0) return &frame->locals[i];
  return NULL;
}

static int interp_set_local(LainirFrame *frame, const char *name, LainirValue value) {
  LainirBinding *b = interp_lookup_local(frame, name);
  if (b) { b->value = value; return 1; }
  if (frame->local_count == frame->local_cap) {
    uint32_t nc = frame->local_cap ? frame->local_cap * 2 : 8;
    LainirBinding *nl;
    int was_heap = frame->locals_heap;
    if (frame->locals_heap) {
      nl = realloc(frame->locals, sizeof(LainirBinding) * nc);
    } else {
      nl = malloc(sizeof(LainirBinding) * nc);
      if (nl && frame->local_count)
        memcpy(nl, frame->locals,
               sizeof(LainirBinding) * frame->local_count);
    }
    if (!nl) return 0;
    /* realloc already owns and, when necessary, releases the old heap
     * block.  Freeing frame->locals again here corrupts the heap after the
     * second local-capacity growth; only the inline-to-heap transition needs
     * a flag change. */
    if (!was_heap) frame->locals_heap = 1;
    frame->locals = nl; frame->local_cap = nc;
  }
  frame->locals[frame->local_count].name = strdup(name);
  if (!frame->locals[frame->local_count].name) return 0;
  frame->locals[frame->local_count].value = value;
  frame->local_count++;
  if (!frame->local_index ||
      (frame->local_count + 1) * 10 >= frame->local_index_cap * 7) {
    uint32_t cap = frame->local_index_cap ? frame->local_index_cap * 2 : 16;
    while (cap < frame->local_count * 2) cap *= 2;
    if (!interp_local_index_rebuild(frame, cap)) {
      /* The linear path remains correct if the optional accelerator cannot
       * allocate memory.  Drop a stale partial index so future lookups do
       * not miss the binding just appended above. */
      free(frame->local_index);
      frame->local_index = NULL;
      frame->local_index_cap = 0;
    }
  } else {
    interp_local_index_insert(
        frame, frame->locals[frame->local_count - 1].name,
        frame->local_count - 1);
  }
  return 1;
}

static void interp_free_frame(LainirFrame *frame) {
  if (!frame) return;
  for (uint32_t i = 0; i < frame->alloca_count; i++)
    free(frame->allocas[i].data);
  free(frame->allocas);
  for (uint32_t i = 0; i < frame->local_count; i++) free(frame->locals[i].name);
  if (frame->locals_heap) free(frame->locals);
  free(frame->local_index);
  if (frame->args_heap) free(frame->args);
}

static int interp_frame_owns_addr(const LainirFrame *frame, const void *address) {
  uintptr_t value = (uintptr_t)address;
  if (!frame || !address) return 0;
  for (uint32_t i = 0; i < frame->alloca_count; i++) {
    uintptr_t begin = (uintptr_t)frame->allocas[i].data;
    uintptr_t end = begin + (frame->allocas[i].size ? frame->allocas[i].size : 1);
    if (value >= begin && value < end) return 1;
  }
  return 0;
}

static LainirCapabilityEntry *interp_lookup_cap(LainirCapabilityTable *caps, const char *name) {
  if (!caps || !name) return NULL;
  for (uint32_t i = 0; i < caps->count; i++)
    if (strcmp(caps->entries[i].name, name) == 0) return &caps->entries[i];
  return NULL;
}

static int interp_take_pending_nested(LainirInterpreter *interp,
                                      LainirFrame *frame,
                                      L1Subroutine *target,
                                      LainirValue *value_out) {
  LainirContinuation *continuation = interp->continuation;
  LainirPendingNestedResult **link;
  if (!continuation) return 0;
  link = &continuation->pending_nested_results;
  while (*link) {
    LainirPendingNestedResult *pending = *link;
    if (pending->frame == frame && pending->target == target) {
      *link = pending->next;
      if (value_out) *value_out = pending->value;
      free(pending);
      return 1;
    }
    link = &pending->next;
  }
  return 0;
}

static int interp_queue_pending_nested(LainirInterpreter *interp,
                                       LainirFrame *frame,
                                       L1Subroutine *target,
                                       LainirValue value) {
  LainirContinuation *continuation = interp->continuation;
  LainirPendingNestedResult *pending;
  if (!continuation) return 0;
  pending = calloc(1, sizeof(*pending));
  if (!pending) return 0;
  pending->frame = frame;
  pending->target = target;
  pending->value = value;
  pending->next = continuation->pending_nested_results;
  continuation->pending_nested_results = pending;
  return 1;
}

static void interp_trap(LainirInterpreter *interp, const char *error) {
  if (!interp->error) interp->error = error;
  if (interp->vm_control)
    (void)lainir_vm_control_record_trap_span(
        interp->vm_control, interp->vm_owner,
        LAINIR_VM_TRAP_INTERPRETER, LAINIR_RUN_TRAP,
        interp->current_line, interp->current_column,
        interp->current_source_start, interp->current_source_end);
  if (getenv("LAINIR_TRACE_FAST") && interp->fast_active_name)
    fprintf(stderr, "lainir fast trap: %s (%s)\n",
            interp->fast_active_name, error);
  if (getenv("LAINIR_TRACE_TRAP") && interp->active_frame) {
    const char *name = "<unknown>";
    if (interp->active_frame->sub && interp->active_frame->sub->name)
      name = interp->active_frame->sub->name;
    fprintf(stderr, "lainir trap: %s in %s", error, name);
    for (LainirFrame *caller = interp->active_frame->caller;
         caller; caller = caller->caller) {
      if (caller->sub && caller->sub->name)
        fprintf(stderr, " caller=%s", caller->sub->name);
    }
    fputc('\n', stderr);
  }
}

static uint64_t interp_value_bits(LainirInterpreter *interp, LainirValue value, const char *ctx) {
  if (value.kind != LAINIR_VALUE_BITS) {
    if (getenv("LAINIR_TRACE_TRAP")) {
      const char *name = "<unknown>";
      if (interp->active_frame && interp->active_frame->sub &&
          interp->active_frame->sub->name)
        name = interp->active_frame->sub->name;
      fprintf(stderr, "lainir trap: %s kind=%d in %s", ctx,
              (int)value.kind, name);
      for (LainirFrame *caller = interp->active_frame ?
               interp->active_frame->caller : NULL;
           caller; caller = caller->caller) {
        if (caller->sub && caller->sub->name)
          fprintf(stderr, " caller=%s", caller->sub->name);
      }
      fputc('\n', stderr);
    }
    interp_trap(interp, ctx); return 0;
  }
  return value.as.bits;
}

static int interp_values_equal(LainirInterpreter *interp,
                               LainirValue left, LainirValue right) {
  if (left.kind != right.kind) {
    if (getenv("LAINIR_TRACE_TRAP")) {
      const char *name = "<unknown>";
      if (interp->active_frame && interp->active_frame->sub &&
          interp->active_frame->sub->name)
        name = interp->active_frame->sub->name;
      fprintf(stderr, "lainir trap: equality kind mismatch in %s left=%d right=%d",
              name, (int)left.kind, (int)right.kind);
      for (LainirFrame *caller = interp->active_frame ? interp->active_frame->caller : NULL;
           caller; caller = caller->caller) {
        if (caller->sub && caller->sub->name)
          fprintf(stderr, " caller=%s", caller->sub->name);
      }
      fputc('\n', stderr);
    }
    interp_trap(interp, "equality operands have different physical kinds");
    return 0;
  }
  switch (left.kind) {
  case LAINIR_VALUE_UNIT:
    return 1;
  case LAINIR_VALUE_BITS:
    return left.as.bits == right.as.bits;
  case LAINIR_VALUE_ADDR:
    return left.as.addr == right.as.addr;
  case LAINIR_VALUE_STRING:
    return left.as.string == right.as.string;
  case LAINIR_VALUE_FUNC:
    return left.as.func == right.as.func;
  }
  interp_trap(interp, "invalid equality operand");
  return 0;
}

static double interp_value_float(LainirInterpreter *interp, LainirValue value,
                                 const char *ctx) {
  if (value.kind != LAINIR_VALUE_BITS ||
      (value.bit_width != 32 && value.bit_width != 64)) {
    interp_trap(interp, ctx);
    return 0.0;
  }
  if (value.bit_width == 32) {
    uint32_t bits = (uint32_t)value.as.bits;
    float result;
    memcpy(&result, &bits, sizeof(result));
    return (double)result;
  }
  {
    double result;
    uint64_t bits = value.as.bits;
    memcpy(&result, &bits, sizeof(result));
    return result;
  }
}

static LainirValue interp_make_float(double value, uint32_t width) {
  if (width == 32) {
    float narrowed = (float)value;
    uint32_t bits;
    memcpy(&bits, &narrowed, sizeof(bits));
    return lainir_value_bits(bits, 32);
  }
  {
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return lainir_value_bits(bits, 64);
  }
}

static int64_t interp_signed_bits(uint64_t bits, uint32_t width) {
  if (!width || width >= 64) return (int64_t)bits;
  {
    uint64_t sign = UINT64_C(1) << (width - 1);
    uint64_t mask = (UINT64_C(1) << width) - 1;
    bits &= mask;
    return (int64_t)((bits ^ sign) - sign);
  }
}

static LainirValue interp_coerce_physical(
    LainirInterpreter *interp, LainirValue value, L1Type *type) {
  if (!type) return value;
  if (type->kind == TY_BITS) {
    if (value.kind != LAINIR_VALUE_BITS) {
      interp_trap(interp, "expected bits for physical coercion");
      return lainir_value_unit();
    }
    return lainir_value_bits(value.as.bits, type->width);
  }
  if (type->kind == TY_ADDR) {
    /* Source-level string literals are interned as a distinct physical
     * value kind, but an `addr` parameter consumes their byte storage.  Keep
     * the conversion at the call/let boundary so pointer arithmetic and
     * loads see the same address representation as an explicit address. */
    if (value.kind == LAINIR_VALUE_STRING)
      return lainir_value_addr((void *)value.as.string);
    return value;
  }
  if (type->kind == TY_UNIT) return lainir_value_unit();
  return value;
}

static LainirValue interp_eval_explicit_integer_binary(
    LainirInterpreter *interp, LainirFrame *frame, L1Expr *expr) {
  LainirValue left = interp_eval_expr(interp, frame, expr->data.bin.left);
  LainirValue right = interp_eval_expr(interp, frame, expr->data.bin.right);
  uint32_t width = left.bit_width ? left.bit_width : 32;
  uint64_t lhs;
  uint64_t rhs;
  int64_t signed_lhs;
  int64_t signed_rhs;
  if (interp->error) return lainir_value_unit();
  lhs = interp_value_bits(interp, left, "expected bits for integer operation");
  rhs = interp_value_bits(interp, right, "expected bits for integer operation");
  signed_lhs = interp_signed_bits(lhs, width);
  signed_rhs = interp_signed_bits(rhs, width);
  switch (expr->kind) {
  case EXPR_SDIV:
    if (!signed_rhs) {
      interp_trap(interp, "sdiv by zero");
      return lainir_value_unit();
    }
    if (width == 64 && signed_lhs == INT64_MIN && signed_rhs == -1)
      return lainir_value_bits((uint64_t)INT64_MIN, width);
    return lainir_value_bits((uint64_t)(signed_lhs / signed_rhs), width);
  case EXPR_UDIV:
    if (!rhs) {
      interp_trap(interp, "udiv by zero");
      return lainir_value_unit();
    }
    return lainir_value_bits(lhs / rhs, width);
  case EXPR_SLT: return lainir_value_bits(signed_lhs < signed_rhs, 1);
  case EXPR_SLE: return lainir_value_bits(signed_lhs <= signed_rhs, 1);
  case EXPR_SGT: return lainir_value_bits(signed_lhs > signed_rhs, 1);
  case EXPR_SGE: return lainir_value_bits(signed_lhs >= signed_rhs, 1);
  case EXPR_ULT: return lainir_value_bits(lhs < rhs, 1);
  case EXPR_ULE: return lainir_value_bits(lhs <= rhs, 1);
  case EXPR_UGT: return lainir_value_bits(lhs > rhs, 1);
  case EXPR_UGE: return lainir_value_bits(lhs >= rhs, 1);
  default:
    interp_trap(interp, "invalid explicit integer operation");
    return lainir_value_unit();
  }
}

static void *interp_value_addr(LainirInterpreter *interp, LainirValue value, const char *ctx) {
  if (value.kind == LAINIR_VALUE_ADDR) {
    if (value.as.addr) return value.as.addr;
    /* Keep the normal trap text stable, but expose the active procedure when
     * explicitly requested.  This is useful for diagnosing generated L1
     * layouts without making ordinary compiler runs noisy. */
    if (getenv("LAINIR_TRACE_TRAP")) {
      const char *name = "<unknown>";
      if (interp->active_frame && interp->active_frame->sub &&
          interp->active_frame->sub->name)
        name = interp->active_frame->sub->name;
      fprintf(stderr, "lainir trap: null address in %s", name);
      if (interp->active_frame && interp->active_frame->arg_count) {
        fprintf(stderr, " args=");
        for (uint32_t i = 0; i < interp->active_frame->arg_count; i++) {
          LainirValue arg = interp->active_frame->args[i];
          if (i) fputc(',', stderr);
          if (arg.kind == LAINIR_VALUE_ADDR)
            fprintf(stderr, "addr:%p", arg.as.addr);
          else if (arg.kind == LAINIR_VALUE_BITS)
            fprintf(stderr, "bits:%llu", (unsigned long long)arg.as.bits);
          else
            fprintf(stderr, "kind:%d", (int)arg.kind);
        }
      }
      for (LainirFrame *caller = interp->active_frame ? interp->active_frame->caller : NULL;
           caller; caller = caller->caller) {
        if (caller->sub && caller->sub->name)
          fprintf(stderr, " caller=%s", caller->sub->name);
      }
      fputc('\n', stderr);
    }
    interp_trap(interp, "null address");
    return NULL;
  }
  if (value.kind == LAINIR_VALUE_STRING) return (void *)value.as.string;
  if (getenv("LAINIR_TRACE_TRAP")) {
    const char *name = "<unknown>";
    if (interp->active_frame && interp->active_frame->sub &&
        interp->active_frame->sub->name)
      name = interp->active_frame->sub->name;
    fprintf(stderr, "lainir trap: %s kind=%d in %s\n", ctx,
            (int)value.kind, name);
  }
  interp_trap(interp, ctx); return NULL;
}

static uint32_t interp_store_width(L1Type *ty, LainirValue value) {
  if (ty) return interp_type_size(ty);
  if (value.kind == LAINIR_VALUE_BITS) {
    if (value.bit_width && value.bit_width <= 8) return 1;
    if (value.bit_width && value.bit_width <= 16) return 2;
    if (value.bit_width && value.bit_width <= 32) return 4;
    return 8;
  }
  /* Text L1 currently omits the store type, so the interpreter must preserve
   * the complete physical representation of pointer-like runtime values.
   * Writing the historical four-byte fallback truncates addresses on 64-bit
   * hosts and corrupts every nested aggregate returned across a call. */
  if (value.kind == LAINIR_VALUE_ADDR ||
      value.kind == LAINIR_VALUE_STRING ||
      value.kind == LAINIR_VALUE_FUNC)
    return (uint32_t)sizeof(void *);
  return 4;
}

static LainirValue interp_load_bits(LainirInterpreter *interp, void *addr,
                                     uint32_t size, uint32_t bit_width) {
  uint64_t bits = 0;
  if (!addr) { interp_trap(interp, "load from null"); return lainir_value_unit(); }
  memcpy(&bits, addr, size);
  return lainir_value_bits(bits, bit_width ? bit_width : size * 8);
}

static LainirValue interp_load_typed(
    LainirInterpreter *interp, void *addr, L1Type *ty) {
  if (!addr) {
    interp_trap(interp, "load from null");
    return lainir_value_unit();
  }
  if (ty && ty->kind == TY_ADDR) {
    void *value = NULL;
    memcpy(&value, addr, sizeof(value));
    return lainir_value_addr(value);
  }
  return interp_load_bits(
      interp, addr, interp_type_size(ty), interp_type_bits(ty));
}

/* ═══════════════════════════════════════════════════════════════
 * Host call
 * ═══════════════════════════════════════════════════════════════ */

static LainirValue interp_call_host(LainirInterpreter *interp, const char *name,
                                     LainirValue *args, uint32_t arg_count) {
  LainirCapabilityEntry *entry = interp_lookup_cap(interp->caps, name);
  LainirValue result = lainir_value_unit();
  const char *error = NULL;
  if (!entry) { interp_trap(interp, "extern capability not found"); return result; }
  LainirRunStatus status =
      entry->fn(args, arg_count, &result, &error, entry->user_data);
  if (status == LAINIR_RUN_BLOCKED) {
    interp->vm_blocked = 1;
    return result;
  }
  if (status != LAINIR_RUN_OK) {
    if (interp->vm_control)
      (void)lainir_vm_control_record_trap_span(
          interp->vm_control, interp->vm_owner,
          LAINIR_VM_TRAP_CAPABILITY, status,
          interp->current_line, interp->current_column,
          interp->current_source_start, interp->current_source_end);
    interp_trap(interp, error ? error : "extern capability call failed");
  }
  return result;
}

/* ═══════════════════════════════════════════════════════════════
 * Expression evaluator
 * ═══════════════════════════════════════════════════════════════ */

static LainirValue interp_eval_call(LainirInterpreter *interp, LainirFrame *frame,
                                     L1Expr *expr) {
  L1Subroutine *sub = interp_find_sub(interp, expr->data.call.fn_name);
  LainirValue args_inline[8];
  LainirValue *args = args_inline;
  int args_heap = 0;
  int endpoint_resume = 0;
  LainirValue result = lainir_value_unit();
  if (sub && interp_take_pending_nested(interp, frame, sub, &result)) {
    return result;
  }
  if (expr->data.call.arg_count > sizeof(args_inline) / sizeof(args_inline[0])) {
    args = calloc(expr->data.call.arg_count, sizeof(LainirValue));
    args_heap = 1;
    if (!args) { interp_trap(interp, "out of memory"); return result; }
  }
  if (interp->vm_control && sub && sub->is_extern &&
      (!strcmp(expr->data.call.fn_name, "endpoint.send") ||
       !strcmp(expr->data.call.fn_name, "endpoint.receive"))) {
    uint32_t pending_kind = 0;
    endpoint_resume = lainir_vm_control_has_endpoint_result(
        interp->vm_control, interp->vm_owner, &pending_kind);
    (void)pending_kind;
  }
  if (!endpoint_resume) {
    for (uint32_t i = 0; i < expr->data.call.arg_count; i++) {
      args[i] = interp_eval_expr(interp, frame, expr->data.call.args[i]);
      if (interp->error) { if (args_heap) free(args); return result; }
    }
  } else if (!strcmp(expr->data.call.fn_name, "endpoint.send")) {
    /* The capability consumes the pending result before inspecting payload;
     * provide a typed placeholder so a resumed send does not re-evaluate its
     * argument expression. */
    args[0] = lainir_value_bits(0, 64);
  }
  if (!sub) { if (args_heap) free(args); interp_trap(interp, "call target not found"); return result; }
  if (sub->is_extern && !sub->blocks) {
    result = interp_call_host(interp, expr->data.call.fn_name, args, expr->data.call.arg_count);
    if (args_heap) free(args); return result;
  }
  L1Block *saved_active_block = interp->active_block;
  L1Instruction *saved_active_inst = interp->active_inst;
  result = interp_call_sub(interp, sub, args, expr->data.call.arg_count);
  interp->active_block = saved_active_block;
  interp->active_inst = saved_active_inst;
  if (args_heap) free(args);
  return result;
}

/* Evaluate a compile-time block in the current frame.  The compiler owns the
 * decision to invoke the interpreter for compile-time work; once here, an
 * eval block is just another structured call frame. */
static LainirValue interp_eval_block(LainirInterpreter *interp,
                                      LainirFrame *frame, L1Block *block) {
  int saved_return = interp->should_return;
  int saved_break = interp->should_break;
  int saved_continue = interp->should_continue;
  LainirValue saved_value = interp->return_value;
  LainirValue result = lainir_value_unit();
  interp->should_return = 0;
  interp->should_break = 0;
  interp->should_continue = 0;
  interp_exec_block(interp, frame, block);
  if (!interp->error && interp->should_return)
    result = interp->return_value;
  interp->should_return = saved_return;
  interp->should_break = saved_break;
  interp->should_continue = saved_continue;
  interp->return_value = saved_value;
  return result;
}

static int interp_expr_cache_lookup(LainirInterpreter *interp,
                                    LainirFrame *frame, L1Expr *expr,
                                    LainirValue *value_out) {
  LainirContinuation *continuation = interp->continuation;
  if (!continuation) return 0;
  for (uint32_t i = 0; i < continuation->expr_cache_count; i++) {
    LainirExprCacheEntry *entry = &continuation->expr_cache[i];
    if (entry->frame == frame && entry->expr == expr) {
      if (value_out) *value_out = entry->value;
      return 1;
    }
  }
  return 0;
}

static void interp_expr_cache_store(LainirInterpreter *interp,
                                    LainirFrame *frame, L1Expr *expr,
                                    LainirValue value) {
  LainirContinuation *continuation = interp->continuation;
  if (!continuation || interp->error || interp->vm_blocked ||
      interp->vm_slice_yielded)
    return;
  if (continuation->expr_cache_count == continuation->expr_cache_capacity) {
    uint32_t next = continuation->expr_cache_capacity
        ? continuation->expr_cache_capacity * 2 : 16;
    LainirExprCacheEntry *entries = realloc(
        continuation->expr_cache, next * sizeof(*entries));
    if (!entries) return;
    continuation->expr_cache = entries;
    continuation->expr_cache_capacity = next;
  }
  continuation->expr_cache[continuation->expr_cache_count++] =
      (LainirExprCacheEntry){frame, expr, value};
}

static void interp_expr_cache_clear_frame(LainirInterpreter *interp,
                                          LainirFrame *frame) {
  LainirContinuation *continuation = interp->continuation;
  if (!continuation) return;
  uint32_t write = 0;
  for (uint32_t i = 0; i < continuation->expr_cache_count; i++) {
    if (continuation->expr_cache[i].frame != frame)
      continuation->expr_cache[write++] = continuation->expr_cache[i];
  }
  continuation->expr_cache_count = write;
}

static void interp_expr_cache_rekey(LainirInterpreter *interp,
                                    LainirFrame *old_frame,
                                    LainirFrame *new_frame) {
  LainirContinuation *continuation = interp->continuation;
  if (!continuation) return;
  for (LainirPendingNestedResult *pending = continuation->pending_nested_results;
       pending; pending = pending->next)
    if (pending->frame == old_frame) pending->frame = new_frame;
  for (uint32_t i = 0; i < continuation->expr_cache_count; i++)
    if (continuation->expr_cache[i].frame == old_frame)
      continuation->expr_cache[i].frame = new_frame;
}

static LainirValue interp_eval_expr_inner(LainirInterpreter *interp,
                                          LainirFrame *frame, L1Expr *expr) {
  if (!expr) return lainir_value_unit();
  if (expr->source_end > expr->source_start) {
    interp->current_source_start = expr->source_start;
    interp->current_source_end = expr->source_end;
  }
  if (!interp_tick(interp)) return lainir_value_unit();
  if (interp->trace_enabled && (uint32_t)expr->kind < 64)
    interp->trace_expr_kinds[expr->kind]++;
  switch (expr->kind) {
  case EXPR_SDIV:
  case EXPR_UDIV:
  case EXPR_SLT:
  case EXPR_SLE:
  case EXPR_SGT:
  case EXPR_SGE:
  case EXPR_ULT:
  case EXPR_ULE:
  case EXPR_UGT:
  case EXPR_UGE:
    return interp_eval_explicit_integer_binary(interp, frame, expr);
  case EXPR_ZEXT:
  case EXPR_SEXT:
  case EXPR_TRUNC: {
    LainirValue operand = interp_eval_expr(
        interp, frame, expr->data.conversion.operand);
    uint32_t target_width = expr->data.conversion.target_ty->width;
    uint64_t bits;
    if (interp->error) return lainir_value_unit();
    bits = interp_value_bits(
        interp, operand, "expected bits for integer conversion");
    if (expr->kind == EXPR_SEXT)
      bits = (uint64_t)interp_signed_bits(bits, operand.bit_width);
    return lainir_value_bits(bits, target_width);
  }
  case EXPR_BITCAST: {
    LainirValue operand = interp_eval_expr(
        interp, frame, expr->data.conversion.operand);
    if (interp->error) return lainir_value_unit();
    if (operand.kind != LAINIR_VALUE_BITS) {
      interp_trap(interp, "#bitcast requires a physical bit value");
      return lainir_value_unit();
    }
    return lainir_value_bits(
        operand.as.bits,
        expr->data.conversion.target_ty
            ? expr->data.conversion.target_ty->width : operand.bit_width);
  }
  case EXPR_CONST:
    return lainir_value_bits((uint64_t)expr->data.const_val, 32);
  case EXPR_STRING:
    return lainir_value_string(expr->data.str_val.content);
  case EXPR_DATA_ADDR: {
    L1Subroutine *data = interp_find_data(interp, expr->data.data_addr.name);
    if (!data) {
      interp_trap(interp, "data object not found");
      return lainir_value_unit();
    }
    return lainir_value_addr(data->data_bytes);
  }
  case EXPR_ARG:
    if (expr->data.arg.index >= frame->arg_count) {
      interp_trap(interp, "argument index out of bounds"); return lainir_value_unit();
    }
    return frame->args[expr->data.arg.index];
  case EXPR_VAR: {
    LainirBinding *b = interp_lookup_local(frame, expr->data.var.name);
    if (!b) { interp_trap(interp, "unknown local variable"); return lainir_value_unit(); }
    return b->value;
  }
  case EXPR_ADD: {
    LainirValue l = interp_eval_expr(interp, frame, expr->data.bin.left);
    LainirValue r = interp_eval_expr(interp, frame, expr->data.bin.right);
    if (interp->error) return lainir_value_unit();
    /* LAIN-IR uses #add for byte-address arithmetic in generated code.  Keep
     * integer addition unchanged, while allowing an address plus a scalar
     * offset to produce another address.  This is the runtime counterpart of
     * the verifier's address expressions and lets formal stdlib code use the
     * same memory helpers as hand-written LAIN-IR. */
    if (l.kind == LAINIR_VALUE_ADDR && r.kind == LAINIR_VALUE_BITS)
      return lainir_value_addr((uint8_t *)l.as.addr + (size_t)r.as.bits);
    if (l.kind == LAINIR_VALUE_BITS && r.kind == LAINIR_VALUE_ADDR)
      return lainir_value_addr((uint8_t *)r.as.addr + (size_t)l.as.bits);
    return lainir_value_bits(interp_value_bits(interp,l,"expected bits for add") + interp_value_bits(interp,r,"expected bits for add"), l.bit_width ? l.bit_width : 32);
  }
  case EXPR_SUB: {
    LainirValue l = interp_eval_expr(interp, frame, expr->data.bin.left);
    LainirValue r = interp_eval_expr(interp, frame, expr->data.bin.right);
    if (interp->error) return lainir_value_unit();
    return lainir_value_bits(interp_value_bits(interp,l,"expected bits for sub") - interp_value_bits(interp,r,"expected bits for sub"), l.bit_width ? l.bit_width : 32);
  }
  case EXPR_MUL: {
    LainirValue l = interp_eval_expr(interp, frame, expr->data.bin.left);
    LainirValue r = interp_eval_expr(interp, frame, expr->data.bin.right);
    if (interp->error) return lainir_value_unit();
    return lainir_value_bits(interp_value_bits(interp,l,"expected bits for mul") * interp_value_bits(interp,r,"expected bits for mul"), l.bit_width ? l.bit_width : 32);
  }
  case EXPR_EQ: {
    LainirValue l = interp_eval_expr(interp, frame, expr->data.bin.left);
    LainirValue r = interp_eval_expr(interp, frame, expr->data.bin.right);
    if (interp->error) return lainir_value_unit();
    int equal = interp_values_equal(interp, l, r);
    if (interp->error) return lainir_value_unit();
    return lainir_value_bits(equal, 1);
  }
  case EXPR_NE: {
    LainirValue l = interp_eval_expr(interp, frame, expr->data.bin.left);
    LainirValue r = interp_eval_expr(interp, frame, expr->data.bin.right);
    if (interp->error) return lainir_value_unit();
    int equal = interp_values_equal(interp, l, r);
    if (interp->error) return lainir_value_unit();
    return lainir_value_bits(!equal, 1);
  }
  case EXPR_FADD: {
    LainirValue l = interp_eval_expr(interp, frame, expr->data.bin.left);
    LainirValue r = interp_eval_expr(interp, frame, expr->data.bin.right);
    if (interp->error) return lainir_value_unit();
    double lv = interp_value_float(interp, l, "expected #float for fadd");
    double rv = interp_value_float(interp, r, "expected #float for fadd");
    return interp_make_float(lv + rv, l.bit_width);
  }
  case EXPR_FSUB: {
    LainirValue l = interp_eval_expr(interp, frame, expr->data.bin.left);
    LainirValue r = interp_eval_expr(interp, frame, expr->data.bin.right);
    if (interp->error) return lainir_value_unit();
    double lv = interp_value_float(interp, l, "expected #float for fsub");
    double rv = interp_value_float(interp, r, "expected #float for fsub");
    return interp_make_float(lv - rv, l.bit_width);
  }
  case EXPR_FMUL: {
    LainirValue l = interp_eval_expr(interp, frame, expr->data.bin.left);
    LainirValue r = interp_eval_expr(interp, frame, expr->data.bin.right);
    if (interp->error) return lainir_value_unit();
    double lv = interp_value_float(interp, l, "expected #float for fmul");
    double rv = interp_value_float(interp, r, "expected #float for fmul");
    return interp_make_float(lv * rv, l.bit_width);
  }
  case EXPR_FDIV: {
    LainirValue l = interp_eval_expr(interp, frame, expr->data.bin.left);
    LainirValue r = interp_eval_expr(interp, frame, expr->data.bin.right);
    if (interp->error) return lainir_value_unit();
    double lv = interp_value_float(interp, l, "expected #float for fdiv");
    double rv = interp_value_float(interp, r, "expected #float for fdiv");
    if (rv == 0.0) { interp_trap(interp, "fdiv by zero"); return lainir_value_unit(); }
    return interp_make_float(lv / rv, l.bit_width);
  }
  case EXPR_FEQ: {
    LainirValue l = interp_eval_expr(interp, frame, expr->data.bin.left);
    LainirValue r = interp_eval_expr(interp, frame, expr->data.bin.right);
    if (interp->error) return lainir_value_unit();
    double lv = interp_value_float(interp, l, "expected #float for feq");
    double rv = interp_value_float(interp, r, "expected #float for feq");
    return lainir_value_bits(lv == rv, 1);
  }
  case EXPR_FLT: {
    LainirValue l = interp_eval_expr(interp, frame, expr->data.bin.left);
    LainirValue r = interp_eval_expr(interp, frame, expr->data.bin.right);
    if (interp->error) return lainir_value_unit();
    double lv = interp_value_float(interp, l, "expected #float for flt");
    double rv = interp_value_float(interp, r, "expected #float for flt");
    return lainir_value_bits(lv < rv, 1);
  }
  case EXPR_POPCOUNT: {
    LainirValue v = interp_eval_expr(interp, frame, expr->data.unary.operand);
    if (interp->error) return lainir_value_unit();
    uint64_t x = interp_value_bits(interp,v,"expected bits for popcount");
    return lainir_value_bits(__builtin_popcountll(x), 32);
  }
  case EXPR_CLZ: {
    LainirValue v = interp_eval_expr(interp, frame, expr->data.unary.operand);
    if (interp->error) return lainir_value_unit();
    uint64_t x = interp_value_bits(interp,v,"expected bits for clz");
    return lainir_value_bits(__builtin_clzll(x), 32);
  }
  case EXPR_ROTL: {
    LainirValue v = interp_eval_expr(interp, frame, expr->data.unary.operand);
    if (interp->error) return lainir_value_unit();
    uint64_t x = interp_value_bits(interp,v,"expected bits for rotl");
    // Rotate left by 1: (x << 1) | (x >> 63)
    return lainir_value_bits((x << 1) | (x >> 63), 64);
  }
  case EXPR_INT2PTR: {
    LainirValue v = interp_eval_expr(interp, frame, expr->data.unary.operand);
    if (interp->error) return lainir_value_unit();
    return lainir_value_addr((void*)(uintptr_t)v.as.bits);
  }
  case EXPR_PTR2INT: {
    LainirValue v = interp_eval_expr(interp, frame, expr->data.unary.operand);
    if (interp->error) return lainir_value_unit();
    return lainir_value_bits((uint64_t)(uintptr_t)interp_value_addr(interp,v,"expected address for ptr2int"), 64);
  }
  case EXPR_ALLOCA: {
    uint32_t sz = expr->data.alloca.byte_size;
    if (!sz) sz = interp_type_size(expr->data.alloca.element_ty);
    if (interp->caps && interp->caps->max_alloc_bytes &&
        (interp->allocated_bytes > interp->caps->max_alloc_bytes ||
         (uint64_t)sz > interp->caps->max_alloc_bytes -
                         interp->allocated_bytes)) {
      interp_trap(interp, "interpreter allocation limit exceeded");
      return lainir_value_unit();
    }
    uint8_t *data = calloc(sz ? sz : 1, 1);
    if (!data) { interp_trap(interp, "out of memory"); return lainir_value_unit(); }
    if (frame->alloca_count == frame->alloca_cap) {
      uint32_t nc = frame->alloca_cap ? frame->alloca_cap * 2 : 4;
      LainirBuffer *na = realloc(frame->allocas, sizeof(LainirBuffer) * nc);
      if (!na) { free(data); interp_trap(interp, "out of memory"); return lainir_value_unit(); }
      frame->allocas = na; frame->alloca_cap = nc;
    }
    frame->allocas[frame->alloca_count].data = data;
    frame->allocas[frame->alloca_count].size = sz;
    frame->alloca_count++;
    interp->allocated_bytes += sz;
    return lainir_value_addr(data);
  }
  case EXPR_LEA: {
    LainirValue bv = interp_eval_expr(interp, frame, expr->data.lea.base);
    LainirValue iv = interp_eval_expr(interp, frame, expr->data.lea.idx);
    if (interp->error) return lainir_value_unit();
    uint8_t *addr = (uint8_t *)interp_value_addr(interp, bv, "expected address for lea");
    if (interp->error) return lainir_value_unit();
    addr += interp_value_bits(interp, iv, "expected bits for lea index") * expr->data.lea.scale;
    addr += expr->data.lea.offset;
    return lainir_value_addr(addr);
  }
  case EXPR_LOAD: {
    LainirValue av = interp_eval_expr(interp, frame, expr->data.load.addr);
    if (interp->error) return lainir_value_unit();
    void *address = interp_value_addr(interp, av, "expected address for load");
    if (interp->error) return lainir_value_unit();
    if (!interp_validate_memory_access(
            interp, frame, address, interp_type_size(expr->data.load.ty), 0))
      return lainir_value_unit();
    return interp_load_typed(interp, address, expr->data.load.ty);
  }
  case EXPR_PROC_ADDR: {
    L1Subroutine *target = interp_find_sub(
        interp, expr->data.proc_addr.fn_name);
    if (!target) {
      interp_trap(interp, "procedure address target not found");
      return lainir_value_unit();
    }
    return lainir_value_func(target);
  }
  case EXPR_CALL:
    return interp_eval_call(interp, frame, expr);
  case EXPR_EVAL:
    return interp_eval_block(interp, frame, expr->data.eval.block);
  case EXPR_CALL_INDIRECT: {
    LainirValue target = interp_eval_expr(
        interp, frame, expr->data.call_indirect.fn_ptr);
    LainirValue args_inline[8];
    LainirValue *args = args_inline;
    int args_heap = 0;
    LainirValue result = lainir_value_unit();
    if (interp->error) return result;
    if (target.kind != LAINIR_VALUE_FUNC || !target.as.func) {
      interp_trap(interp, "call_indirect target is not a procedure");
      return result;
    }
    if (expr->data.call_indirect.arg_count >
        sizeof(args_inline) / sizeof(args_inline[0])) {
      args = calloc(
          expr->data.call_indirect.arg_count, sizeof(LainirValue));
      args_heap = 1;
      if (!args) {
        interp_trap(interp, "out of memory");
        return result;
      }
    }
    for (uint32_t i = 0; i < expr->data.call_indirect.arg_count; i++) {
      args[i] = interp_eval_expr(
          interp, frame, expr->data.call_indirect.args[i]);
      if (interp->error) {
        if (args_heap) free(args);
        return result;
      }
    }
    if (target.as.func->is_extern && !target.as.func->blocks)
      result = interp_call_host(
          interp, target.as.func->name, args,
          expr->data.call_indirect.arg_count);
    else {
      L1Block *saved_active_block = interp->active_block;
      L1Instruction *saved_active_inst = interp->active_inst;
      result = interp_call_sub(
          interp, target.as.func, args,
          expr->data.call_indirect.arg_count);
      interp->active_block = saved_active_block;
      interp->active_inst = saved_active_inst;
    }
    if (args_heap) free(args);
    return result;
  }
  default:
    interp_trap(interp, "unsupported expression");
    return lainir_value_unit();
  }
}

static LainirValue interp_eval_expr(LainirInterpreter *interp,
                                     LainirFrame *frame, L1Expr *expr) {
  LainirValue cached = lainir_value_unit();
  if (!expr) return cached;
  if (interp_expr_cache_lookup(interp, frame, expr, &cached))
    return cached;
  cached = interp_eval_expr_inner(interp, frame, expr);
  interp_expr_cache_store(interp, frame, expr, cached);
  return cached;
}

/* ═══════════════════════════════════════════════════════════════
 * Block executor (structured IR — no terminators)
 * ═══════════════════════════════════════════════════════════════ */

static void interp_exec_block(LainirInterpreter *interp, LainirFrame *frame,
                               L1Block *block) {
  L1Instruction *inst = block->body;
  int track = interp->continuation_tracking && interp->continuation &&
              frame == &interp->continuation->frame;
  LainirNestedContinuation *nested =
      interp->continuation ? interp->continuation->nested_top : NULL;
  int nested_track = nested && frame == nested->frame &&
                     block == nested->block && nested->inst;
  if (track && interp->continuation->next_block == block &&
      interp->continuation->next_inst)
    inst = interp->continuation->next_inst;
  if (nested_track) inst = nested->inst;
  while (inst) {
    interp->active_block = block;
    interp->active_inst = inst;
    interp->current_line = inst->line > 0 ? (uint32_t)inst->line : 0;
    interp->current_column = inst->column > 0 ? (uint32_t)inst->column : 0;
    interp->current_source_start = inst->source_start;
    interp->current_source_end = inst->source_end;
    if (track) {
      interp->continuation->next_block = block;
      interp->continuation->next_inst = inst;
    }
    if (interp->vm_control)
      lainir_vm_control_set_position(
          interp->vm_control, interp->vm_owner,
          (uint64_t)(uintptr_t)block,
          (uint64_t)(uintptr_t)inst);
    int saved_boundary = interp->instruction_boundary;
    interp->instruction_boundary = 1;
    int ticked = interp_tick(interp);
    interp->instruction_boundary = saved_boundary;
    if (!ticked) return;
    if (interp->trace_enabled && (uint32_t)inst->kind < 16)
      interp->trace_inst_kinds[inst->kind]++;
    if (interp->should_return || interp->should_break || interp->should_continue)
      return;

    switch (inst->kind) {
    case INST_LET: {
      LainirValue value = interp_eval_expr(interp, frame, inst->data.let.val);
      if (interp->error || interp->vm_blocked) return;
      value = interp_coerce_physical(interp, value, inst->data.let.ty);
      if (interp->error) return;
      interp_set_local(frame, inst->data.let.name, value);
      break;
    }
    case INST_SET: {
      LainirValue value = interp_eval_expr(interp, frame, inst->data.set.val);
      if (interp->error || interp->vm_blocked) return;
      value = interp_coerce_physical(interp, value, inst->data.set.ty);
      if (interp->error) return;
      if (!interp_set_local(frame, inst->data.set.name, value))
        { interp_trap(interp, "set: unknown variable"); return; }
      break;
    }
    case INST_STORE: {
      LainirValue dest  = interp_eval_expr(interp, frame, inst->data.store.dest);
      LainirValue value = interp_eval_expr(interp, frame, inst->data.store.val);
      if (interp->error || interp->vm_blocked) return;
      value = interp_coerce_physical(
          interp, value, inst->data.store.store_ty);
      if (interp->error || interp->vm_blocked) return;
      uint8_t *addr = (uint8_t *)interp_value_addr(interp, dest, "expected address for store");
      if (interp->error) return;
      if (interp_addr_is_readonly_data(interp, addr)) {
        interp_trap(interp, "store to read-only #data");
        return;
      }
      uint32_t width = interp_store_width(inst->data.store.store_ty, value);
      if (!interp_validate_memory_access(interp, frame, addr, width, 1))
        return;
      memcpy(addr, &value.as.bits, width);
      break;
    }
    case INST_IF: {
      LainirValue cond = interp_eval_expr(interp, frame, inst->data.if_stmt.condition);
      if (interp->error || interp->vm_blocked) return;
      if (interp_value_bits(interp, cond, "expected bits for if condition")) {
        int saved_tracking = interp->continuation_tracking;
        interp->continuation_tracking = 0;
        if (inst->data.if_stmt.then_body)
          interp_exec_block(interp, frame, inst->data.if_stmt.then_body);
        interp->continuation_tracking = saved_tracking;
      } else {
        int saved_tracking = interp->continuation_tracking;
        interp->continuation_tracking = 0;
        if (inst->data.if_stmt.else_body)
          interp_exec_block(interp, frame, inst->data.if_stmt.else_body);
        interp->continuation_tracking = saved_tracking;
      }
      if (interp->error || interp->vm_slice_yielded || interp->vm_blocked ||
          interp->should_return)
        return;
      break;
    }
    case INST_LOOP: {
      if (!inst->data.loop.body) break;
      int saved_break = interp->should_break;
      int saved_cont  = interp->should_continue;
      interp->should_break = 0;
      interp->should_continue = 0;
      while (1) {
        int saved_tracking = interp->continuation_tracking;
        interp->continuation_tracking = 0;
        interp_exec_block(interp, frame, inst->data.loop.body);
        interp->continuation_tracking = saved_tracking;
        if (interp->error || interp->vm_blocked) return;
        if (interp->should_return) break;
        if (interp->should_break) { interp->should_break = 0; break; }
        if (interp->should_continue) { interp->should_continue = 0; continue; }
        break;
      }
      interp->should_break = saved_break;
      interp->should_continue = saved_cont;
      break;
    }
    case INST_BREAK:
      interp->should_break = 1;
      return;
    case INST_CONTINUE:
      interp->should_continue = 1;
      return;
    case INST_RETURN: {
      LainirValue value = interp_eval_expr(interp, frame, inst->data.ret.val);
      if (interp->error || interp->vm_slice_yielded || interp->vm_blocked) return;
      value = interp_coerce_physical(interp, value, frame->sub->ret_ty);
      if (interp->error) return;
      interp->should_return = 1;
      interp->return_value = value;
      return;
    }
    case INST_CALL:
      (void)interp_eval_expr(interp, frame, inst->data.call_inst.expr);
      if (interp->error || interp->vm_slice_yielded || interp->vm_blocked) return;
      break;
    }
    interp_expr_cache_clear_frame(interp, frame);
    if (track) {
      interp->continuation->next_block = inst->next ? block : block->next;
      interp->continuation->next_inst = inst->next;
    }
    inst = inst->next;
  }
}

/* Move a suspended nested frame off the C stack.  Inline argument/local
 * storage is copied into the heap frame; alloca buffers and heap-owned tables
 * are transferred, so address values remain valid across the suspension. */
static LainirFrame *interp_detach_frame(LainirFrame *source,
                                        LainirFrame *caller) {
  LainirFrame *frame = calloc(1, sizeof(*frame));
  if (!frame) return NULL;
  *frame = *source;
  frame->caller = caller;
  if (source->args == source->args_inline) {
    memcpy(frame->args_inline, source->args_inline,
           sizeof(source->args_inline));
    frame->args = frame->args_inline;
    frame->args_heap = 0;
  } else {
    source->args = NULL;
    source->args_heap = 0;
  }
  if (source->locals == source->locals_inline) {
    memcpy(frame->locals_inline, source->locals_inline,
           sizeof(source->locals_inline));
    frame->locals = frame->locals_inline;
    frame->locals_heap = 0;
    for (uint32_t i = 0; i < source->local_count; i++)
      source->locals_inline[i].name = NULL;
  } else {
    source->locals = NULL;
    source->locals_heap = 0;
  }
  source->local_count = 0;
  source->local_index = NULL;
  source->allocas = NULL;
  source->alloca_count = 0;
  source->alloca_cap = 0;
  return frame;
}

/* A small opt-in native path for the bootstrap compiler's leaf procedures.
 * These procedures are pure layout accessors or bounded byte comparisons.
 * Keeping the path behind LAINIR_FAST_BUILTINS preserves the ordinary
 * interpreter for conformance/debugging runs while making long self-hosting
 * bootstraps practical. */
static int interp_fast_enabled(const LainirInterpreter *interp) {
  return interp && interp->fast_builtins_enabled;
}

static int interp_fast_leaves_enabled(const LainirInterpreter *interp) {
  return interp && interp->fast_leaves_enabled;
}

static const char *interp_fast_name(const char *name) {
  if (name && name[0] == 'f' && name[1] == '0' && name[2] == '_')
    return name + 3;
  return name;
}

static void *interp_fast_addr(const LainirValue *value) {
  if (!value) return NULL;
  if (value->kind == LAINIR_VALUE_ADDR) return value->as.addr;
  if (value->kind == LAINIR_VALUE_STRING) return (void *)value->as.string;
  return NULL;
}

static uint64_t interp_fast_bits(const LainirValue *value) {
  return value && value->kind == LAINIR_VALUE_BITS ? value->as.bits : 0;
}

static uint64_t interp_fast_load_u64(const uint8_t *base, size_t offset) {
  uint64_t value = 0;
  memcpy(&value, base + offset, sizeof(value));
  return value;
}

static void interp_fast_store_u64(uint8_t *base, size_t offset,
                                  uint64_t value) {
  memcpy(base + offset, &value, sizeof(value));
}

static uint32_t interp_fast_load_u32(const uint8_t *base, size_t offset) {
  uint32_t value = 0;
  memcpy(&value, base + offset, sizeof(value));
  return value;
}

static int interp_fast_suffix(const char *name, const char *suffix) {
  if (!name || !suffix) return 0;
  size_t name_length = strlen(name);
  size_t suffix_length = strlen(suffix);
  return name_length >= suffix_length &&
         strcmp(name + name_length - suffix_length, suffix) == 0;
}

/* The source compiler's StringBuffer accessors are tiny wrappers around the
 * same raw loads used by the native `tool_*` accessors.  Generated source-
 * faithful compilers give them an f0_<hash>_ prefix, so dispatch by suffix
 * while keeping this path opt-in with LAINIR_FAST_BUILTINS. */
static int interp_fast_sb_byte(const uint8_t *buffer, uint64_t index,
                               uint8_t *result) {
  static unsigned trace_count = 0;
  if (!buffer || !result) return 0;
  uint64_t header = interp_fast_load_u64(buffer, 0);
  /* Frozen L1 constants can materialize `usize` sentinels as either a full
   * 64-bit all-ones value or a zero-extended 32-bit all-ones value. */
  if (header == UINT64_MAX || header == UINT32_MAX) {
    uint8_t *data = (uint8_t *)interp_fast_load_u64(buffer, 16);
    if (!data) return 0;
    *result = data[index];
  } else {
    *result = buffer[8 + index];
  }
  if (getenv("LAINIR_TRACE_FAST_SB") && trace_count++ < 32) {
    fprintf(stderr, "fast_sb buffer=%p header=%llu index=%llu byte=%u data=%p\n",
            (const void *)buffer, (unsigned long long)header,
            (unsigned long long)index, (unsigned)*result,
            header == UINT64_MAX ? (void *)interp_fast_load_u64(buffer, 16)
                                 : (const void *)(buffer + 8));
    fflush(stderr);
  }
  return 1;
}

static int interp_fast_span_equal(const uint8_t *buffer_a, uint64_t start_a,
                                  const uint8_t *buffer_b, uint64_t start_b,
                                  uint64_t length) {
  for (uint64_t index = 0; index < length; index++) {
    uint8_t left = 0;
    uint8_t right = 0;
    if (!interp_fast_sb_byte(buffer_a, start_a + index, &left) ||
        !interp_fast_sb_byte(buffer_b, start_b + index, &right) ||
        left != right)
      return 0;
  }
  return 1;
}

/* Exact native counterpart of lainc's scan_top_op/scan_semi/scan_brace.  The
 * source routines intentionally use unsigned depth arithmetic: an unmatched
 * closing parenthesis underflows and therefore cannot become top-level again.
 * Keep that behavior instead of clamping depth, which was the source of an
 * earlier parser fast-path mismatch. */
static int interp_fast_scan_delimited(const uint8_t *source, uint64_t start,
                                      uint64_t end, int mode, int op,
                                      uint64_t *result) {
  if (!source || !result) return 0;
  uint64_t current = start;
  uint64_t depth = 0;
  while (current < end) {
    uint8_t byte = 0;
    if (!interp_fast_sb_byte(source, current, &byte)) return 0;
    if (byte == 34) {
      /* This mirrors skip_string: it starts after the opening quote, stops at
       * the next quote, and returns the position after that quote. */
      current++;
      while (current < end) {
        if (!interp_fast_sb_byte(source, current, &byte)) return 0;
        if (byte == 34) {
          current++;
          break;
        }
        current++;
      }
    } else if (byte == 40) {
      depth++;
      current++;
    } else if (byte == 41) {
      depth--;
      current++;
    } else {
      int target = mode == 0 ? op : (mode == 1 ? 59 : 123);
      if (depth == 0 && byte == (uint8_t)target) {
        *result = current;
        return 1;
      }
      current++;
    }
  }
  *result = end;
  return 1;
}

static int interp_fast_skip_line_comment(const uint8_t *source,
                                         uint64_t index, uint64_t length,
                                         uint64_t *result) {
  if (!source || !result) return 0;
  uint64_t current = index + 2;
  while (current < length) {
    uint8_t byte = 0;
    if (!interp_fast_sb_byte(source, current, &byte)) return 0;
    if (byte == 10) {
      *result = current + 1;
      return 1;
    }
    current++;
  }
  *result = current;
  return 1;
}

static int interp_fast_skip_space(const uint8_t *source, uint64_t index,
                                  uint64_t length, uint64_t *result) {
  if (!source || !result) return 0;
  uint64_t current = index;
  while (current < length) {
    uint8_t byte = 0;
    if (!interp_fast_sb_byte(source, current, &byte)) return 0;
    if (byte == 32 || byte == 10) {
      current++;
      continue;
    }
    if (byte == 47 && current + 1 < length) {
      uint8_t next = 0;
      if (!interp_fast_sb_byte(source, current + 1, &next)) return 0;
      if (next == 47) {
        if (!interp_fast_skip_line_comment(source, current, length, &current))
          return 0;
        continue;
      }
    }
    break;
  }
  *result = current;
  return 1;
}

/* The source-faithful lainc keeps six decoded fields for each metadata row at
 * a fixed offset in its StringBuffer and an open-addressed name index in a
 * separately allocated table.  These helpers mirror the L1 implementation
 * exactly.  They are deliberately kept separate from the std::meta helpers
 * above: both APIs use the name `meta_*`, but they operate on different object
 * layouts. */
static uint64_t interp_fast_meta_row_field(const uint8_t *table,
                                           uint64_t row, uint64_t field) {
  if (!table || row == 0 || field >= 6) return 0;
  return interp_fast_load_u64(table, 655360ULL + (row - 1) * 48ULL + field * 8ULL);
}

static uint64_t interp_fast_meta_name_hash(const uint8_t *source,
                                           uint64_t start,
                                           uint64_t length) {
  if (length == 0) return 1;
  uint8_t first = 0;
  uint8_t last = 0;
  if (!interp_fast_sb_byte(source, start, &first) ||
      !interp_fast_sb_byte(source, start + length - 1, &last))
    return UINT64_MAX;
  return length * 131ULL + (uint64_t)first * 17ULL + (uint64_t)last + 2ULL;
}

static uint64_t interp_fast_meta_index_lookup(const uint8_t *table,
                                              const uint8_t *source,
                                              uint64_t start,
                                              uint64_t length) {
  if (!table || !source) return UINT64_MAX;
  const uint8_t *index_base =
      (const uint8_t *)(uintptr_t)interp_fast_load_u64(table, 524184);
  if (!index_base) return UINT64_MAX;
  uint64_t generation = interp_fast_load_u64(table, 524192);
  uint64_t generation_base = generation * 4294967296ULL;
  uint64_t hash = interp_fast_meta_name_hash(source, start, length);
  if (hash == UINT64_MAX) return UINT64_MAX;
  uint64_t expected = generation_base + hash + 1ULL;
  uint64_t slot = hash % 16384ULL;
  for (uint64_t probes = 0; probes < 16384ULL; probes++) {
    uint64_t offset = (slot + probes) * 16ULL;
    uint64_t stored = interp_fast_load_u64(index_base, offset);
    if (stored == 0 || stored < generation_base) return 0;
    if (stored == expected) {
      uint64_t row = interp_fast_load_u64(index_base, offset + 8);
      uint64_t row_length = interp_fast_meta_row_field(table, row, 2);
      uint64_t row_start = interp_fast_meta_row_field(table, row, 1);
      if (row_length == length &&
          interp_fast_span_equal(source, row_start, source, start, length))
        return row;
    }
  }
  return 0;
}

static uint64_t interp_fast_meta_cache_get(const uint8_t *table,
                                           const uint8_t *source,
                                           uint64_t start,
                                           uint64_t length,
                                           uint64_t mode) {
  if (!table || !source) return 0;
  for (uint64_t slot = 0; slot < 4; slot++) {
    uint64_t base = 524000ULL + slot * 40ULL;
    if (interp_fast_load_u64(table, base) == (uint64_t)(uintptr_t)source &&
        interp_fast_load_u64(table, base + 8) == start &&
        interp_fast_load_u64(table, base + 16) == length &&
        interp_fast_load_u64(table, base + 24) == mode) {
      uint64_t row = interp_fast_load_u64(table, base + 32);
      if (row != 0) return row;
    }
  }
  return 0;
}

static void interp_fast_meta_cache_put(uint8_t *table, const uint8_t *source,
                                       uint64_t start, uint64_t length,
                                       uint64_t mode, uint64_t row) {
  if (!table || !source || row == 0) return;
  uint64_t slot = interp_fast_load_u64(table, 524240);
  if (slot >= 4) slot = 0;
  uint64_t base = 524000ULL + slot * 40ULL;
  interp_fast_store_u64(table, base, (uint64_t)(uintptr_t)source);
  interp_fast_store_u64(table, base + 8, start);
  interp_fast_store_u64(table, base + 16, length);
  interp_fast_store_u64(table, base + 24, mode);
  interp_fast_store_u64(table, base + 32, row);
  interp_fast_store_u64(table, 524240, slot + 1);
}

static int interp_fast_meta_index_prepare(uint8_t *table,
                                          const uint8_t *source) {
  if (!table || !source) return 0;
  uint8_t *index_base =
      (uint8_t *)(uintptr_t)interp_fast_load_u64(table, 524184);
  if (!index_base) return 0;
  uint64_t old_source = interp_fast_load_u64(table, 524168);
  uint64_t indexed = interp_fast_load_u64(table, 524176);
  uint64_t generation = interp_fast_load_u64(table, 524192);
  if (getenv("LAINIR_TRACE_META")) {
    static unsigned trace_prepare = 0;
    if (trace_prepare++ < 16)
      fprintf(stderr, "fast_meta_prepare table=%p index=%p old=%p indexed=%llu gen=%llu rows=%llu\n",
              (void *)table, (void *)index_base,
              (void *)(uintptr_t)old_source,
              (unsigned long long)indexed,
              (unsigned long long)generation,
              (unsigned long long)interp_fast_load_u64(table, 524280));
  }
  if (old_source != (uint64_t)(uintptr_t)source) {
    interp_fast_store_u64(table, 524168, (uint64_t)(uintptr_t)source);
    indexed = 0;
    generation++;
    interp_fast_store_u64(table, 524176, 0);
    interp_fast_store_u64(table, 524192, generation);
  }
  uint64_t generation_base = generation * 4294967296ULL;
  uint64_t rows = interp_fast_load_u64(table, 524280);
  while (indexed < rows) {
    uint64_t row = indexed + 1;
    uint64_t row_offset = 655360ULL + (row - 1) * 48ULL;
    uint64_t start = interp_fast_load_u64(table, row_offset + 8);
    uint64_t length = interp_fast_load_u64(table, row_offset + 16);
    uint64_t hash = interp_fast_meta_name_hash(source, start, length);
    if (hash == UINT64_MAX) return 0;
    uint64_t expected = generation_base + hash + 1ULL;
    uint64_t slot = hash % 16384ULL;
    int placed = 0;
    for (uint64_t probes = 0; probes < 16384ULL; probes++) {
      uint64_t offset = (slot + probes) * 16ULL;
      uint64_t stored = interp_fast_load_u64(index_base, offset);
      if (stored == 0 || stored < generation_base) {
        interp_fast_store_u64(index_base, offset, expected);
        interp_fast_store_u64(index_base, offset + 8, row);
        placed = 1;
        break;
      }
      if (stored == expected) {
        uint64_t prior = interp_fast_load_u64(index_base, offset + 8);
        uint64_t prior_start = interp_fast_meta_row_field(table, prior, 1);
        uint64_t prior_length = interp_fast_meta_row_field(table, prior, 2);
        if (prior_length == length &&
            interp_fast_span_equal(source, prior_start, source, start, length)) {
          placed = 1;
          break;
        }
      }
    }
    /* Preserve the L1 loop's monotonic progress even if a pathological table
     * fills all slots; lookup will fall back to the row scan in that case. */
    if (!placed) {
      interp_fast_store_u64(table, 524176, row);
      return 1;
    }
    indexed = row;
    interp_fast_store_u64(table, 524176, indexed);
  }
  return 1;
}

static int interp_fast_meta_row_matches(const uint8_t *table,
                                        const uint8_t *source,
                                        uint64_t row, uint64_t start,
                                        uint64_t length, uint64_t want,
                                        int check_kind) {
  if (!table || !source || row == 0) return 0;
  if (check_kind && interp_fast_meta_row_field(table, row, 0) != want)
    return 0;
  uint64_t row_length = interp_fast_meta_row_field(table, row, 2);
  if (row_length != length) return 0;
  uint64_t row_start = interp_fast_meta_row_field(table, row, 1);
  return interp_fast_span_equal(source, row_start, source, start, length);
}

static int interp_fast_meta_table_lookup(LainirInterpreter *interp,
                                         const LainirValue *args,
                                         uint64_t want, int check_kind,
                                         int latest, LainirValue *result) {
  uint8_t *table = (uint8_t *)interp_fast_addr(&args[0]);
  uint8_t *source = (uint8_t *)interp_fast_addr(&args[2]);
  uint64_t start = interp_fast_bits(&args[3]);
  uint64_t length = interp_fast_bits(&args[4]);
  if (!table || !source) {
    interp_trap(interp, "load from null");
    return 1;
  }
  /* A direct row scan is the source-faithful implementation of all three
   * helpers.  Keep it separate from the experimental hash-index path: the
   * index stores only the first row for a name, while kind/latest lookups can
   * intentionally select another row.  This path therefore changes only the
   * execution layer (C loop instead of interpreted Lain instructions), not
   * lookup semantics. */
  int direct_scan = interp->fast_meta_scan_enabled;
  /* A missing external index means this is an older artifact.  Let the L1
   * implementation handle it rather than silently changing its semantics. */
  if (!interp_fast_load_u64(table, 524184)) return 0;

  /* The L1 lookup helpers prepare the append-stable name index before they
   * scan rows.  A native early return must preserve that side effect because
   * meta_lookup_qualified and later lookups can consult the index directly. */
  if (!latest && direct_scan &&
      !interp_fast_meta_index_prepare(table, source))
    return 0;

  if (!latest && !direct_scan && getenv("LAINIR_FAST_META_INDEX") &&
      !interp_fast_meta_index_prepare(table, source)) return 0;

  if (!latest && !direct_scan && !getenv("LAINIR_FAST_META_INDEX")) {
    if (getenv("LAINIR_TRACE_META"))
      fprintf(stderr, "fast_meta_fallback source=%p table=%p\n",
              (void *)source, (void *)table);
    return 0;
  }

  if (!latest && getenv("LAINIR_FAST_META_INDEX")) {
    uint64_t mode = check_kind ? want + 1ULL : 0;
    uint64_t cached = interp_fast_meta_cache_get(table, source, start,
                                                 length, mode);
    if (cached != 0) {
      if (interp->trace_enabled) interp->trace_meta_cache_hits++;
      *result = lainir_value_bits(cached, 64);
      return 1;
    }
    if (interp->trace_enabled) interp->trace_meta_cache_misses++;
  }

  uint64_t rows = interp_fast_load_u64(table, 524280);
  if (interp->trace_meta_enabled) {
    static unsigned trace_lookups = 0;
    if (trace_lookups++ < 128)
      fprintf(stderr, "fast_meta_lookup[%u] latest=%d kind=%d want=%llu start=%llu len=%llu rows=%llu\n",
              trace_lookups, latest, check_kind,
              (unsigned long long)want, (unsigned long long)start,
              (unsigned long long)length, (unsigned long long)rows);
  }
  if (!latest && !direct_scan && getenv("LAINIR_FAST_META_INDEX_HIT")) {
    /* index_prepare is dispatched independently and has already brought the
     * table through the current row count in normal generated code.  Calling
     * the index lookup here is safe for both the first and later lookups. */
    uint64_t indexed = interp_fast_meta_index_lookup(table, source, start,
                                                      length);
      if (indexed == UINT64_MAX) return 0;
    if (indexed != 0 && (!getenv("LAINIR_FAST_META_SKIP_KIND6") ||
                         interp_fast_meta_row_field(table, indexed, 0) != 6) &&
        (!check_kind || interp_fast_meta_row_field(table, indexed, 0) == want)) {
      if (getenv("LAINIR_TRACE_META"))
        fprintf(stderr, "fast_meta_hit row=%llu kind=%llu row_start=%llu row_len=%llu query_start=%llu query_len=%llu\n",
                (unsigned long long)indexed,
                (unsigned long long)interp_fast_meta_row_field(table, indexed, 0),
                (unsigned long long)interp_fast_meta_row_field(table, indexed, 1),
                (unsigned long long)interp_fast_meta_row_field(table, indexed, 2),
                (unsigned long long)start, (unsigned long long)length);
      if (!getenv("LAINIR_FAST_META_NO_CACHE"))
        interp_fast_meta_cache_put(table, source, start, length,
                                   check_kind ? want + 1ULL : 0, indexed);
      *result = lainir_value_bits(indexed, 64);
      return 1;
    }
    /* A miss or kind collision must retain the source implementation's
     * fallback semantics.  Returning 0 from the fast-dispatch hook here makes
     * interp_call_sub execute the original L1 scan, while successful indexed
     * hits stay on the native path. */
    return 0;
  }

  if (!latest && !direct_scan) return 0;

  if (latest) {
    for (uint64_t row = rows; row > 0; row--) {
      if (interp_fast_meta_row_matches(table, source, row, start, length,
                                       want, 1)) {
        *result = lainir_value_bits(row, 64);
        return 1;
      }
    }
  } else {
    for (uint64_t row = 1; row <= rows; row++) {
      if (interp_fast_meta_row_matches(table, source, row, start, length,
                                       want, check_kind)) {
        uint64_t kind = interp_fast_meta_row_field(table, row, 0);
        /* meta_lookup accepts every non-nil row; meta_lookup_kind accepts only
         * the requested kind.  The indexed hit above intentionally preserves
         * the source implementation's direct return behavior. */
        if (kind != 0 && (!check_kind || kind == want)) {
          interp_fast_meta_cache_put(table, source, start, length,
                                     check_kind ? want + 1ULL : 0, row);
          *result = lainir_value_bits(row, 64);
          return 1;
        }
      }
    }
  }
  *result = lainir_value_bits(0, 64);
  return 1;
}

static LainirValue interp_fast_meta_nil(void) {
  uint8_t *nil = (uint8_t *)calloc(8, 1);
  return lainir_value_addr(nil);
}

static LainirValue interp_fast_meta_lookup_span(
    LainirInterpreter *interp, const LainirValue *args, int local_only) {
  uint8_t *environment = (uint8_t *)interp_fast_addr(&args[0]);
  uint8_t *source = (uint8_t *)interp_fast_addr(&args[1]);
  uint64_t start = interp_fast_bits(&args[2]);
  uint64_t length = interp_fast_bits(&args[3]);
  if (!environment || !source) {
    interp_trap(interp, "load from null");
    return lainir_value_unit();
  }
  uint8_t *data = (uint8_t *)interp_fast_load_u64(source, 0);
  uint64_t hash = length;
  if (hash >= 256) hash -= 256;
  for (uint64_t index = 0; index < length; index++) {
    hash += data[start + index];
    if (hash >= 256) hash -= 256;
  }
  uint32_t depth = 0;
  while (environment && depth++ < (local_only ? 1u : 256u)) {
    if (interp_fast_load_u32(environment, 0) == 0)
      return interp_fast_meta_nil();
    uint8_t *buckets = (uint8_t *)interp_fast_load_u64(environment, 40);
    if (!buckets) {
      interp_trap(interp, "load from null");
      return lainir_value_unit();
    }
    uint8_t *binding = (uint8_t *)interp_fast_load_u64(buckets, hash * 16);
    uint32_t binding_steps = 0;
    while (binding && binding_steps++ < 4096) {
      if (interp_fast_load_u32(binding, 0) == 0) break;
      if (interp_fast_load_u64(binding, 56) == hash &&
          interp_fast_load_u64(binding, 24) == length) {
        uint8_t *binding_source =
            (uint8_t *)interp_fast_load_u64(binding, 8);
        uint8_t *binding_data = binding_source
            ? (uint8_t *)interp_fast_load_u64(binding_source, 0) : NULL;
        if (!binding_data) {
          interp_trap(interp, "load from null");
          return lainir_value_unit();
        }
        if (memcmp(data + start,
                   binding_data + interp_fast_load_u64(binding, 16),
                   (size_t)length) == 0)
          return lainir_value_addr(
              (void *)interp_fast_load_u64(binding, 32));
      }
      binding = (uint8_t *)interp_fast_load_u64(binding, 48);
    }
    if (local_only) break;
    environment = (uint8_t *)interp_fast_load_u64(environment, 8);
  }
  return interp_fast_meta_nil();
}

static LainirValue interp_fast_meta_lookup_path(
    LainirInterpreter *interp, const LainirValue *args) {
  uint8_t *environment = (uint8_t *)interp_fast_addr(&args[0]);
  uint8_t *source = (uint8_t *)interp_fast_addr(&args[1]);
  uint8_t *node = (uint8_t *)interp_fast_addr(&args[2]);
  if (!environment || !source || !node) {
    interp_trap(interp, "load from null");
    return lainir_value_unit();
  }
  uint8_t *data = (uint8_t *)interp_fast_load_u64(source, 0);

  /* Tokenized paths (`a . b`) retain the dot and member as sibling nodes. */
  uint8_t *next = (uint8_t *)interp_fast_load_u64(node, 40);
  if (next && interp_fast_load_u32(next, 0) == 1 &&
      interp_fast_load_u64(next, 16) == 1 &&
      data[interp_fast_load_u64(next, 8)] == '.') {
    uint8_t *current = node;
    while (current) {
      LainirValue span_args[4] = {
        args[0], args[1],
        lainir_value_bits(interp_fast_load_u64(current, 8), 64),
        lainir_value_bits(interp_fast_load_u64(current, 16), 64)
      };
      LainirValue value = interp_fast_meta_lookup_span(interp, span_args, 0);
      if (interp->error) return value;
      uint8_t *value_ptr = (uint8_t *)interp_fast_addr(&value);
      if (!value_ptr || interp_fast_load_u32(value_ptr, 0) == 0)
        return value;
      next = (uint8_t *)interp_fast_load_u64(current, 40);
      if (!next || interp_fast_load_u32(next, 0) != 1 ||
          interp_fast_load_u64(next, 16) != 1 ||
          data[interp_fast_load_u64(next, 8)] != '.')
        return value;
      uint8_t *member = (uint8_t *)interp_fast_load_u64(next, 40);
      if (!member) return interp_fast_meta_nil();
      uint32_t kind = interp_fast_load_u32(value_ptr, 0);
      if (kind != 2 && kind != 4) return interp_fast_meta_nil();
      environment = (uint8_t *)interp_fast_load_u64(value_ptr, 40);
      current = member;
    }
    return interp_fast_meta_nil();
  }

  /* A lexed qualified atom (`a.b`) is split in place and resolved through
   * the environment captured by each module/type value. */
  uint64_t start = interp_fast_load_u64(node, 8);
  uint64_t length = interp_fast_load_u64(node, 16);
  uint64_t index = 0;
  while (index <= length) {
    uint64_t scan = index;
    while (scan < length && data[start + scan] != '.') scan++;
    uint64_t segment_length = scan - index;
    if (!segment_length) return interp_fast_meta_nil();
    LainirValue span_args[4] = {
      lainir_value_addr(environment), args[1],
      lainir_value_bits(start + index, 64),
      lainir_value_bits(segment_length, 64)
    };
    LainirValue value = interp_fast_meta_lookup_span(interp, span_args, 0);
    if (interp->error) return value;
    uint8_t *value_ptr = (uint8_t *)interp_fast_addr(&value);
    if (!value_ptr || interp_fast_load_u32(value_ptr, 0) == 0)
      return value;
    if (scan >= length) return value;
    uint32_t kind = interp_fast_load_u32(value_ptr, 0);
    if (kind != 2 && kind != 4) return interp_fast_meta_nil();
    environment = (uint8_t *)interp_fast_load_u64(value_ptr, 40);
    index = scan + 1;
  }
  return interp_fast_meta_nil();
}

static int interp_fast_local_matches_group(const uint8_t *data,
                                           const uint8_t *group,
                                           const uint8_t *node,
                                           uint32_t depth) {
  if (!group || !node || depth > 4096) return 0;
  const uint8_t *statement =
      (const uint8_t *)interp_fast_load_u64(group, 24);
  while (statement) {
    if (interp_fast_load_u32(statement, 0) == 0) return 0;
    if (interp_fast_load_u32(statement, 0) == 1 &&
        interp_fast_load_u64(statement, 16) == 3 &&
        memcmp(data + interp_fast_load_u64(statement, 8), "let", 3) == 0) {
      const uint8_t *name =
          (const uint8_t *)interp_fast_load_u64(statement, 40);
      if (name && interp_fast_load_u64(name, 16) ==
                       interp_fast_load_u64(node, 16) &&
          memcmp(data + interp_fast_load_u64(name, 8),
                 data + interp_fast_load_u64(node, 8),
                 (size_t)interp_fast_load_u64(node, 16)) == 0)
        return 1;
    }
    if (interp_fast_load_u32(statement, 0) == 2 &&
        interp_fast_local_matches_group(data, statement, node, depth + 1))
      return 1;
    statement = (const uint8_t *)interp_fast_load_u64(statement, 40);
  }
  return 0;
}

static int interp_try_fast_builtin(LainirInterpreter *interp,
                                    L1Subroutine *sub,
                                    const LainirValue *args,
                                    uint32_t arg_count,
                                    LainirValue *result) {
  if (!interp_fast_enabled(interp) || !sub || !sub->name || !result)
    return 0;
  const char *name = interp_fast_name(sub->name);
  interp->fast_active_name = name;
  if (interp->trace_meta_enabled && strstr(name, "meta")) {
    static unsigned trace_meta_names = 0;
    if (trace_meta_names++ < 128)
      fprintf(stderr, "fast_meta_name[%u] %s argc=%u\n",
              trace_meta_names, name, arg_count);
  }
  if (interp->trace_fast_names_enabled) {
    static unsigned trace_names = 0;
    if (trace_names++ < 256)
      fprintf(stderr, "fast_name[%u] %s argc=%u\n", trace_names, name, arg_count);
  }

  if (interp_fast_suffix(name, "_sb_byte_at") && arg_count == 2) {
    uint8_t *buffer = (uint8_t *)interp_fast_addr(&args[0]);
    uint8_t byte = 0;
    if (!interp_fast_sb_byte(buffer, interp_fast_bits(&args[1]), &byte)) {
      interp_trap(interp, "load from null");
      return 1;
    }
    *result = lainir_value_bits(byte, 32);
    return 1;
  }
  if (interp_fast_suffix(name, "_sb_load64_at") && arg_count == 2) {
    uint8_t *buffer = (uint8_t *)interp_fast_addr(&args[0]);
    if (!buffer) { interp_trap(interp, "load from null"); return 1; }
    *result = lainir_value_bits(
        interp_fast_load_u64(buffer, interp_fast_bits(&args[1])), 64);
    return 1;
  }
  if (interp_fast_suffix(name, "_sb_load_addr_at") && arg_count == 2) {
    uint8_t *buffer = (uint8_t *)interp_fast_addr(&args[0]);
    if (!buffer) { interp_trap(interp, "load from null"); return 1; }
    *result = lainir_value_addr((void *)interp_fast_load_u64(
        buffer, interp_fast_bits(&args[1])));
    return 1;
  }
  if (interp_fast_suffix(name, "_sb_load8_at") && arg_count == 2) {
    uint8_t *buffer = (uint8_t *)interp_fast_addr(&args[0]);
    if (!buffer) { interp_trap(interp, "load from null"); return 1; }
    *result = lainir_value_bits(buffer[interp_fast_bits(&args[1])], 8);
    return 1;
  }
  if (interp_fast_suffix(name, "_sb_span_eq_buf") && arg_count == 5) {
    uint8_t *left = (uint8_t *)interp_fast_addr(&args[0]);
    uint8_t *right = (uint8_t *)interp_fast_addr(&args[2]);
    uint64_t left_start = interp_fast_bits(&args[1]);
    uint64_t right_start = interp_fast_bits(&args[3]);
    uint64_t length = interp_fast_bits(&args[4]);
    if (!left || !right) { interp_trap(interp, "load from null"); return 1; }
    int same = 1;
    for (uint64_t index = 0; index < length; index++) {
      uint8_t left_byte = 0;
      uint8_t right_byte = 0;
      if (!interp_fast_sb_byte(left, left_start + index, &left_byte) ||
          !interp_fast_sb_byte(right, right_start + index, &right_byte)) {
        interp_trap(interp, "load from null");
        return 1;
      }
      if (left_byte != right_byte) { same = 0; break; }
    }
    *result = lainir_value_bits(same, 1);
    return 1;
  }

  if (strcmp(name, "tool_byte_at") == 0 && arg_count == 2) {
    uint8_t *data = (uint8_t *)interp_fast_addr(&args[0]);
    if (!data) { interp_trap(interp, "load from null"); return 1; }
    *result = lainir_value_bits(data[interp_fast_bits(&args[1])], 8);
    return 1;
  }
  if (strcmp(name, "tool_source_data") == 0 && arg_count == 1) {
    uint8_t *source = (uint8_t *)interp_fast_addr(&args[0]);
    if (!source) { interp_trap(interp, "load from null"); return 1; }
    *result = lainir_value_addr((void *)interp_fast_load_u64(source, 0));
    return 1;
  }
  if (strcmp(name, "tool_source_length") == 0 && arg_count == 1) {
    uint8_t *source = (uint8_t *)interp_fast_addr(&args[0]);
    if (!source) { interp_trap(interp, "load from null"); return 1; }
    *result = lainir_value_bits(interp_fast_load_u64(source, 8), 64);
    return 1;
  }
  if (strcmp(name, "raw_is_nil") == 0 && arg_count == 1) {
    uint8_t *node = (uint8_t *)interp_fast_addr(&args[0]);
    if (!node) { interp_trap(interp, "load from null"); return 1; }
    *result = lainir_value_bits(node[0] == 0, 1);
    return 1;
  }
  if (strcmp(name, "raw_node_kind") == 0 && arg_count == 1) {
    uint8_t *node = (uint8_t *)interp_fast_addr(&args[0]);
    if (!node) { interp_trap(interp, "load from null"); return 1; }
    *result = lainir_value_bits(interp_fast_load_u32(node, 0), 32);
    return 1;
  }
  if (strcmp(name, "raw_node_delimiter") == 0 && arg_count == 1) {
    uint8_t *node = (uint8_t *)interp_fast_addr(&args[0]);
    if (!node) { interp_trap(interp, "load from null"); return 1; }
    *result = lainir_value_bits(interp_fast_load_u32(node, 4), 32);
    return 1;
  }
  if (strcmp(name, "raw_node_start") == 0 && arg_count == 1) {
    uint8_t *node = (uint8_t *)interp_fast_addr(&args[0]);
    if (!node) { interp_trap(interp, "load from null"); return 1; }
    *result = lainir_value_bits(interp_fast_load_u64(node, 8), 64);
    return 1;
  }
  if (strcmp(name, "raw_node_length") == 0 && arg_count == 1) {
    uint8_t *node = (uint8_t *)interp_fast_addr(&args[0]);
    if (!node) { interp_trap(interp, "load from null"); return 1; }
    *result = lainir_value_bits(interp_fast_load_u64(node, 16), 64);
    return 1;
  }
  if ((strcmp(name, "raw_node_first") == 0 ||
       strcmp(name, "raw_node_last") == 0 ||
       strcmp(name, "raw_node_next") == 0) && arg_count == 1) {
    uint8_t *node = (uint8_t *)interp_fast_addr(&args[0]);
    if (!node) { interp_trap(interp, "load from null"); return 1; }
    size_t offset = strcmp(name, "raw_node_first") == 0 ? 24 :
                    strcmp(name, "raw_node_last") == 0 ? 32 : 40;
    *result = lainir_value_addr((void *)interp_fast_load_u64(node, offset));
    return 1;
  }
  if ((strcmp(name, "meta_env_is_nil") == 0 ||
       strcmp(name, "meta_env_binding_is_nil") == 0) && arg_count == 1) {
    uint8_t *object = (uint8_t *)interp_fast_addr(&args[0]);
    if (!object) { interp_trap(interp, "load from null"); return 1; }
    *result = lainir_value_bits(interp_fast_load_u32(object, 0) == 0, 1);
    return 1;
  }
  if (interp->fast_scanners_enabled &&
      interp_fast_suffix(name, "_scan_top_op") && arg_count == 4) {
    uint8_t *source = (uint8_t *)interp_fast_addr(&args[0]);
    uint64_t start = interp_fast_bits(&args[1]);
    uint64_t end = interp_fast_bits(&args[2]);
    uint64_t op = interp_fast_bits(&args[3]);
    uint64_t found = end;
    if (!source) { interp_trap(interp, "load from null"); return 1; }
    if (!interp_fast_scan_delimited(source, start, end, 0, (int)op, &found))
      return 0;
    *result = lainir_value_bits(found, 64);
    return 1;
  }
  if (interp->fast_scanners_enabled &&
      interp_fast_suffix(name, "_is_space") && arg_count == 1) {
    uint64_t byte = interp_fast_bits(&args[0]);
    *result = lainir_value_bits(byte == 32 || byte == 10, 1);
    return 1;
  }
  if (interp->fast_scanners_enabled &&
      interp_fast_suffix(name, "_skip_line_comment") && arg_count == 2) {
    uint8_t *source = (uint8_t *)interp_fast_addr(&args[0]);
    uint64_t index = interp_fast_bits(&args[1]);
    uint64_t length = 0;
    uint64_t found = index;
    if (!source) { interp_trap(interp, "load from null"); return 1; }
    uint64_t header = interp_fast_load_u64(source, 0);
    length = (header == UINT64_MAX || header == UINT32_MAX)
        ? interp_fast_load_u64(source, 8) : header;
    if (!interp_fast_skip_line_comment(source, index, length, &found)) return 0;
    *result = lainir_value_bits(found, 64);
    return 1;
  }
  if (interp->fast_scanners_enabled &&
      interp_fast_suffix(name, "_skip_space") && arg_count == 2) {
    uint8_t *source = (uint8_t *)interp_fast_addr(&args[0]);
    uint64_t index = interp_fast_bits(&args[1]);
    uint64_t length = 0;
    uint64_t found = index;
    if (!source) { interp_trap(interp, "load from null"); return 1; }
    uint64_t header = interp_fast_load_u64(source, 0);
    length = (header == UINT64_MAX || header == UINT32_MAX)
        ? interp_fast_load_u64(source, 8) : header;
    if (!interp_fast_skip_space(source, index, length, &found)) return 0;
    *result = lainir_value_bits(found, 64);
    return 1;
  }
  if (interp->fast_scanners_enabled &&
      interp_fast_suffix(name, "_scan_semi") && arg_count == 3) {
    uint8_t *source = (uint8_t *)interp_fast_addr(&args[0]);
    uint64_t start = interp_fast_bits(&args[1]);
    uint64_t end = interp_fast_bits(&args[2]);
    uint64_t found = end;
    if (!source) { interp_trap(interp, "load from null"); return 1; }
    if (!interp_fast_scan_delimited(source, start, end, 1, 0, &found))
      return 0;
    *result = lainir_value_bits(found, 64);
    return 1;
  }
  if (interp->fast_scanners_enabled &&
      interp_fast_suffix(name, "_scan_brace") && arg_count == 3) {
    uint8_t *source = (uint8_t *)interp_fast_addr(&args[0]);
    uint64_t start = interp_fast_bits(&args[1]);
    uint64_t end = interp_fast_bits(&args[2]);
    uint64_t found = end;
    if (!source) { interp_trap(interp, "load from null"); return 1; }
    if (!interp_fast_scan_delimited(source, start, end, 2, 0, &found))
      return 0;
    *result = lainir_value_bits(found, 64);
    return 1;
  }
  if (interp_fast_suffix(name, "_sb_span_equals") && arg_count == 5) {
    uint8_t *source = (uint8_t *)interp_fast_addr(&args[0]);
    uint64_t start = interp_fast_bits(&args[1]);
    uint64_t length = interp_fast_bits(&args[2]);
    const uint8_t *literal =
        (const uint8_t *)interp_fast_addr(&args[3]);
    uint64_t literal_length = interp_fast_bits(&args[4]);
    if (!source || !literal) {
      interp_trap(interp, "load from null");
      return 1;
    }
    if (length != literal_length) {
      *result = lainir_value_bits(0, 1);
      return 1;
    }
    for (uint64_t index = 0; index < length; index++) {
      uint8_t byte = 0;
      if (!interp_fast_sb_byte(source, start + index, &byte)) {
        interp_trap(interp, "load from null");
        return 1;
      }
      if (byte != literal[index]) {
        *result = lainir_value_bits(0, 1);
        return 1;
      }
    }
    *result = lainir_value_bits(1, 1);
    return 1;
  }
  if (strcmp(name, "meta_value_kind") == 0 && arg_count == 1) {
    uint8_t *value = (uint8_t *)interp_fast_addr(&args[0]);
    if (!value) { interp_trap(interp, "load from null"); return 1; }
    *result = lainir_value_bits(interp_fast_load_u32(value, 0), 32);
    return 1;
  }
  if ((interp->fast_meta_scan_enabled ||
       getenv("LAINIR_FAST_META_LATEST")) &&
      interp_fast_suffix(name, "_meta_lookup_kind_latest") && arg_count == 6) {
    return interp_fast_meta_table_lookup(interp, args, interp_fast_bits(&args[5]),
                                          1, 1, result);
  }
  if ((interp->fast_meta_scan_enabled ||
       getenv("LAINIR_FAST_META_KIND")) &&
      interp_fast_suffix(name, "_meta_lookup_kind") && arg_count == 6) {
    return interp_fast_meta_table_lookup(interp, args, interp_fast_bits(&args[5]),
                                          1, 0, result);
  }
  if ((interp->fast_meta_scan_enabled ||
       getenv("LAINIR_FAST_META_LOOKUP")) &&
      interp_fast_suffix(name, "_meta_lookup") && arg_count == 5) {
    return interp_fast_meta_table_lookup(interp, args, 0, 0, 0, result);
  }
  if (interp_fast_suffix(name, "_meta_index_prepare") && arg_count == 3) {
    uint8_t *table = (uint8_t *)interp_fast_addr(&args[0]);
    uint8_t *source = (uint8_t *)interp_fast_addr(&args[2]);
    uint8_t *index_base;
    uint64_t old_source;
    uint64_t indexed;
    uint64_t generation;
    uint64_t generation_base;
    uint64_t rows;
    if (!table || !source) {
      interp_trap(interp, "load from null");
      return 1;
    }
    index_base = (uint8_t *)interp_fast_load_u64(table, 524184);
    /* Older generated compilers do not have the external index pointer. */
    if (!index_base) return 0;
    old_source = interp_fast_load_u64(table, 524168);
    indexed = interp_fast_load_u64(table, 524176);
    generation = interp_fast_load_u64(table, 524192);
    if (old_source != (uint64_t)(uintptr_t)source) {
      interp_fast_store_u64(table, 524168, (uint64_t)(uintptr_t)source);
      indexed = 0;
      generation++;
      interp_fast_store_u64(table, 524176, 0);
      interp_fast_store_u64(table, 524192, generation);
    }
    generation_base = generation * 4294967296ULL;
    rows = interp_fast_load_u64(table, 524280);
    while (indexed < rows) {
      uint64_t row = indexed + 1;
      uint64_t row_offset = 655360ULL + (row - 1) * 48ULL;
      uint64_t ns = interp_fast_load_u64(table, row_offset + 8);
      uint64_t nl = interp_fast_load_u64(table, row_offset + 16);
      uint64_t hash = nl * 131ULL + 2ULL;
      uint8_t first = 0;
      uint8_t last = 0;
      if (nl != 0) {
        if (!interp_fast_sb_byte(source, ns, &first) ||
            !interp_fast_sb_byte(source, ns + nl - 1, &last)) {
          interp_trap(interp, "load from null");
          return 1;
        }
        hash += (uint64_t)first * 17ULL + last;
      }
      uint64_t expected = generation_base + hash + 1ULL;
      uint64_t slot = hash % 16384ULL;
      for (uint64_t probes = 0; probes < 16384ULL; probes++) {
        uint64_t offset = (slot + probes) * 16ULL;
        uint64_t stored = interp_fast_load_u64(index_base, offset);
        if (stored == 0 || stored < generation_base) {
          interp_fast_store_u64(index_base, offset, expected);
          interp_fast_store_u64(index_base, offset + 8, row);
          break;
        }
        if (stored == expected) {
          uint64_t prior = interp_fast_load_u64(index_base, offset + 8);
          uint64_t prior_offset = 655360ULL + (prior - 1) * 48ULL;
          uint64_t prior_ns = interp_fast_load_u64(table, prior_offset + 8);
          uint64_t prior_nl = interp_fast_load_u64(table, prior_offset + 16);
          if (prior_nl == nl &&
              interp_fast_span_equal(source, prior_ns, source, ns, nl))
            break;
        }
      }
      indexed = row;
      interp_fast_store_u64(table, 524176, indexed);
    }
    *result = lainir_value_unit();
    return 1;
  }
  if (interp_fast_suffix(name, "_meta_row_field") && arg_count == 4) {
    uint8_t *table = (uint8_t *)interp_fast_addr(&args[0]);
    uint64_t row = interp_fast_bits(&args[2]);
    uint64_t field = interp_fast_bits(&args[3]);
    if (!table) { interp_trap(interp, "load from null"); return 1; }
    if (row == 0) {
      *result = lainir_value_bits(0, 64);
    } else {
      uint64_t offset = 655360ULL + (row - 1) * 48ULL + field * 8ULL;
      *result = lainir_value_bits(interp_fast_load_u64(table, offset), 64);
    }
    return 1;
  }
  if (getenv("LAINIR_FAST_META_INDEX_LOOKUP") &&
      interp_fast_suffix(name, "_meta_index_lookup") && arg_count == 5) {
    uint8_t *table = (uint8_t *)interp_fast_addr(&args[0]);
    uint8_t *source = (uint8_t *)interp_fast_addr(&args[2]);
    uint64_t start = interp_fast_bits(&args[3]);
    uint64_t length = interp_fast_bits(&args[4]);
    if (!table || !source) {
      interp_trap(interp, "load from null");
      return 1;
    }
    uint64_t row = interp_fast_meta_index_lookup(table, source, start, length);
    /* UINT64_MAX is the helper's "old artifact / invalid buffer" sentinel;
     * let the original L1 procedure handle that layout. */
    if (row == UINT64_MAX) return 0;
    *result = lainir_value_bits(row, 64);
    return 1;
  }
  if (interp_fast_suffix(name, "_sb_len") && arg_count == 1) {
    uint8_t *buffer = (uint8_t *)interp_fast_addr(&args[0]);
    uint64_t header;
    if (!buffer) { interp_trap(interp, "load from null"); return 1; }
    header = interp_fast_load_u64(buffer, 0);
    if (header == UINT64_MAX || header == UINT32_MAX)
      header = interp_fast_load_u64(buffer, 8);
    *result = lainir_value_bits(header, 64);
    return 1;
  }
  if (strcmp(name, "meta_env_hash_span") == 0 && arg_count == 3) {
    uint8_t *source = (uint8_t *)interp_fast_addr(&args[0]);
    if (!source) { interp_trap(interp, "load from null"); return 1; }
    uint8_t *data = (uint8_t *)interp_fast_load_u64(source, 0);
    uint64_t start = interp_fast_bits(&args[1]);
    uint64_t length = interp_fast_bits(&args[2]);
    uint64_t hash = length;
    if (hash >= 256) hash -= 256;
    for (uint64_t index = 0; index < length; index++) {
      hash += data[start + index];
      if (hash >= 256) hash -= 256;
    }
    *result = lainir_value_bits(hash, 64);
    return 1;
  }
  if (strcmp(name, "meta_env_lookup_span") == 0 && arg_count == 4) {
    *result = interp_fast_meta_lookup_span(interp, args, 0);
    return 1;
  }
  if (strcmp(name, "meta_env_lookup_path") == 0 && arg_count == 3) {
    *result = interp_fast_meta_lookup_path(interp, args);
    return 1;
  }
  if (strcmp(name, "program_local_matches_group") == 0 && arg_count == 3) {
    uint8_t *source = (uint8_t *)interp_fast_addr(&args[0]);
    uint8_t *group = (uint8_t *)interp_fast_addr(&args[1]);
    uint8_t *node = (uint8_t *)interp_fast_addr(&args[2]);
    if (!source || !group || !node) {
      interp_trap(interp, "load from null");
      return 1;
    }
    uint8_t *data = (uint8_t *)interp_fast_load_u64(source, 0);
    *result = lainir_value_bits(
        interp_fast_local_matches_group(data, group, node, 0), 1);
    return 1;
  }
  if (interp_fast_leaves_enabled(interp) && strcmp(name, "program_nil") == 0 && arg_count == 0) {
    *result = interp_fast_meta_nil();
    return 1;
  }
  if (interp_fast_leaves_enabled(interp) && strcmp(name, "program_function_is_nil") == 0 && arg_count == 1) {
    uint8_t *function = (uint8_t *)interp_fast_addr(&args[0]);
    if (!function) { interp_trap(interp, "load from null"); return 1; }
    *result = lainir_value_bits(interp_fast_load_u64(function, 8) == 0, 1);
    return 1;
  }
  if (interp_fast_leaves_enabled(interp) && strcmp(name, "program_function_environment") == 0 && arg_count == 1) {
    uint8_t *function = (uint8_t *)interp_fast_addr(&args[0]);
    if (!function) { interp_trap(interp, "load from null"); return 1; }
    *result = lainir_value_addr((void *)interp_fast_load_u64(function, 88));
    return 1;
  }
  if (interp_fast_leaves_enabled(interp) && strcmp(name, "program_name_prefix_length") == 0 && arg_count == 2) {
    uint8_t *source = (uint8_t *)interp_fast_addr(&args[0]);
    uint8_t *node = (uint8_t *)interp_fast_addr(&args[1]);
    if (!source || !node) { interp_trap(interp, "load from null"); return 1; }
    uint8_t *data = (uint8_t *)interp_fast_load_u64(source, 0);
    uint64_t start = interp_fast_load_u64(node, 8);
    uint64_t length = interp_fast_load_u64(node, 16);
    /* Large spans can be whole source groups rather than names.  Leave those
     * to the step-counted L1 implementation so the native loop cannot hide a
     * pathological scan behind the bootstrap limit. */
    if (length > 4096) return 0;
    uint64_t prefix = 0;
    for (uint64_t index = 0; index < length; index++) {
      if (data[start + index] == '.' && prefix == 0) prefix = index;
    }
    *result = lainir_value_bits(prefix, 64);
    return 1;
  }
  if (interp_fast_leaves_enabled(interp) && (strcmp(name, "program_is_dot") == 0 ||
       strcmp(name, "program_is_dot_member") == 0) && arg_count == 2) {
    uint8_t *source = (uint8_t *)interp_fast_addr(&args[0]);
    uint8_t *node = (uint8_t *)interp_fast_addr(&args[1]);
    if (!source || !node) { interp_trap(interp, "load from null"); return 1; }
    uint8_t *data = (uint8_t *)interp_fast_load_u64(source, 0);
    uint64_t start = interp_fast_load_u64(node, 8);
    uint64_t length = interp_fast_load_u64(node, 16);
    int match = interp_fast_load_u32(node, 0) == 1;
    if (strcmp(name, "program_is_dot") == 0)
      match = match && length == 1 && data[start] == '.';
    else
      match = match && length >= 2 && data[start] == '.';
    *result = lainir_value_bits(match, 1);
    return 1;
  }
  if (interp_fast_leaves_enabled(interp) && strcmp(name, "program_operand_name") == 0 && arg_count == 2) {
    uint8_t *source = (uint8_t *)interp_fast_addr(&args[0]);
    uint8_t *current = (uint8_t *)interp_fast_addr(&args[1]);
    if (!source || !current) { interp_trap(interp, "load from null"); return 1; }
    uint8_t *data = (uint8_t *)interp_fast_load_u64(source, 0);
    for (uint32_t depth = 0; current && depth < 256; depth++) {
      uint8_t *colon1 = (uint8_t *)interp_fast_load_u64(current, 40);
      if (colon1 && interp_fast_load_u32(colon1, 0) == 1 &&
          interp_fast_load_u64(colon1, 16) == 1 &&
          data[interp_fast_load_u64(colon1, 8)] == ':') {
        uint8_t *colon2 = (uint8_t *)interp_fast_load_u64(colon1, 40);
        if (colon2 && interp_fast_load_u32(colon2, 0) == 1 &&
            interp_fast_load_u64(colon2, 16) == 1 &&
            data[interp_fast_load_u64(colon2, 8)] == ':') {
          uint8_t *member = (uint8_t *)interp_fast_load_u64(colon2, 40);
          if (!member || interp_fast_load_u32(member, 0) != 1) break;
          current = member;
          continue;
        }
      }
      uint8_t *dot = (uint8_t *)interp_fast_load_u64(current, 40);
      if (!dot) break;
      uint64_t dot_length = interp_fast_load_u64(dot, 16);
      uint64_t dot_start = interp_fast_load_u64(dot, 8);
      if (interp_fast_load_u32(dot, 0) == 1 && dot_length >= 2 &&
          data[dot_start] == '.') {
        current = dot;
        continue;
      }
      if (interp_fast_load_u32(dot, 0) != 1 || dot_length != 1 ||
          data[dot_start] != '.') break;
      uint8_t *member = (uint8_t *)interp_fast_load_u64(dot, 40);
      if (!member || interp_fast_load_u32(member, 0) != 1) break;
      current = member;
    }
    *result = lainir_value_addr(current);
    return 1;
  }
  if (strcmp(name, "meta_env_lookup_local") == 0 && arg_count == 3) {
    LainirValue local_args[4] = {args[0], args[1], args[2], args[2]};
    local_args[3].as.bits = interp_fast_bits(&args[2]);
    /* The local API receives a RawAst name, so resolve its start/length
     * before using the common span lookup implementation. */
    uint8_t *node = (uint8_t *)interp_fast_addr(&args[2]);
    if (!node) { interp_trap(interp, "load from null"); return 1; }
    local_args[2] = lainir_value_bits(interp_fast_load_u64(node, 8), 64);
    local_args[3] = lainir_value_bits(interp_fast_load_u64(node, 16), 64);
    /* Repackage as environment, source, start, length. */
    LainirValue span_args[4] = {
      args[0], args[1], local_args[2], local_args[3]
    };
    *result = interp_fast_meta_lookup_span(interp, span_args, 1);
    return 1;
  }
  if (strcmp(name, "meta_atom_equal") == 0 && arg_count == 4) {
    uint8_t *source = (uint8_t *)interp_fast_addr(&args[0]);
    uint8_t *node = (uint8_t *)interp_fast_addr(&args[1]);
    uint8_t *text = (uint8_t *)interp_fast_addr(&args[2]);
    uint64_t length = interp_fast_bits(&args[3]);
    if (!source || !node || !text) {
      interp_trap(interp, "load from null"); return 1;
    }
    int same = interp_fast_load_u32(node, 0) == 1 &&
               interp_fast_load_u64(node, 16) == length;
    if (same) {
      uint8_t *data = (uint8_t *)interp_fast_load_u64(source, 0);
      uint64_t start = interp_fast_load_u64(node, 8);
      same = memcmp(data + start, text, (size_t)length) == 0;
    }
    *result = lainir_value_bits(same, 1);
    return 1;
  }
  /* Syntax token comparisons are a hot path during archive elaboration.  The
   * L1 implementation walks each byte through memory.byte_at; use the same
   * record layout directly when the scanner fast paths are enabled. */
  if (interp->fast_scanners_enabled &&
      interp_fast_suffix(name, "_token_equal") && arg_count == 5) {
    uint8_t *store = (uint8_t *)interp_fast_addr(&args[0]);
    uint64_t unit_id = interp_fast_bits(&args[1]);
    uint64_t node_id = interp_fast_bits(&args[2]);
    const uint8_t *literal =
        (const uint8_t *)interp_fast_addr(&args[3]);
    uint64_t literal_length = interp_fast_bits(&args[4]);
    int same = 0;
    if (store && literal) {
      uint8_t *units_vec = (uint8_t *)interp_fast_load_u64(store, 0);
      uint8_t *units_storage = units_vec
          ? (uint8_t *)interp_fast_load_u64(units_vec, 0) : NULL;
      uint8_t *unit = units_storage
          ? (uint8_t *)interp_fast_load_u64(units_storage + 24 + unit_id * 8, 0)
          : NULL;
      uint8_t *nodes_vec = unit
          ? (uint8_t *)interp_fast_load_u64(unit, 8) : NULL;
      uint8_t *nodes_storage = nodes_vec
          ? (uint8_t *)interp_fast_load_u64(nodes_vec, 0) : NULL;
      uint8_t *node = nodes_storage
          ? (uint8_t *)interp_fast_load_u64(nodes_storage + 24 + node_id * 8, 0)
          : NULL;
      if (getenv("LAINIR_TRACE_FAST_TOKEN_EQUAL")) {
        fprintf(stderr, " ptrs store=%p units_vec=%p unit=%p nodes_vec=%p node=%p source=%p span=%p kind=%u\n",
                (void *)store, (void *)units_vec, (void *)unit,
                (void *)nodes_vec, (void *)node,
                unit ? (void *)interp_fast_load_u64(unit, 0) : NULL,
                node ? (void *)interp_fast_load_u64(node, 12) : NULL,
                node ? interp_fast_load_u32(node, 0) : 0);
      }
      if (unit && node && interp_fast_load_u32(node, 0) == 1 &&
          node[60] == 0) {
        uint8_t *span = (uint8_t *)interp_fast_load_u64(node, 12);
        uint8_t *source_span = (uint8_t *)interp_fast_load_u64(unit, 0);
        uint8_t *source = source_span
            ? (uint8_t *)interp_fast_load_u64(source_span, 0) : NULL;
        if (span && source) {
          uint64_t start = interp_fast_load_u64(span, 0);
          uint64_t end = interp_fast_load_u64(span, 8);
          if (end >= start && end - start == literal_length)
            same = memcmp(source + start, literal,
                          (size_t)literal_length) == 0;
        }
      }
    }
    if (getenv("LAINIR_TRACE_FAST_TOKEN_EQUAL")) {
      fprintf(stderr, "fast_token_equal unit=%llu node=%llu len=%llu result=%d\n",
              (unsigned long long)unit_id, (unsigned long long)node_id,
              (unsigned long long)literal_length, same);
      fflush(stderr);
    }
    *result = lainir_value_bits(same, 1);
    return 1;
  }
  return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * Subroutine call
 * ═══════════════════════════════════════════════════════════════ */

static void interp_destroy_continuation(LainirContinuation *continuation) {
  LainirNestedContinuation *nested;
  if (!continuation) return;
  nested = continuation->nested_top;
  while (nested) {
    LainirNestedContinuation *parent = nested->parent;
    interp_free_frame(nested->frame);
    free(nested->frame);
    free(nested);
    nested = parent;
  }
  while (continuation->pending_nested_results) {
    LainirPendingNestedResult *pending = continuation->pending_nested_results;
    continuation->pending_nested_results = pending->next;
    free(pending);
  }
  free(continuation->expr_cache);
  interp_free_frame(&continuation->frame);
  free(continuation);
}

static int interp_resume_nested_continuation(LainirInterpreter *interp) {
  LainirContinuation *continuation = interp->continuation;
  if (!continuation) return 1;
  while (continuation->nested_top) {
    LainirNestedContinuation *nested = continuation->nested_top;
    LainirFrame *frame = nested->frame;
    int depth = 1;
    for (LainirNestedContinuation *parent = nested->parent;
         parent; parent = parent->parent)
      depth++;
    interp->active_frame = frame;
    interp->call_depth = (uint32_t)(depth + 1);
    interp->should_return = 0;
    interp->return_value = lainir_value_unit();
    if (interp->vm_control &&
        !lainir_vm_control_push_frame(
            interp->vm_control, interp->vm_owner,
            (uint64_t)(uintptr_t)frame->sub,
            (uint64_t)(uintptr_t)frame->sub->blocks,
            (uint64_t)(uintptr_t)frame)) {
      interp_trap(interp, "VM nested continuation frame rejected");
      return 0;
    }
    while (nested->block) {
      interp->continuation_tracking = 0;
      interp_exec_block(interp, frame, nested->block);
      if (interp->vm_slice_yielded || interp->vm_blocked) {
        nested->block = interp->active_block;
        nested->inst = interp->active_inst;
        (void)lainir_vm_control_pop_frame(interp->vm_control,
                                          interp->vm_owner);
        interp->active_frame = nested->parent
            ? nested->parent->frame : &continuation->frame;
        interp->call_depth = (uint32_t)(nested->parent ? depth : 1);
        return 0;
      }
      if (interp->error) return 0;
      if (interp->should_return) break;
      nested->block = nested->block->next;
      nested->inst = NULL;
    }
    if (!interp->should_return) {
      interp_trap(interp, "nested procedure did not return");
      return 0;
    }
    if (interp->return_value.kind == LAINIR_VALUE_ADDR &&
        interp_frame_owns_addr(frame, interp->return_value.as.addr)) {
      interp_trap(interp, "#alloca address escaped nested continuation");
      return 0;
    }
    if (!interp_queue_pending_nested(
            interp, nested->parent ? nested->parent->frame : &continuation->frame,
            nested->sub, interp->return_value)) {
      interp_trap(interp, "out of memory for nested pending result");
      return 0;
    }
    continuation->nested_top = nested->parent;
    (void)lainir_vm_control_pop_frame(interp->vm_control, interp->vm_owner);
    interp_free_frame(frame);
    free(frame);
    free(nested);
    if (continuation->nested_top) continue;
    interp->active_frame = &continuation->frame;
    interp->call_depth = 1;
    /* The child return value is already queued for the root expression.  Do
     * not let the child's return flag terminate the root before it resumes
     * at its saved instruction and consumes that pending result. */
    interp->should_return = 0;
    interp->return_value = lainir_value_unit();
    return 1;
  }
  return 1;
}

static LainirValue interp_call_root_resumable(
    LainirInterpreter *interp, L1Subroutine *sub,
    const LainirValue *args, uint32_t arg_count) {
  LainirContinuation *continuation = NULL;
  void *saved_state = lainir_vm_control_backend_state(interp->vm_control);
  if (saved_state) {
    continuation = (LainirContinuation *)saved_state;
    if (continuation->module != interp->module || continuation->entry != sub) {
      interp_trap(interp, "VM continuation entry mismatch");
      return lainir_value_unit();
    }
  } else {
    continuation = calloc(1, sizeof(*continuation));
    if (!continuation) {
      interp_trap(interp, "out of memory");
      return lainir_value_unit();
    }
    continuation->module = interp->module;
    continuation->entry = sub;
    continuation->frame.sub = sub;
    continuation->frame.args = continuation->frame.args_inline;
    continuation->frame.locals = continuation->frame.locals_inline;
    continuation->frame.local_cap =
        sizeof(continuation->frame.locals_inline) /
        sizeof(continuation->frame.locals_inline[0]);
    if (arg_count) {
      if (arg_count > sizeof(continuation->frame.args_inline) /
                          sizeof(continuation->frame.args_inline[0])) {
        continuation->frame.args = calloc(arg_count, sizeof(LainirValue));
        continuation->frame.args_heap = 1;
      }
      if (!continuation->frame.args) {
        interp_destroy_continuation(continuation);
        interp_trap(interp, "out of memory");
        return lainir_value_unit();
      }
      for (uint32_t i = 0; i < arg_count; i++) {
        continuation->frame.args[i] = interp_coerce_physical(
            interp, args[i], i < sub->param_count ? sub->param_tys[i] : NULL);
        if (interp->error) {
          interp_destroy_continuation(continuation);
          return lainir_value_unit();
        }
      }
    }
    continuation->frame.arg_count = arg_count;
    if (!lainir_vm_control_set_backend_state(
            interp->vm_control, interp->vm_owner, continuation) ||
        !lainir_vm_control_set_backend_state_destructor(
            interp->vm_control, interp->vm_owner,
            (LainirVmBackendStateFree)interp_destroy_continuation) ||
        !lainir_vm_control_push_frame(
            interp->vm_control, interp->vm_owner,
            (uint64_t)(uintptr_t)sub, (uint64_t)(uintptr_t)sub->blocks,
            (uint64_t)(uintptr_t)&continuation->frame)) {
      (void)lainir_vm_control_set_backend_state(
          interp->vm_control, interp->vm_owner, NULL);
      interp_destroy_continuation(continuation);
      interp_trap(interp, "VM continuation setup rejected");
      return lainir_value_unit();
    }
  }

  interp->continuation = continuation;
  interp->active_frame = &continuation->frame;
  interp->call_depth = 1;
  interp->should_return = 0;
  interp->return_value = lainir_value_unit();

  if (continuation->nested_top &&
      !interp_resume_nested_continuation(interp))
    return lainir_value_unit();

  L1Block *block = continuation->next_block
                       ? continuation->next_block : sub->blocks;
  while (block) {
    interp->continuation_tracking = 1;
    interp_exec_block(interp, &continuation->frame, block);
    interp->continuation_tracking = 0;
    if (interp->vm_slice_yielded || interp->vm_blocked)
      return lainir_value_unit();
    if (interp->error || interp->should_return) break;
    block = block->next;
  }
  if (interp->error) {
    (void)lainir_vm_control_set_backend_state(
        interp->vm_control, interp->vm_owner, NULL);
    interp_destroy_continuation(continuation);
    return lainir_value_unit();
  }
  LainirValue result = interp->return_value;
  if (!interp->should_return) {
    interp_trap(interp, "procedure did not return");
    (void)lainir_vm_control_set_backend_state(
        interp->vm_control, interp->vm_owner, NULL);
    interp_destroy_continuation(continuation);
    return lainir_value_unit();
  }
  (void)lainir_vm_control_pop_frame(interp->vm_control, interp->vm_owner);
  (void)lainir_vm_control_set_backend_state(
      interp->vm_control, interp->vm_owner, NULL);
  interp_destroy_continuation(continuation);
  (void)lainir_vm_control_finish(interp->vm_control, interp->vm_owner);
  return result;
}

static LainirValue interp_call_sub(LainirInterpreter *interp, L1Subroutine *sub,
                                    const LainirValue *args, uint32_t arg_count) {
  LainirValue fast_result;
  if (interp_try_fast_builtin(interp, sub, args, arg_count, &fast_result))
    return fast_result;
  if (interp->vm_control && interp->call_depth == 0)
    return interp_call_root_resumable(interp, sub, args, arg_count);
  LainirFrame frame; memset(&frame, 0, sizeof(frame));
  frame.args = frame.args_inline;
  frame.locals = frame.locals_inline;
  frame.local_cap = sizeof(frame.locals_inline) / sizeof(frame.locals_inline[0]);
  LainirFrame *saved_frame = interp->active_frame;
  LainirTraceProcedure *saved_trace = interp->trace_active;
  LainirTraceProcedure *trace = interp_trace_procedure(interp, sub);
  if (trace) trace->calls++;
  interp->trace_active = trace;
  uint32_t max_depth = interp->caps ? interp->caps->max_call_depth : 0;
  if (max_depth && interp->call_depth >= max_depth) {
    interp_trap(interp, "interpreter call-depth limit exceeded");
    interp->trace_active = saved_trace;
    return lainir_value_unit();
  }
  interp->call_depth++;
  frame.sub = sub; frame.arg_count = arg_count; frame.caller = saved_frame;
  interp->active_frame = &frame;
  int vm_frame_pushed = 0;
  if (arg_count) {
    if (arg_count > sizeof(frame.args_inline) / sizeof(frame.args_inline[0])) {
      frame.args = calloc(arg_count, sizeof(LainirValue));
      frame.args_heap = 1;
    }
    if (!frame.args) { interp_trap(interp, "out of memory"); interp->call_depth--; interp->trace_active = saved_trace; interp->active_frame = saved_frame; return lainir_value_unit(); }
    for (uint32_t i = 0; i < arg_count; i++) {
      frame.args[i] = interp_coerce_physical(
          interp, args[i], i < sub->param_count ? sub->param_tys[i] : NULL);
      if (interp->error) {
        interp_free_frame(&frame);
        interp->call_depth--;
        interp->trace_active = saved_trace;
        interp->active_frame = saved_frame;
        return lainir_value_unit();
      }
    }
    const char *trace_call = interp->trace_call_name;
    if (trace_call && sub->name && strcmp(trace_call, sub->name) == 0) {
      fprintf(stderr, "lainir call %s argc=%u", sub->name, sub->param_count);
      for (uint32_t trace_index = 0; trace_index < sub->param_count; trace_index++) {
        LainirValue trace_arg = frame.args[trace_index];
        if (trace_arg.kind == LAINIR_VALUE_BITS)
          fprintf(stderr, " arg%u=bits:%llu", trace_index,
                  (unsigned long long)trace_arg.as.bits);
        else if (trace_arg.kind == LAINIR_VALUE_ADDR)
          fprintf(stderr, " arg%u=addr:%p", trace_index, (void *)trace_arg.as.addr);
        else
          fprintf(stderr, " arg%u=kind:%d", trace_index, (int)trace_arg.kind);
      }
      fputc('\n', stderr);
      if (getenv("LAINIR_TRACE_CALL_FIELDS")) {
        for (uint32_t trace_index = 0; trace_index < sub->param_count; trace_index++) {
          LainirValue trace_arg = frame.args[trace_index];
          if (trace_arg.kind == LAINIR_VALUE_ADDR && trace_arg.as.addr) {
            unsigned char *raw = (unsigned char *)trace_arg.as.addr;
            fprintf(stderr, " fields%u=", trace_index);
            for (unsigned int off = 0; off < 80; off += 8) {
              uint64_t word = 0;
              memcpy(&word, raw + off, sizeof(word));
              fprintf(stderr, "%s%llu", off ? "," : "", (unsigned long long)word);
            }
            fputc('\n', stderr);
          }
        }
      }
      if (strcmp(sub->name, "f0_Syntax_token_equal") == 0 &&
          sub->param_count == 5 && getenv("LAINIR_TRACE_TOKEN_EQUAL")) {
        uint8_t *store_raw = (uint8_t *)frame.args[0].as.addr;
        uint64_t unit_id = frame.args[1].as.bits;
        uint64_t node_id = frame.args[2].as.bits;
        uint8_t *unit = NULL;
        uint8_t *node = NULL;
        if (store_raw) {
          uint8_t *units_vec = (uint8_t *)interp_fast_load_u64(store_raw, 0);
          uint8_t *units_storage = units_vec ? (uint8_t *)interp_fast_load_u64(units_vec, 0) : NULL;
          unit = units_storage ? (uint8_t *)interp_fast_load_u64(units_storage + 24 + unit_id * 8, 0) : NULL;
          if (unit) {
            uint8_t *nodes_vec = (uint8_t *)interp_fast_load_u64(unit, 8);
            uint8_t *nodes_storage = nodes_vec ? (uint8_t *)interp_fast_load_u64(nodes_vec, 0) : NULL;
            node = nodes_storage ? (uint8_t *)interp_fast_load_u64(nodes_storage + 24 + node_id * 8, 0) : NULL;
          }
        }
        fprintf(stderr, " token_equal_debug unit=%p node=%p", (void *)unit, (void *)node);
        if (unit) {
          uint8_t *source = (uint8_t *)interp_fast_load_u64(unit, 0);
          fprintf(stderr, " source=%p", (void *)source);
        }
        if (node) {
          uint8_t *span = (uint8_t *)interp_fast_load_u64(node, 12);
          fprintf(stderr, " node_type=%u", interp_fast_load_u32(node, 0));
          if (span) fprintf(stderr, " span=%llu..%llu", (unsigned long long)interp_fast_load_u64(span, 0), (unsigned long long)interp_fast_load_u64(span, 8));
        }
        fprintf(stderr, " literal=%p len=%llu\n", frame.args[3].kind == LAINIR_VALUE_ADDR ? frame.args[3].as.addr : NULL, (unsigned long long)frame.args[4].as.bits);
        if (unit && node && frame.args[3].kind == LAINIR_VALUE_ADDR) {
          uint8_t *source = (uint8_t *)interp_fast_load_u64(unit, 0);
          uint8_t *span = (uint8_t *)interp_fast_load_u64(node, 12);
          uint64_t start = span ? interp_fast_load_u64(span, 0) : 0;
          uint64_t len = frame.args[4].as.bits;
          fprintf(stderr, " token_bytes=");
          for (uint64_t i = 0; i < len && i < 16; i++) fprintf(stderr, "%02x", source ? source[start + i] : 0);
          fprintf(stderr, " literal_bytes=");
          for (uint64_t i = 0; i < len && i < 16; i++) fprintf(stderr, "%02x", ((uint8_t *)frame.args[3].as.addr)[i]);
          fputc('\n', stderr);
        }
      }
      if (strcmp(sub->name, "f0_Lower_program") == 0 &&
          sub->param_count == 1 && frame.args[0].kind == LAINIR_VALUE_ADDR &&
          frame.args[0].as.addr) {
        uint8_t *source = (uint8_t *)frame.args[0].as.addr;
        fprintf(stderr, " lower_vectors=");
        for (unsigned int off = 16; off <= 40; off += 8) {
          uint8_t *vec = (uint8_t *)interp_fast_load_u64(source, off);
          uint64_t len = vec ? interp_fast_load_u64(vec, 8) : 0;
          fprintf(stderr, "%s%u:%p/%llu", off == 16 ? "" : ",", off,
                  (void *)vec, (unsigned long long)len);
        }
        fputc('\n', stderr);
      }
      fflush(stderr);
    }
  }

  if (interp->vm_control) {
    if (!lainir_vm_control_push_frame(
            interp->vm_control, interp->vm_owner,
            (uint64_t)(uintptr_t)sub, (uint64_t)(uintptr_t)sub->blocks,
            (uint64_t)(uintptr_t)&frame)) {
      interp_trap(interp, "VM call frame push rejected");
      interp_free_frame(&frame);
      interp->call_depth--;
      interp->trace_active = saved_trace;
      interp->active_frame = saved_frame;
      return lainir_value_unit();
    }
    vm_frame_pushed = 1;
  }

  int saved_ret = interp->should_return;
  LainirValue saved_val = interp->return_value;
  interp->should_return = 0;

  L1Block *block = sub->blocks;
  while (block) {
    interp_exec_block(interp, &frame, block);
    if (interp->vm_slice_yielded || interp->vm_blocked) {
      if (interp->vm_blocked && interp->continuation) {
        LainirFrame *saved = interp_detach_frame(
            &frame, &interp->continuation->frame);
        LainirNestedContinuation *nested =
            saved ? calloc(1, sizeof(*nested)) : NULL;
        if (!saved || !nested) {
          free(saved);
          interp_trap(interp, "out of memory for nested continuation");
        } else {
          interp_expr_cache_rekey(interp, &frame, saved);
          nested->frame = saved;
          nested->sub = sub;
          nested->block = interp->active_block;
          nested->inst = interp->active_inst;
          if (!interp->continuation->nested_top) {
            interp->continuation->nested_top = nested;
          } else if (interp->continuation->nested_top->frame == &frame) {
            nested->parent = interp->continuation->nested_top;
            interp->continuation->nested_top = nested;
          } else {
            LainirNestedContinuation *tail =
                interp->continuation->nested_top;
            while (tail->parent) tail = tail->parent;
            nested->parent = NULL;
            tail->parent = nested;
          }
        }
      }
      interp_free_frame(&frame);
      if (vm_frame_pushed)
        (void)lainir_vm_control_pop_frame(interp->vm_control, interp->vm_owner);
      interp->should_return = saved_ret;
      interp->call_depth--;
      interp->trace_active = saved_trace;
      interp->active_frame = saved_frame;
      return lainir_value_unit();
    }
    if (interp->error) {
      interp_free_frame(&frame);
      if (vm_frame_pushed)
        (void)lainir_vm_control_pop_frame(interp->vm_control, interp->vm_owner);
      interp->should_return = saved_ret;
      interp->call_depth--;
      interp->trace_active = saved_trace;
      interp->active_frame = saved_frame;
      return lainir_value_unit();
    }
    if (interp->should_return) break;
    block = block->next;
  }

  LainirValue result = interp->return_value;
  if (!interp->error && result.kind == LAINIR_VALUE_ADDR &&
      interp_frame_owns_addr(&frame, result.as.addr)) {
    interp_trap(interp, "#alloca address escaped procedure activation");
    result = lainir_value_unit();
  }
  const char *trace_return = interp->trace_return_name;
  if (trace_return && sub->name && strcmp(trace_return, sub->name) == 0) {
    fprintf(stderr, "lainir return %s kind=%d", sub->name, (int)result.kind);
    if (result.kind == LAINIR_VALUE_BITS)
      fprintf(stderr, " bits:%llu", (unsigned long long)result.as.bits);
    else if (result.kind == LAINIR_VALUE_ADDR)
      fprintf(stderr, " addr:%p", (void *)result.as.addr);
    fputc('\n', stderr);
    fflush(stderr);
  }
  interp->should_return = saved_ret;
  interp->return_value = saved_val;
  if (vm_frame_pushed)
    (void)lainir_vm_control_pop_frame(interp->vm_control, interp->vm_owner);
  interp_free_frame(&frame);
  interp->call_depth--;
  interp->trace_active = saved_trace;
  interp->active_frame = saved_frame;
  return result;
}

/* ═══════════════════════════════════════════════════════════════
 * Public entry point
 * ═══════════════════════════════════════════════════════════════ */

static int interp_env_enabled(const char *name) {
  const char *value = getenv(name);
  return value && value[0] == '1';
}

static void interp_init_env_flags(LainirInterpreter *interp) {
  interp->trace_live_enabled = interp_env_enabled("LAINIR_TRACE_LIVE");
  interp->fast_builtins_enabled = interp_env_enabled("LAINIR_FAST_BUILTINS");
  interp->fast_leaves_enabled = interp_env_enabled("LAINIR_FAST_LEAVES");
  interp->fast_scanners_enabled = interp_env_enabled("LAINIR_FAST_SCANNERS");
  interp->fast_meta_scan_enabled = interp_env_enabled("LAINIR_FAST_META_SCAN");
  interp->trace_meta_enabled = interp_env_enabled("LAINIR_TRACE_META");
  interp->trace_fast_names_enabled =
      interp_env_enabled("LAINIR_TRACE_FAST_NAMES");
  interp->trace_call_name = getenv("LAINIR_TRACE_CALL");
  interp->trace_return_name = getenv("LAINIR_TRACE_RETURN");
}

LainirRunStatus lainir_run(const LainirRunRequest *request,
                          LainirValue *result_out, const char **error_out) {
  LainirInterpreter interp; memset(&interp, 0, sizeof(interp));
  if (!request || !request->module || !request->entry_name) {
    if (error_out) *error_out = "invalid run request";
    return LAINIR_RUN_TRAP;
  }
  if (request->vm_session &&
      !lainir_vm_session_accepts(request->vm_session,
                                  request->vm_session_owner,
                                  request->vm_control, request->vm_owner)) {
    if (error_out) *error_out = "VM session rejected control";
    return LAINIR_RUN_TRAP;
  }
  interp.module = request->module;
  interp.caps = request->caps;
  interp.vm_control = request->vm_control;
  interp.vm_owner = request->vm_owner;
  /* Diagnostic and fast-path environment switches are read once per run;
   * getenv must not sit on the interpreter's step/call hot paths. */
  interp_init_env_flags(&interp);
  interp_build_sub_index(&interp);
  interp_trace_init(&interp);
  L1Subroutine *entry = interp_find_sub(&interp, request->entry_name);
  if (!entry) {
    interp_trace_finish(&interp);
    free(interp.sub_index);
    if (error_out) *error_out = "entry not found";
    return LAINIR_RUN_NO_ENTRY;
  }
  *result_out = interp_call_sub(&interp, entry, request->args, request->arg_count);
  interp_trace_finish(&interp);
  free(interp.sub_index);
  if (interp.error) {
    if (interp.vm_control)
      (void)lainir_vm_control_abort(interp.vm_control, interp.vm_owner);
    if (error_out) *error_out = interp.error;
    return LAINIR_RUN_TRAP;
  }
  if (interp.vm_blocked) {
    if (error_out) *error_out = NULL;
    return LAINIR_RUN_BLOCKED;
  }
  if (interp.vm_slice_yielded) {
    if (error_out) *error_out = NULL;
    return LAINIR_RUN_SLICE;
  }
  if (error_out) *error_out = NULL;
  return LAINIR_RUN_OK;
}

LainirRunStatus lainir_run_owned_result(
    const LainirRunRequest *request, LainirValue *scalar_out,
    LainirVmResultHandle **handle_out,
    LainirVmResultPayloadFree payload_free, void *payload_user_data,
    const char **error_out) {
  LainirValue value = lainir_value_unit();
  LainirRunStatus status;
  if (handle_out) *handle_out = NULL;
  status = lainir_run(request, &value, error_out);
  if (status != LAINIR_RUN_OK) return status;
  if (value.kind == LAINIR_VALUE_ADDR || value.kind == LAINIR_VALUE_STRING ||
      value.kind == LAINIR_VALUE_FUNC) {
    if (!request || !request->vm_session || !handle_out) {
      if (error_out) *error_out = "object result requires VM session";
      return LAINIR_RUN_TRAP;
    }
    *handle_out = lainir_vm_result_handle_new(
        request->vm_session, request->vm_session_owner, &value,
        payload_free, payload_user_data);
    if (!*handle_out) {
      if (error_out) *error_out = "object result handle allocation failed";
      return LAINIR_RUN_TRAP;
    }
    if (scalar_out) *scalar_out = lainir_value_unit();
    return LAINIR_RUN_OK;
  }
  if (scalar_out) *scalar_out = value;
  return LAINIR_RUN_OK;
}

LainirRunStatus lainir_eval_block(
    L1Subroutine *module,
    L1Block *block,
    L1Type *return_type,
    LainirCapabilityTable *caps,
    LainirValue *result_out,
    const char **error_out) {
  LainirInterpreter interp;
  L1Subroutine eval_sub;

  if (!module || !block || !result_out) {
    if (error_out) *error_out = "invalid eval block request";
    return LAINIR_RUN_TRAP;
  }

  memset(&interp, 0, sizeof(interp));
  memset(&eval_sub, 0, sizeof(eval_sub));
  eval_sub.name = "<eval>";
  eval_sub.ret_ty = return_type;
  eval_sub.blocks = block;
  interp.module = module;
  interp.caps = caps;
  interp_init_env_flags(&interp);
  interp_build_sub_index(&interp);

  *result_out = interp_call_sub(&interp, &eval_sub, NULL, 0);
  free(interp.sub_index);
  if (interp.error) {
    if (error_out) *error_out = interp.error;
    return LAINIR_RUN_TRAP;
  }
  if (error_out) *error_out = NULL;
  return LAINIR_RUN_OK;
}

/* Compiler-side #eval materialization.  This deliberately lives beside the
 * interpreter: the compiler supplies an IR module, asks the interpreter to
 * execute each eval block, then replaces scalar results with constants.
 * Unit results stay as evaluated EXPR_EVAL nodes so a backend can lower them
 * to an effect-free expression without inventing a second unit constant. */
static LainirRunStatus fold_expr(L1Subroutine *module, L1Expr **slot,
                                 LainirCapabilityTable *caps,
                                 const char **error_out,
                                 LainirEvalSink sink,
                                 void *sink_user_data);
static unsigned fold_eval_depth;

static LainirRunStatus fold_block(L1Subroutine *module, L1Block *block,
                                  LainirCapabilityTable *caps,
                                  const char **error_out,
                                  LainirEvalSink sink,
                                  void *sink_user_data) {
  for (; block; block = block->next) {
    for (L1Instruction *inst = block->body; inst; inst = inst->next) {
      L1Expr **expr = NULL;
      switch (inst->kind) {
      case INST_LET: expr = &inst->data.let.val; break;
      case INST_SET: expr = &inst->data.set.val; break;
      case INST_STORE:
        if (fold_expr(module, &inst->data.store.dest, caps, error_out, sink, sink_user_data) != LAINIR_RUN_OK) return LAINIR_RUN_TRAP;
        expr = &inst->data.store.val; break;
      case INST_RETURN: expr = &inst->data.ret.val; break;
      case INST_CALL: expr = &inst->data.call_inst.expr; break;
      case INST_IF:
        expr = &inst->data.if_stmt.condition;
        if (fold_expr(module, expr, caps, error_out, sink, sink_user_data) != LAINIR_RUN_OK) return LAINIR_RUN_TRAP;
        if (fold_block(module, inst->data.if_stmt.then_body, caps, error_out, sink, sink_user_data) != LAINIR_RUN_OK) return LAINIR_RUN_TRAP;
        if (fold_block(module, inst->data.if_stmt.else_body, caps, error_out, sink, sink_user_data) != LAINIR_RUN_OK) return LAINIR_RUN_TRAP;
        continue;
      case INST_LOOP:
        if (fold_block(module, inst->data.loop.body, caps, error_out, sink, sink_user_data) != LAINIR_RUN_OK) return LAINIR_RUN_TRAP;
        continue;
      default: continue;
      }
      if (expr && fold_expr(module, expr, caps, error_out, sink, sink_user_data) != LAINIR_RUN_OK) return LAINIR_RUN_TRAP;
    }
  }
  return LAINIR_RUN_OK;
}

static LainirRunStatus fold_expr(L1Subroutine *module, L1Expr **slot,
                                 LainirCapabilityTable *caps,
                                 const char **error_out,
                                 LainirEvalSink sink,
                                 void *sink_user_data) {
  L1Expr *expr = slot ? *slot : NULL;
  if (!expr) return LAINIR_RUN_OK;
  switch (expr->kind) {
  case EXPR_EVAL: {
    LainirValue value;
    int outer_eval = fold_eval_depth == 0;
    LainirRunStatus nested_status;
    fold_eval_depth++;
    nested_status = fold_block(module, expr->data.eval.block, caps, error_out,
                               sink, sink_user_data);
    fold_eval_depth--;
    if (nested_status != LAINIR_RUN_OK) return nested_status;
    LainirRunStatus status = lainir_eval_block(module, expr->data.eval.block,
                                               expr->data.eval.ret_ty, caps,
                                               &value, error_out);
    if (status != LAINIR_RUN_OK) return status;
    if (value.kind == LAINIR_VALUE_UNIT) {
      if (sink && outer_eval) sink(&value, sink_user_data);
      return LAINIR_RUN_OK;
    }
    if (value.kind != LAINIR_VALUE_BITS) {
      if (error_out) *error_out = "#eval result must be a bits value";
      return LAINIR_RUN_BAD_CALL;
    }
    if (sink && outer_eval) sink(&value, sink_user_data);
    L1Expr *constant = lainir_new_expr(EXPR_CONST);
    if (!constant) {
      if (error_out) *error_out = "out of memory";
      return LAINIR_RUN_TRAP;
    }
    constant->data.const_val = (int64_t)value.as.bits;
    *slot = constant;
    lainir_free_expr_tree(expr);
    return LAINIR_RUN_OK;
  }
  case EXPR_LOAD: return fold_expr(module, &expr->data.load.addr, caps, error_out, sink, sink_user_data);
  case EXPR_LEA:
    if (fold_expr(module, &expr->data.lea.base, caps, error_out, sink, sink_user_data) != LAINIR_RUN_OK) return LAINIR_RUN_TRAP;
    return fold_expr(module, &expr->data.lea.idx, caps, error_out, sink, sink_user_data);
  case EXPR_CALL: for (uint32_t i = 0; i < expr->data.call.arg_count; i++) if (fold_expr(module, &expr->data.call.args[i], caps, error_out, sink, sink_user_data) != LAINIR_RUN_OK) return LAINIR_RUN_TRAP; return LAINIR_RUN_OK;
  case EXPR_CALL_INDIRECT:
    if (fold_expr(module, &expr->data.call_indirect.fn_ptr, caps, error_out, sink, sink_user_data) != LAINIR_RUN_OK) return LAINIR_RUN_TRAP;
    for (uint32_t i = 0; i < expr->data.call_indirect.arg_count; i++) if (fold_expr(module, &expr->data.call_indirect.args[i], caps, error_out, sink, sink_user_data) != LAINIR_RUN_OK) return LAINIR_RUN_TRAP;
    return LAINIR_RUN_OK;
  case EXPR_ADD: case EXPR_SUB: case EXPR_MUL: case EXPR_EQ: case EXPR_NE:
  case EXPR_SDIV: case EXPR_UDIV: case EXPR_SLT: case EXPR_SLE: case EXPR_SGT: case EXPR_SGE: case EXPR_ULT: case EXPR_ULE: case EXPR_UGT: case EXPR_UGE:
  case EXPR_FADD: case EXPR_FSUB: case EXPR_FMUL: case EXPR_FDIV: case EXPR_FEQ: case EXPR_FLT:
    if (fold_expr(module, &expr->data.bin.left, caps, error_out, sink, sink_user_data) != LAINIR_RUN_OK) return LAINIR_RUN_TRAP;
    return fold_expr(module, &expr->data.bin.right, caps, error_out, sink, sink_user_data);
  case EXPR_POPCOUNT: case EXPR_CLZ: case EXPR_ROTL: case EXPR_INT2PTR: case EXPR_PTR2INT:
    return fold_expr(module, &expr->data.unary.operand, caps, error_out, sink, sink_user_data);
  case EXPR_ZEXT: case EXPR_SEXT: case EXPR_TRUNC: case EXPR_BITCAST:
    return fold_expr(module, &expr->data.conversion.operand, caps, error_out, sink, sink_user_data);
  default: return LAINIR_RUN_OK;
  }
}

static uint64_t count_expr_evals(const L1Expr *expr);

static void set_eval_type_expr(L1Expr *expr, L1Type *type);
static void set_eval_type_block(L1Block *block, L1Type *type) {
  for (; block; block = block->next) {
    for (L1Instruction *inst = block->body; inst; inst = inst->next) {
      switch (inst->kind) {
      case INST_LET: set_eval_type_expr(inst->data.let.val, type); break;
      case INST_SET: set_eval_type_expr(inst->data.set.val, type); break;
      case INST_STORE:
        set_eval_type_expr(inst->data.store.dest, type);
        set_eval_type_expr(inst->data.store.val, type);
        break;
      case INST_RETURN: set_eval_type_expr(inst->data.ret.val, type); break;
      case INST_CALL: set_eval_type_expr(inst->data.call_inst.expr, type); break;
      case INST_IF:
        set_eval_type_expr(inst->data.if_stmt.condition, type);
        set_eval_type_block(inst->data.if_stmt.then_body, type);
        set_eval_type_block(inst->data.if_stmt.else_body, type);
        break;
      case INST_LOOP: set_eval_type_block(inst->data.loop.body, type); break;
      default: break;
      }
    }
  }
}

static void set_eval_type_expr(L1Expr *expr, L1Type *type) {
  if (!expr) return;
  if (expr->kind == EXPR_EVAL && !expr->data.eval.ret_ty)
    expr->data.eval.ret_ty = type;
  switch (expr->kind) {
  case EXPR_EVAL: set_eval_type_block(expr->data.eval.block, type); break;
  case EXPR_LOAD: set_eval_type_expr(expr->data.load.addr, type); break;
  case EXPR_LEA:
    set_eval_type_expr(expr->data.lea.base, type);
    set_eval_type_expr(expr->data.lea.idx, type);
    break;
  case EXPR_CALL:
    for (uint32_t i = 0; i < expr->data.call.arg_count; i++)
      set_eval_type_expr(expr->data.call.args[i], type);
    break;
  case EXPR_CALL_INDIRECT:
    set_eval_type_expr(expr->data.call_indirect.fn_ptr, type);
    for (uint32_t i = 0; i < expr->data.call_indirect.arg_count; i++)
      set_eval_type_expr(expr->data.call_indirect.args[i], type);
    break;
  case EXPR_ZEXT: case EXPR_SEXT: case EXPR_TRUNC: case EXPR_BITCAST:
    set_eval_type_expr(expr->data.conversion.operand, type); break;
  default: break;
  }
}

static uint64_t count_block_evals(const L1Block *block) {
  uint64_t count = 0;
  for (; block; block = block->next) {
    for (const L1Instruction *inst = block->body; inst; inst = inst->next) {
      switch (inst->kind) {
      case INST_LET: count += count_expr_evals(inst->data.let.val); break;
      case INST_SET: count += count_expr_evals(inst->data.set.val); break;
      case INST_STORE:
        count += count_expr_evals(inst->data.store.dest);
        count += count_expr_evals(inst->data.store.val);
        break;
      case INST_IF:
        count += count_expr_evals(inst->data.if_stmt.condition);
        count += count_block_evals(inst->data.if_stmt.then_body);
        count += count_block_evals(inst->data.if_stmt.else_body);
        break;
      case INST_LOOP: count += count_block_evals(inst->data.loop.body); break;
      case INST_RETURN: count += count_expr_evals(inst->data.ret.val); break;
      case INST_CALL: count += count_expr_evals(inst->data.call_inst.expr); break;
      default: break;
      }
    }
  }
  return count;
}

static uint64_t count_expr_evals(const L1Expr *expr) {
  uint64_t count = 0;
  if (!expr) return 0;
  switch (expr->kind) {
  case EXPR_EVAL:
    return 1 + count_block_evals(expr->data.eval.block);
  case EXPR_LOAD: return count_expr_evals(expr->data.load.addr);
  case EXPR_LEA:
    return count_expr_evals(expr->data.lea.base) +
           count_expr_evals(expr->data.lea.idx);
  case EXPR_ZEXT: case EXPR_SEXT: case EXPR_TRUNC: case EXPR_BITCAST:
    return count_expr_evals(expr->data.conversion.operand);
  case EXPR_POPCOUNT: case EXPR_CLZ: case EXPR_ROTL:
  case EXPR_INT2PTR: case EXPR_PTR2INT:
    return count_expr_evals(expr->data.unary.operand);
  case EXPR_CALL:
    for (uint32_t i = 0; i < expr->data.call.arg_count; i++)
      count += count_expr_evals(expr->data.call.args[i]);
    return count;
  case EXPR_CALL_INDIRECT:
    count += count_expr_evals(expr->data.call_indirect.fn_ptr);
    for (uint32_t i = 0; i < expr->data.call_indirect.arg_count; i++)
      count += count_expr_evals(expr->data.call_indirect.args[i]);
    return count;
  case EXPR_ADD: case EXPR_SUB: case EXPR_MUL:
  case EXPR_EQ: case EXPR_NE: case EXPR_SDIV: case EXPR_UDIV:
  case EXPR_SLT: case EXPR_SLE: case EXPR_SGT: case EXPR_SGE:
  case EXPR_ULT: case EXPR_ULE: case EXPR_UGT: case EXPR_UGE:
  case EXPR_FADD: case EXPR_FSUB: case EXPR_FMUL: case EXPR_FDIV:
  case EXPR_FEQ: case EXPR_FLT:
    return count_expr_evals(expr->data.bin.left) +
           count_expr_evals(expr->data.bin.right);
  default: return 0;
  }
}

LainirRunStatus lainir_fold_module_with_sink(L1Subroutine *module,
                                             LainirCapabilityTable *caps,
                                             const char **error_out,
                                             LainirEvalSink sink,
                                             void *sink_user_data) {
  LainirCapabilityTable default_caps;
  uint64_t eval_count;
  if (!module) {
    if (error_out) *error_out = "invalid module";
    return LAINIR_RUN_TRAP;
  }
  if (!caps) {
    memset(&default_caps, 0, sizeof(default_caps));
    lainir_caps_set_limits(&default_caps, UINT64_C(1000000), 256,
                           UINT64_C(64) * 1024 * 1024);
    lainir_caps_set_eval_limit(&default_caps, UINT64_C(100000));
    caps = &default_caps;
  }
  eval_count = 0;
  for (L1Subroutine *sub = module; sub; sub = sub->next)
    eval_count += count_block_evals(sub->blocks);
  if (caps->max_eval_blocks && eval_count > caps->max_eval_blocks) {
    if (error_out) *error_out = "compile-time eval block limit exceeded";
    return LAINIR_RUN_TRAP;
  }
  for (L1Subroutine *sub = module; sub; sub = sub->next)
    set_eval_type_block(sub->blocks, sub->ret_ty);
  fold_eval_depth = 0;
  for (L1Subroutine *sub = module; sub; sub = sub->next)
    if (fold_block(module, sub->blocks, caps, error_out, sink, sink_user_data) != LAINIR_RUN_OK)
      return LAINIR_RUN_TRAP;
  if (error_out) *error_out = NULL;
  return LAINIR_RUN_OK;
}

LainirRunStatus lainir_fold_module(L1Subroutine *module,
                                   LainirCapabilityTable *caps,
                                   const char **error_out) {
  return lainir_fold_module_with_sink(module, caps, error_out, NULL, NULL);
}

const L1Subroutine *lainir_module_first_procedure(const L1Subroutine *module) {
  return module;
}

const L1Subroutine *lainir_procedure_next(const L1Subroutine *procedure) {
  return procedure ? procedure->next : NULL;
}

const char *lainir_procedure_name(const L1Subroutine *procedure) {
  return procedure ? procedure->name : NULL;
}

uint32_t lainir_procedure_name_length(const L1Subroutine *procedure) {
  return procedure && procedure->name ? (uint32_t)strlen(procedure->name) : 0;
}

const char *lainir_procedure_link_name(const L1Subroutine *procedure) {
  return procedure ? procedure->link_name : NULL;
}

const L1Type *lainir_procedure_return_type(const L1Subroutine *procedure) {
  return procedure ? procedure->ret_ty : NULL;
}

int lainir_procedure_is_external(const L1Subroutine *procedure) {
  return procedure ? procedure->is_extern : 0;
}

int lainir_procedure_is_data(const L1Subroutine *procedure) {
  return procedure ? procedure->is_data : 0;
}

uint32_t lainir_data_size(const L1Subroutine *data) {
  return data && data->is_data ? data->data_size : 0;
}

uint32_t lainir_data_alignment(const L1Subroutine *data) {
  return data && data->is_data ? data->data_alignment : 0;
}

const uint8_t *lainir_data_bytes(const L1Subroutine *data) {
  return data && data->is_data ? data->data_bytes : NULL;
}

uint32_t lainir_procedure_parameter_count(const L1Subroutine *procedure) {
  return procedure ? procedure->param_count : 0;
}

const L1Type *lainir_procedure_parameter_type(const L1Subroutine *procedure, uint32_t index) {
  if (!procedure || index >= procedure->param_count) return NULL;
  return procedure->param_tys[index];
}

const char *lainir_procedure_parameter_name(const L1Subroutine *procedure,
                                            uint32_t index) {
  if (!procedure || index >= procedure->param_count || !procedure->param_names)
    return NULL;
  return procedure->param_names[index];
}

const L1Block *lainir_procedure_first_block(const L1Subroutine *procedure) {
  return procedure ? procedure->blocks : NULL;
}

const L1Block *lainir_block_next(const L1Block *block) {
  return block ? block->next : NULL;
}

L1ExprKind lainir_expr_kind(const L1Expr *expr) {
  return expr ? expr->kind : (L1ExprKind)-1;
}


const L1Type *lainir_expr_type(const L1Expr *expr) {
  if (!expr) return NULL;
  switch (expr->kind) {
    case EXPR_VAR: return expr->data.var.ty;
    case EXPR_ARG: return expr->data.arg.ty;
    case EXPR_LOAD: return expr->data.load.ty;
    case EXPR_ZEXT: case EXPR_SEXT: case EXPR_TRUNC: case EXPR_BITCAST:
      return expr->data.conversion.target_ty;
    case EXPR_CALL: return expr->data.call.ret_ty;
    case EXPR_CALL_INDIRECT: return expr->data.call_indirect.ret_ty;
    case EXPR_STRING: return expr->data.str_val.ty;
    case EXPR_DATA_ADDR: return expr->data.data_addr.ty;
    case EXPR_ALLOCA: return expr->data.alloca.result_ty;
    case EXPR_EVAL: return expr->data.eval.ret_ty;
    default: return NULL;
  }
}

const L1Expr *lainir_expr_left(const L1Expr *expr) {
  if (!expr) return NULL;
  if (expr->kind == EXPR_LEA) return expr->data.lea.base;
  switch (expr->kind) {
    case EXPR_ADD: case EXPR_SUB: case EXPR_MUL:
    case EXPR_EQ: case EXPR_NE: case EXPR_FADD: case EXPR_FSUB:
    case EXPR_FMUL: case EXPR_FDIV: case EXPR_FEQ: case EXPR_FLT:
    case EXPR_SDIV: case EXPR_UDIV: case EXPR_SLT: case EXPR_SLE:
    case EXPR_SGT: case EXPR_SGE: case EXPR_ULT: case EXPR_ULE:
    case EXPR_UGT: case EXPR_UGE:
      return expr->data.bin.left;
    default: return NULL;
  }
}

const L1Expr *lainir_expr_right(const L1Expr *expr) {
  if (!expr) return NULL;
  if (expr->kind == EXPR_LEA) return expr->data.lea.idx;
  switch (expr->kind) {
    case EXPR_ADD: case EXPR_SUB: case EXPR_MUL:
    case EXPR_EQ: case EXPR_NE: case EXPR_FADD: case EXPR_FSUB:
    case EXPR_FMUL: case EXPR_FDIV: case EXPR_FEQ: case EXPR_FLT:
    case EXPR_SDIV: case EXPR_UDIV: case EXPR_SLT: case EXPR_SLE:
    case EXPR_SGT: case EXPR_SGE: case EXPR_ULT: case EXPR_ULE:
    case EXPR_UGT: case EXPR_UGE:
      return expr->data.bin.right;
    default: return NULL;
  }
}

const L1Expr *lainir_expr_next(const L1Expr *expr) {
  (void)expr;
  return NULL;
}

uint32_t lainir_expr_argument_count(const L1Expr *expr) {
  if (!expr) return 0;
  if (expr->kind == EXPR_CALL) return expr->data.call.arg_count;
  if (expr->kind == EXPR_CALL_INDIRECT) return expr->data.call_indirect.arg_count;
  return 0;
}

const L1Expr *lainir_expr_argument_at(const L1Expr *expr, uint32_t index) {
  if (!expr) return NULL;
  if (expr->kind == EXPR_CALL && index < expr->data.call.arg_count)
    return expr->data.call.args[index];
  if (expr->kind == EXPR_CALL_INDIRECT && index < expr->data.call_indirect.arg_count)
    return expr->data.call_indirect.args[index];
  return NULL;
}

int64_t lainir_expr_const_value(const L1Expr *expr) {
  return expr && expr->kind == EXPR_CONST ? expr->data.const_val : 0;
}

uint32_t lainir_expr_arg_index(const L1Expr *expr) {
  return expr && expr->kind == EXPR_ARG ? expr->data.arg.index : 0;
}

const char *lainir_expr_name(const L1Expr *expr) {
  if (!expr) return NULL;
  if (expr->kind == EXPR_VAR) return expr->data.var.name;
  if (expr->kind == EXPR_DATA_ADDR) return expr->data.data_addr.name;
  return NULL;
}

const char *lainir_expr_callee_name(const L1Expr *expr) {
  if (!expr) return NULL;
  if (expr->kind == EXPR_CALL) return expr->data.call.fn_name;
  if (expr->kind == EXPR_PROC_ADDR) return expr->data.proc_addr.fn_name;
  return NULL;
}

const char *lainir_expr_string(const L1Expr *expr) {
  return expr && expr->kind == EXPR_STRING ? expr->data.str_val.content : NULL;
}

const L1Expr *lainir_expr_operand(const L1Expr *expr) {
  if (!expr) return NULL;
  switch (expr->kind) {
    case EXPR_LOAD: return expr->data.load.addr;
    case EXPR_EVAL: return NULL;
    case EXPR_POPCOUNT: case EXPR_CLZ: case EXPR_ROTL:
    case EXPR_INT2PTR: case EXPR_PTR2INT:
      return expr->data.unary.operand;
    case EXPR_ZEXT: case EXPR_SEXT: case EXPR_TRUNC: case EXPR_BITCAST:
      return expr->data.conversion.operand;
    default: return NULL;
  }
}

const L1Block *lainir_expr_block(const L1Expr *expr) {
  return expr && expr->kind == EXPR_EVAL ? expr->data.eval.block : NULL;
}

uint32_t lainir_expr_scale(const L1Expr *expr) {
  return expr && expr->kind == EXPR_LEA ? expr->data.lea.scale : 0;
}

uint32_t lainir_expr_offset(const L1Expr *expr) {
  return expr && expr->kind == EXPR_LEA ? expr->data.lea.offset : 0;
}

uint32_t lainir_expr_byte_size(const L1Expr *expr) {
  return expr && expr->kind == EXPR_ALLOCA ? expr->data.alloca.byte_size : 0;
}

const char *lainir_diagnostic_message(const L1Diagnostic *diagnostic) {
  return diagnostic ? diagnostic->message : "";
}

int lainir_diagnostic_code(const L1Diagnostic *diagnostic) {
  return diagnostic ? diagnostic->code : 0;
}

int lainir_diagnostic_line(const L1Diagnostic *diagnostic) {
  return diagnostic ? diagnostic->line : 0;
}

int lainir_diagnostic_column(const L1Diagnostic *diagnostic) {
  return diagnostic ? diagnostic->column : 0;
}

void lainir_diagnostic_clear(L1Diagnostic *diagnostic) {
  if (diagnostic) memset(diagnostic, 0, sizeof(*diagnostic));
}

int lainir_type_kind(const L1Type *type) { return type ? (int)type->kind : -1; }
uint32_t lainir_type_width(const L1Type *type) { return type ? type->width : 0; }

const L1Instruction *lainir_block_first_instruction(const L1Block *block) {
  return block ? block->body : NULL;
}

const L1Instruction *lainir_instruction_next(const L1Instruction *instruction) {
  return instruction ? instruction->next : NULL;
}

L1InstKind lainir_instruction_kind(const L1Instruction *instruction) {
  return instruction ? instruction->kind : (L1InstKind)-1;
}

const char *lainir_instruction_name(const L1Instruction *instruction) {
  if (!instruction) return NULL;
  if (instruction->kind == INST_LET) return instruction->data.let.name;
  if (instruction->kind == INST_SET) return instruction->data.set.name;
  return NULL;
}

const char *lainir_instruction_label(const L1Instruction *instruction) {
  if (!instruction) return NULL;
  if (instruction->kind == INST_LOOP) return instruction->data.loop.label;
  if (instruction->kind == INST_BREAK || instruction->kind == INST_CONTINUE)
    return instruction->data.jump.label;
  return NULL;
}

const L1Type *lainir_instruction_type(const L1Instruction *instruction) {
  if (!instruction) return NULL;
  if (instruction->kind == INST_LET) return instruction->data.let.ty;
  if (instruction->kind == INST_SET) return instruction->data.set.ty;
  if (instruction->kind == INST_STORE) return instruction->data.store.store_ty;
  return NULL;
}

const L1Expr *lainir_instruction_value(const L1Instruction *instruction) {
  if (!instruction) return NULL;
  if (instruction->kind == INST_LET) return instruction->data.let.val;
  if (instruction->kind == INST_SET) return instruction->data.set.val;
  if (instruction->kind == INST_STORE) return instruction->data.store.val;
  if (instruction->kind == INST_RETURN) return instruction->data.ret.val;
  if (instruction->kind == INST_CALL) return instruction->data.call_inst.expr;
  return NULL;
}

const L1Expr *lainir_instruction_destination(const L1Instruction *instruction) {
  if (!instruction) return NULL;
  if (instruction->kind == INST_STORE) return instruction->data.store.dest;
  return NULL;
}

const L1Expr *lainir_instruction_condition(const L1Instruction *instruction) {
  return instruction && instruction->kind == INST_IF
      ? instruction->data.if_stmt.condition : NULL;
}

const L1Block *lainir_instruction_then_block(const L1Instruction *instruction) {
  return instruction && instruction->kind == INST_IF
      ? instruction->data.if_stmt.then_body : NULL;
}

const L1Block *lainir_instruction_else_block(const L1Instruction *instruction) {
  return instruction && instruction->kind == INST_IF
      ? instruction->data.if_stmt.else_body : NULL;
}

const L1Block *lainir_instruction_loop_block(const L1Instruction *instruction) {
  return instruction && instruction->kind == INST_LOOP
      ? instruction->data.loop.body : NULL;
}
