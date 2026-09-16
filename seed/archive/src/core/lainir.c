#include "lainir/builder.h"

#include <stdlib.h>
#include <string.h>

/*
 * Everything below is per-builder or pure.  The IR data model itself lives in
 * core.h and owns nothing; this file owns allocation, construction and the
 * type query.
 *
 * infer_expr_type() is a pure query used by the parser, the verifier and the
 * emitter.  The physical types it synthesizes are immutable process-wide
 * constants: a fresh L1Type per query would make a large compile grow memory
 * without bound, and a mutable shared one would let any caller silently rewrite
 * the inferred type of every other expression in the process.  Types that
 * belong to a node are returned borrowed from that node.
 */
static const L1Type k_inferred_bits64 = {TY_BITS, 64};
static const L1Type k_inferred_bits1 = {TY_BITS, 1};
static const L1Type k_inferred_addr64 = {TY_ADDR, 64};

typedef struct BuilderAllocation {
  struct BuilderAllocation *next;
  void *pointer;
} BuilderAllocation;

struct L1Builder {
  LainirAllocator allocator;
  BuilderAllocation *allocations;
  L1Subroutine *root;
};

static void *heap_allocate(void *context, size_t size) {
  (void)context;
  return calloc(1, size);
}

static void heap_release(void *context, void *pointer) {
  (void)context;
  free(pointer);
}

L1Builder *lainir_builder_new(const LainirAllocator *allocator) {
  LainirAllocator chosen;
  L1Builder *builder;

  if (allocator) {
    chosen = *allocator;
  } else {
    chosen.allocate = heap_allocate;
    chosen.release = heap_release;
    chosen.context = NULL;
  }
  if (!chosen.allocate || !chosen.release)
    return NULL;
  builder = chosen.allocate(chosen.context, sizeof(*builder));
  if (!builder)
    return NULL;
  builder->allocator = chosen;
  builder->allocations = NULL;
  builder->root = NULL;
  return builder;
}

void *lainir_builder_allocate(L1Builder *builder, size_t size) {
  BuilderAllocation *record;
  void *pointer;

  if (!builder || size == 0)
    return NULL;
  pointer = builder->allocator.allocate(builder->allocator.context, size);
  if (!pointer)
    return NULL;
  record = builder->allocator.allocate(builder->allocator.context,
                                       sizeof(*record));
  if (!record) {
    builder->allocator.release(builder->allocator.context, pointer);
    return NULL;
  }
  record->pointer = pointer;
  record->next = builder->allocations;
  builder->allocations = record;
  return pointer;
}

char *lainir_builder_copy_string(L1Builder *builder, const char *text) {
  size_t length;
  char *copy;

  if (!text)
    return NULL;
  length = strlen(text);
  copy = lainir_builder_allocate(builder, length + 1);
  if (!copy)
    return NULL;
  memcpy(copy, text, length + 1);
  return copy;
}

void *lainir_builder_grow(L1Builder *builder, void *pointer,
                          size_t old_size, size_t new_size) {
  void *grown;
  if (new_size <= old_size)
    return pointer;
  grown = lainir_builder_allocate(builder, new_size);
  if (!grown)
    return NULL;
  if (pointer && old_size)
    memcpy(grown, pointer, old_size);
  return grown;
}

void lainir_builder_free(L1Builder *builder) {
  LainirAllocator allocator;
  BuilderAllocation *record;

  if (!builder)
    return;
  allocator = builder->allocator;
  record = builder->allocations;
  while (record) {
    BuilderAllocation *next = record->next;
    allocator.release(allocator.context, record->pointer);
    allocator.release(allocator.context, record);
    record = next;
  }
  allocator.release(allocator.context, builder);
}

L1Subroutine *lainir_builder_root(const L1Builder *builder) {
  return builder ? builder->root : NULL;
}

void lainir_builder_set_root(L1Builder *builder, L1Subroutine *root) {
  if (builder)
    builder->root = root;
}

L1Type *lainir_new_type(L1Builder *builder, L1TypeKind kind, uint32_t width) {
  L1Type *ty = lainir_builder_allocate(builder, sizeof(*ty));
  if (!ty)
    return NULL;
  ty->kind = kind;
  ty->width = width;
  return ty;
}

L1Expr *lainir_new_expr(L1Builder *builder, L1ExprKind kind) {
  L1Expr *expr = lainir_builder_allocate(builder, sizeof(*expr));
  if (!expr)
    return NULL;
  expr->kind = kind;
  return expr;
}

L1Instruction *lainir_new_instruction(L1Builder *builder, L1InstKind kind) {
  L1Instruction *inst = lainir_builder_allocate(builder, sizeof(*inst));
  if (!inst)
    return NULL;
  inst->kind = kind;
  return inst;
}

L1Block *lainir_new_block(L1Builder *builder) {
  return lainir_builder_allocate(builder, sizeof(L1Block));
}

L1Subroutine *lainir_new_subroutine(L1Builder *builder, const char *name) {
  L1Subroutine *sub = lainir_builder_allocate(builder, sizeof(*sub));
  if (!sub)
    return NULL;
  sub->name = lainir_builder_copy_string(builder, name);
  if (!sub->name)
    return NULL;
  return sub;
}

L1Type *infer_expr_type(L1Expr *expr) {
  switch (expr->kind) {
  case EXPR_VAR:
    return expr->data.var.ty;
  case EXPR_CONST:
    return (L1Type *)&k_inferred_bits64;
  case EXPR_LOAD:
    return expr->data.load.ty;
  case EXPR_ADD:
  case EXPR_SUB:
  case EXPR_MUL:
  case EXPR_SDIV:
  case EXPR_UDIV:
    return infer_expr_type(expr->data.bin.left);
  case EXPR_EQ:
  case EXPR_NE:
  case EXPR_SLT:
  case EXPR_SLE:
  case EXPR_SGT:
  case EXPR_SGE:
  case EXPR_ULT:
  case EXPR_ULE:
  case EXPR_UGT:
  case EXPR_UGE:
  case EXPR_FEQ:
  case EXPR_FLT:
    return (L1Type *)&k_inferred_bits1;
  case EXPR_STRING:
    /* A string literal denotes the address of immutable backing storage. */
    return expr->data.str_val.ty ? expr->data.str_val.ty
                                 : (L1Type *)&k_inferred_addr64;
  case EXPR_DATA_ADDR:
    return expr->data.data_addr.ty ? expr->data.data_addr.ty
                                   : (L1Type *)&k_inferred_addr64;
  case EXPR_CALL:
    return expr->data.call.ret_ty;
  case EXPR_EVAL:
    return expr->data.eval.ret_ty;
  case EXPR_ARG:
    return expr->data.arg.ty;
  case EXPR_ALLOCA:
    return expr->data.alloca.result_ty;
  case EXPR_LEA:
  case EXPR_INT2PTR:
  case EXPR_PROC_ADDR:
    return (L1Type *)&k_inferred_addr64;
  case EXPR_PTR2INT:
    return (L1Type *)&k_inferred_bits64;
  case EXPR_CALL_INDIRECT:
    return expr->data.call_indirect.ret_ty;
  case EXPR_ZEXT:
  case EXPR_SEXT:
  case EXPR_TRUNC:
  case EXPR_BITCAST:
    return expr->data.conversion.target_ty;
  case EXPR_FADD:
  case EXPR_FSUB:
  case EXPR_FMUL:
  case EXPR_FDIV:
    return infer_expr_type(expr->data.bin.left);
  default:
    return (L1Type *)&k_inferred_bits64;
  }
}
