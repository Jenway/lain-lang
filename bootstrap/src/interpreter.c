#include "interpreter.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
  char *name;
  LainirValue value;
} LainirBinding;

typedef struct {
  uint8_t *data;
  uint32_t size;
} LainirBuffer;

typedef struct LainirFrame {
  L1Subroutine *sub;
  LainirValue *args;
  uint32_t arg_count;
  LainirBinding *locals;
  uint32_t local_count;
  uint32_t local_cap;
  struct LainirFrame *caller;
} LainirFrame;

typedef struct {
  const char *name;
  L1Subroutine *sub;
} LainirSubIndexEntry;

typedef struct {
  L1Subroutine *module;
  LainirSubIndexEntry *sub_index;
  uint32_t sub_index_cap;
  LainirCapabilityTable *caps;
  /* Aggregate allocations have run lifetime, not frame lifetime.  A struct
   * may be returned from a callee and consumed by its caller. */
  LainirBuffer *allocas;
  uint32_t alloca_count;
  uint32_t alloca_cap;
  const char *error;
  int should_return;
  int should_break;
  int should_continue;
  LainirValue return_value;
} LainirInterpreter;

/* ── forward declarations for mutual recursion ── */
static LainirValue interp_eval_expr(LainirInterpreter *, LainirFrame *, L1Expr *);
static void interp_exec_block(LainirInterpreter *, LainirFrame *, L1Block *);
static LainirValue interp_call_sub(LainirInterpreter *, L1Subroutine *,
                                    const LainirValue *, uint32_t);

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

static LainirBinding *interp_lookup_local(LainirFrame *frame, const char *name) {
  for (uint32_t i = 0; i < frame->local_count; i++)
    if (strcmp(frame->locals[i].name, name) == 0) return &frame->locals[i];
  return NULL;
}

static int interp_set_local(LainirFrame *frame, const char *name, LainirValue value) {
  LainirBinding *b = interp_lookup_local(frame, name);
  if (b) { b->value = value; return 1; }
  if (frame->local_count == frame->local_cap) {
    uint32_t nc = frame->local_cap ? frame->local_cap * 2 : 8;
    LainirBinding *nl = realloc(frame->locals, sizeof(LainirBinding) * nc);
    if (!nl) return 0;
    frame->locals = nl; frame->local_cap = nc;
  }
  frame->locals[frame->local_count].name = strdup(name);
  if (!frame->locals[frame->local_count].name) return 0;
  frame->locals[frame->local_count].value = value;
  frame->local_count++;
  return 1;
}

static void interp_free_frame(LainirFrame *frame) {
  if (!frame) return;
  for (uint32_t i = 0; i < frame->local_count; i++) free(frame->locals[i].name);
  free(frame->locals); free(frame->args);
}

static void interp_free_allocas(LainirInterpreter *interp) {
  for (uint32_t i = 0; i < interp->alloca_count; i++)
    free(interp->allocas[i].data);
  free(interp->allocas);
  interp->allocas = NULL;
  interp->alloca_count = 0;
  interp->alloca_cap = 0;
}

static LainirCapabilityEntry *interp_lookup_cap(LainirCapabilityTable *caps, const char *name) {
  if (!caps || !name) return NULL;
  for (uint32_t i = 0; i < caps->count; i++)
    if (strcmp(caps->entries[i].name, name) == 0) return &caps->entries[i];
  return NULL;
}

static void interp_trap(LainirInterpreter *interp, const char *error) {
  if (!interp->error) interp->error = error;
}

static uint64_t interp_value_bits(LainirInterpreter *interp, LainirValue value, const char *ctx) {
  if (value.kind != LAINIR_VALUE_BITS) { interp_trap(interp, ctx); return 0; }
  return value.as.bits;
}

static void *interp_value_addr(LainirInterpreter *interp, LainirValue value, const char *ctx) {
  if (value.kind == LAINIR_VALUE_ADDR) return value.as.addr;
  if (value.kind == LAINIR_VALUE_STRING) return (void *)value.as.string;
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

static LainirValue interp_eval_primitive(LainirInterpreter *interp, L1Expr *expr,
                                          LainirValue *ops) {
  const char *op = expr->data.primitive.opcode;
  uint32_t width = interp_type_bits(expr->data.primitive.result_ty);
  uint64_t a = ops[0].as.bits;
  uint64_t b = expr->data.primitive.operand_count > 1 ? ops[1].as.bits : 0;

  if (strcmp(op, "integer.add") == 0) return lainir_value_bits(a + b, width);
  if (strcmp(op, "integer.sub") == 0) return lainir_value_bits(a - b, width);
  if (strcmp(op, "integer.mul") == 0) return lainir_value_bits(a * b, width);
  if (strcmp(op, "integer.div") == 0) {
    if (b == 0) { interp_trap(interp, "div by zero"); return lainir_value_unit(); }
    return lainir_value_bits(a / b, width);
  }
  if (strcmp(op, "integer.eq") == 0) return lainir_value_bits(a == b, 1);
  if (strcmp(op, "integer.ne") == 0) return lainir_value_bits(a != b, 1);
  if (strcmp(op, "integer.lt") == 0) return lainir_value_bits((int64_t)a < (int64_t)b, 1);
  if (strcmp(op, "integer.le") == 0) return lainir_value_bits((int64_t)a <= (int64_t)b, 1);
  if (strcmp(op, "integer.gt") == 0) return lainir_value_bits((int64_t)a > (int64_t)b, 1);
  if (strcmp(op, "integer.ge") == 0) return lainir_value_bits((int64_t)a >= (int64_t)b, 1);

  interp_trap(interp, "unsupported primitive");
  return lainir_value_unit();
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
  if (entry->fn(args, arg_count, &result, &error, entry->user_data) != LAINIR_RUN_OK)
    interp_trap(interp, error ? error : "extern capability call failed");
  return result;
}

/* ═══════════════════════════════════════════════════════════════
 * Expression evaluator
 * ═══════════════════════════════════════════════════════════════ */

static LainirValue interp_eval_call(LainirInterpreter *interp, LainirFrame *frame,
                                     L1Expr *expr) {
  L1Subroutine *sub = interp_find_sub(interp, expr->data.call.fn_name);
  LainirValue *args = NULL;
  LainirValue result = lainir_value_unit();
  if (expr->data.call.arg_count) {
    args = calloc(expr->data.call.arg_count, sizeof(LainirValue));
    if (!args) { interp_trap(interp, "out of memory"); return result; }
  }
  for (uint32_t i = 0; i < expr->data.call.arg_count; i++) {
    args[i] = interp_eval_expr(interp, frame, expr->data.call.args[i]);
    if (interp->error) { free(args); return result; }
  }
  if (!sub) { free(args); interp_trap(interp, "call target not found"); return result; }
  if (sub->is_extern && !sub->blocks) {
    result = interp_call_host(interp, expr->data.call.fn_name, args, expr->data.call.arg_count);
    free(args); return result;
  }
  result = interp_call_sub(interp, sub, args, expr->data.call.arg_count);
  free(args);
  return result;
}

static LainirValue interp_eval_expr(LainirInterpreter *interp, LainirFrame *frame,
                                     L1Expr *expr) {
  if (!expr) return lainir_value_unit();
  switch (expr->kind) {
  case EXPR_CONST:
    return lainir_value_bits((uint64_t)expr->data.const_val, 32);
  case EXPR_STRING:
    return lainir_value_string(expr->data.str_val.content);
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
  case EXPR_DIV: {
    LainirValue l = interp_eval_expr(interp, frame, expr->data.bin.left);
    LainirValue r = interp_eval_expr(interp, frame, expr->data.bin.right);
    if (interp->error) return lainir_value_unit();
    uint64_t divisor = interp_value_bits(interp,r,"expected bits for div");
    if (divisor == 0) { interp_trap(interp, "div by zero"); return lainir_value_unit(); }
    return lainir_value_bits(interp_value_bits(interp,l,"expected bits for div") / divisor, l.bit_width ? l.bit_width : 32);
  }
  case EXPR_EQ: {
    LainirValue l = interp_eval_expr(interp, frame, expr->data.bin.left);
    LainirValue r = interp_eval_expr(interp, frame, expr->data.bin.right);
    if (interp->error) return lainir_value_unit();
    return lainir_value_bits(interp_value_bits(interp,l,"expected bits for eq") == interp_value_bits(interp,r,"expected bits for eq"), 1);
  }
  case EXPR_NE: {
    LainirValue l = interp_eval_expr(interp, frame, expr->data.bin.left);
    LainirValue r = interp_eval_expr(interp, frame, expr->data.bin.right);
    if (interp->error) return lainir_value_unit();
    return lainir_value_bits(interp_value_bits(interp,l,"expected bits for ne") != interp_value_bits(interp,r,"expected bits for ne"), 1);
  }
  case EXPR_LT: {
    LainirValue l = interp_eval_expr(interp, frame, expr->data.bin.left);
    LainirValue r = interp_eval_expr(interp, frame, expr->data.bin.right);
    if (interp->error) return lainir_value_unit();
    return lainir_value_bits((int64_t)interp_value_bits(interp,l,"expected bits for lt") < (int64_t)interp_value_bits(interp,r,"expected bits for lt"), 1);
  }
  case EXPR_LE: {
    LainirValue l = interp_eval_expr(interp, frame, expr->data.bin.left);
    LainirValue r = interp_eval_expr(interp, frame, expr->data.bin.right);
    if (interp->error) return lainir_value_unit();
    return lainir_value_bits((int64_t)interp_value_bits(interp,l,"expected bits for le") <= (int64_t)interp_value_bits(interp,r,"expected bits for le"), 1);
  }
  case EXPR_GT: {
    LainirValue l = interp_eval_expr(interp, frame, expr->data.bin.left);
    LainirValue r = interp_eval_expr(interp, frame, expr->data.bin.right);
    if (interp->error) return lainir_value_unit();
    return lainir_value_bits((int64_t)interp_value_bits(interp,l,"expected bits for gt") > (int64_t)interp_value_bits(interp,r,"expected bits for gt"), 1);
  }
  case EXPR_GE: {
    LainirValue l = interp_eval_expr(interp, frame, expr->data.bin.left);
    LainirValue r = interp_eval_expr(interp, frame, expr->data.bin.right);
    if (interp->error) return lainir_value_unit();
    return lainir_value_bits((int64_t)interp_value_bits(interp,l,"expected bits for ge") >= (int64_t)interp_value_bits(interp,r,"expected bits for ge"), 1);
  }
  case EXPR_FADD: {
    LainirValue l = interp_eval_expr(interp, frame, expr->data.bin.left);
    LainirValue r = interp_eval_expr(interp, frame, expr->data.bin.right);
    if (interp->error) return lainir_value_unit();
    double lv = *(double*)&l.as.bits;
    double rv = *(double*)&r.as.bits;
    double result = lv + rv;
    uint64_t bits;
    memcpy(&bits, &result, sizeof(bits));
    return lainir_value_bits(bits, 64);
  }
  case EXPR_FSUB: {
    LainirValue l = interp_eval_expr(interp, frame, expr->data.bin.left);
    LainirValue r = interp_eval_expr(interp, frame, expr->data.bin.right);
    if (interp->error) return lainir_value_unit();
    double lv = *(double*)&l.as.bits;
    double rv = *(double*)&r.as.bits;
    double result = lv - rv;
    uint64_t bits;
    memcpy(&bits, &result, sizeof(bits));
    return lainir_value_bits(bits, 64);
  }
  case EXPR_FMUL: {
    LainirValue l = interp_eval_expr(interp, frame, expr->data.bin.left);
    LainirValue r = interp_eval_expr(interp, frame, expr->data.bin.right);
    if (interp->error) return lainir_value_unit();
    double lv = *(double*)&l.as.bits;
    double rv = *(double*)&r.as.bits;
    double result = lv * rv;
    uint64_t bits;
    memcpy(&bits, &result, sizeof(bits));
    return lainir_value_bits(bits, 64);
  }
  case EXPR_FDIV: {
    LainirValue l = interp_eval_expr(interp, frame, expr->data.bin.left);
    LainirValue r = interp_eval_expr(interp, frame, expr->data.bin.right);
    if (interp->error) return lainir_value_unit();
    double lv = *(double*)&l.as.bits;
    double rv = *(double*)&r.as.bits;
    if (rv == 0.0) { interp_trap(interp, "fdiv by zero"); return lainir_value_unit(); }
    double result = lv / rv;
    uint64_t bits;
    memcpy(&bits, &result, sizeof(bits));
    return lainir_value_bits(bits, 64);
  }
  case EXPR_FEQ: {
    LainirValue l = interp_eval_expr(interp, frame, expr->data.bin.left);
    LainirValue r = interp_eval_expr(interp, frame, expr->data.bin.right);
    if (interp->error) return lainir_value_unit();
    double lv = *(double*)&l.as.bits;
    double rv = *(double*)&r.as.bits;
    return lainir_value_bits(lv == rv, 1);
  }
  case EXPR_FLT: {
    LainirValue l = interp_eval_expr(interp, frame, expr->data.bin.left);
    LainirValue r = interp_eval_expr(interp, frame, expr->data.bin.right);
    if (interp->error) return lainir_value_unit();
    double lv = *(double*)&l.as.bits;
    double rv = *(double*)&r.as.bits;
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
  case EXPR_PRIMITIVE: {
    uint32_t c = expr->data.primitive.operand_count;
    LainirValue *ops = calloc(c ? c : 1, sizeof(LainirValue));
    if (!ops) { interp_trap(interp, "out of memory"); return lainir_value_unit(); }
    for (uint32_t i = 0; i < c; i++) {
      ops[i] = interp_eval_expr(interp, frame, expr->data.primitive.operands[i]);
      if (interp->error) { free(ops); return lainir_value_unit(); }
    }
    LainirValue r = interp_eval_primitive(interp, expr, ops);
    free(ops); return r;
  }
  case EXPR_ALLOCA: {
    uint32_t sz = expr->data.alloca.byte_size;
    if (!sz) sz = interp_type_size(expr->data.alloca.element_ty);
    uint8_t *data = calloc(sz ? sz : 1, 1);
    if (!data) { interp_trap(interp, "out of memory"); return lainir_value_unit(); }
    if (interp->alloca_count == interp->alloca_cap) {
      uint32_t nc = interp->alloca_cap ? interp->alloca_cap * 2 : 4;
      LainirBuffer *na = realloc(interp->allocas, sizeof(LainirBuffer) * nc);
      if (!na) { free(data); interp_trap(interp, "out of memory"); return lainir_value_unit(); }
      interp->allocas = na; interp->alloca_cap = nc;
    }
    interp->allocas[interp->alloca_count].data = data;
    interp->allocas[interp->alloca_count].size = sz;
    interp->alloca_count++;
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
    return interp_load_typed(
        interp, interp_value_addr(interp, av, "expected address for load"),
        expr->data.load.ty);
  }
  case EXPR_FIELD: {
    LainirValue bv = interp_eval_expr(interp, frame, expr->data.field.base);
    uint32_t sz = interp_type_size(expr->data.field.field_ty);
    uint32_t bt = interp_type_bits(expr->data.field.field_ty);
    if (interp->error) return lainir_value_unit();
    if (!expr->data.field.field_ty) { sz = 4; bt = 32; }
    uint8_t *addr = (uint8_t *)interp_value_addr(interp, bv, "expected address for field");
    if (interp->error) return lainir_value_unit();
    if (expr->data.field.field_ty &&
        expr->data.field.field_ty->kind == TY_ADDR)
      return interp_load_typed(
          interp, addr + expr->data.field.field_index,
          expr->data.field.field_ty);
    return interp_load_bits(interp, addr + expr->data.field.field_index, sz, bt);
  }
  case EXPR_EVAL:
  case EXPR_CALL:
    return interp_eval_call(interp, frame, expr);
  case EXPR_CALL_INDIRECT:
    interp_trap(interp, "call_indirect not implemented");
    return lainir_value_unit();
  default:
    interp_trap(interp, "unsupported expression");
    return lainir_value_unit();
  }
}

/* ═══════════════════════════════════════════════════════════════
 * Block executor (structured IR — no terminators)
 * ═══════════════════════════════════════════════════════════════ */

static void interp_exec_block(LainirInterpreter *interp, LainirFrame *frame,
                               L1Block *block) {
  L1Instruction *inst = block->body;
  while (inst) {
    if (interp->should_return || interp->should_break || interp->should_continue)
      return;

    switch (inst->kind) {
    case INST_LET: {
      LainirValue value = interp_eval_expr(interp, frame, inst->data.let.val);
      if (interp->error) return;
      interp_set_local(frame, inst->data.let.name, value);
      break;
    }
    case INST_SET: {
      LainirValue value = interp_eval_expr(interp, frame, inst->data.set.val);
      if (interp->error) return;
      if (!interp_set_local(frame, inst->data.set.name, value))
        { interp_trap(interp, "set: unknown variable"); return; }
      break;
    }
    case INST_STORE: {
      LainirValue dest  = interp_eval_expr(interp, frame, inst->data.store.dest);
      LainirValue value = interp_eval_expr(interp, frame, inst->data.store.val);
      if (interp->error) return;
      uint8_t *addr = (uint8_t *)interp_value_addr(interp, dest, "expected address for store");
      if (interp->error) return;
      uint32_t width = interp_store_width(inst->data.store.store_ty, value);
      memcpy(addr, &value.as.bits, width);
      break;
    }
    case INST_IF: {
      LainirValue cond = interp_eval_expr(interp, frame, inst->data.if_stmt.condition);
      if (interp->error) return;
      if (interp_value_bits(interp, cond, "expected bits for if condition")) {
        if (inst->data.if_stmt.then_body)
          interp_exec_block(interp, frame, inst->data.if_stmt.then_body);
      } else {
        if (inst->data.if_stmt.else_body)
          interp_exec_block(interp, frame, inst->data.if_stmt.else_body);
      }
      break;
    }
    case INST_LOOP: {
      if (!inst->data.loop.body) break;
      int saved_break = interp->should_break;
      int saved_cont  = interp->should_continue;
      interp->should_break = 0;
      interp->should_continue = 0;
      while (1) {
        interp_exec_block(interp, frame, inst->data.loop.body);
        if (interp->error) return;
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
      if (interp->error) return;
      interp->should_return = 1;
      interp->return_value = value;
      return;
    }
    case INST_CALL:
      (void)interp_eval_expr(interp, frame, inst->data.call_inst.expr);
      if (interp->error) return;
      break;
    }
    inst = inst->next;
  }
}

/* ═══════════════════════════════════════════════════════════════
 * Subroutine call
 * ═══════════════════════════════════════════════════════════════ */

static LainirValue interp_call_sub(LainirInterpreter *interp, L1Subroutine *sub,
                                    const LainirValue *args, uint32_t arg_count) {
  LainirFrame frame; memset(&frame, 0, sizeof(frame));
  frame.sub = sub; frame.arg_count = arg_count;
  if (arg_count) {
    frame.args = calloc(arg_count, sizeof(LainirValue));
    if (!frame.args) { interp_trap(interp, "out of memory"); return lainir_value_unit(); }
    memcpy(frame.args, args, sizeof(LainirValue) * arg_count);
  }

  int saved_ret = interp->should_return;
  LainirValue saved_val = interp->return_value;
  interp->should_return = 0;

  L1Block *block = sub->blocks;
  while (block) {
    interp_exec_block(interp, &frame, block);
    if (interp->error) { interp_free_frame(&frame); interp->should_return = saved_ret; return lainir_value_unit(); }
    if (interp->should_return) break;
    block = block->next;
  }

  LainirValue result = interp->return_value;
  interp->should_return = saved_ret;
  interp->return_value = saved_val;
  interp_free_frame(&frame);
  return result;
}

/* ═══════════════════════════════════════════════════════════════
 * Public entry point
 * ═══════════════════════════════════════════════════════════════ */

LainirRunStatus lainir_run(const LainirRunRequest *request,
                          LainirValue *result_out, const char **error_out) {
  LainirInterpreter interp; memset(&interp, 0, sizeof(interp));
  if (!request || !request->module || !request->entry_name) {
    if (error_out) *error_out = "invalid run request";
    return LAINIR_RUN_TRAP;
  }
  interp.module = request->module;
  interp.caps = request->caps;
  interp_build_sub_index(&interp);
  L1Subroutine *entry = interp_find_sub(&interp, request->entry_name);
  if (!entry) {
    free(interp.sub_index);
    if (error_out) *error_out = "entry not found";
    return LAINIR_RUN_NO_ENTRY;
  }
  *result_out = interp_call_sub(&interp, entry, request->args, request->arg_count);
  free(interp.sub_index);
  if (interp.error) {
    interp_free_allocas(&interp);
    if (error_out) *error_out = interp.error;
    return LAINIR_RUN_TRAP;
  }
  /* Address results currently have no owner token in the public run API.
   * Preserve the historical usable-pointer behavior until that API gains one;
   * all scalar/string/unit runs release their aggregate arena here. */
  if (result_out->kind != LAINIR_VALUE_ADDR)
    interp_free_allocas(&interp);
  if (error_out) *error_out = NULL;
  return LAINIR_RUN_OK;
}
