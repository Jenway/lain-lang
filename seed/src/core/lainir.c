#include "lainir/core.h"

#include <stdlib.h>
#include <string.h>

L1Subroutine *g_subroutines_head = NULL;
L1Subroutine *g_current_sub = NULL;
L1Block *g_current_block = NULL;
uint32_t g_temp_counter = 0;
L1Block *g_scratch_blocks[8];
int g_scratch_count = 0;
L1ExportName *g_export_names_head = NULL;
L1ExportName *g_declared_module_names_head = NULL;
L1ExportName *g_declared_signature_names_head = NULL;
int g_has_explicit_exports = 0;

/*
 * infer_expr_type() is used by verifier and emitter as a query.  The
 * synthesized physical types it returns are not owned by either caller, so
 * allocating a fresh L1Type for every query turns a large compile into an
 * unbounded heap/address-space growth pattern.  These three shapes cover all
 * synthesized types in the current IR; parsed and procedure-owned types
 * remain ordinary allocations.
 */
static L1Type g_inferred_bits64 = {TY_BITS, 64};
static L1Type g_inferred_bits1 = {TY_BITS, 1};
static L1Type g_inferred_addr64 = {TY_ADDR, 64};

static void lainir_free_block_list(L1Block *block);

static void lainir_free_expr(L1Expr *expr) {
  uint32_t i;

  if (!expr)
    return;

  switch (expr->kind) {
  case EXPR_VAR:
    free(expr->data.var.name);
    break;
  case EXPR_LOAD:
    lainir_free_expr(expr->data.load.addr);
    break;
  case EXPR_LEA:
    lainir_free_expr(expr->data.lea.base);
    lainir_free_expr(expr->data.lea.idx);
    break;
  case EXPR_ADD:
  case EXPR_SUB:
  case EXPR_MUL:
  case EXPR_DIV:
  case EXPR_EQ:
  case EXPR_NE:
  case EXPR_LT:
  case EXPR_LE:
  case EXPR_GT:
  case EXPR_GE:
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
  case EXPR_FADD:
  case EXPR_FSUB:
  case EXPR_FMUL:
  case EXPR_FDIV:
  case EXPR_FEQ:
  case EXPR_FLT:
    lainir_free_expr(expr->data.bin.left);
    lainir_free_expr(expr->data.bin.right);
    break;
  case EXPR_POPCOUNT:
  case EXPR_CLZ:
  case EXPR_ROTL:
  case EXPR_INT2PTR:
  case EXPR_PTR2INT:
    lainir_free_expr(expr->data.unary.operand);
    break;
  case EXPR_ZEXT:
  case EXPR_SEXT:
  case EXPR_TRUNC:
  case EXPR_BITCAST:
    lainir_free_expr(expr->data.conversion.operand);
    break;
  case EXPR_CALL:
    free(expr->data.call.fn_name);
    for (i = 0; i < expr->data.call.arg_count; i++)
      lainir_free_expr(expr->data.call.args[i]);
    free(expr->data.call.args);
    break;
  case EXPR_STRING:
    free(expr->data.str_val.content);
    break;
  case EXPR_PRIMITIVE:
    free(expr->data.primitive.opcode);
    for (i = 0; i < expr->data.primitive.operand_count; i++)
      lainir_free_expr(expr->data.primitive.operands[i]);
    free(expr->data.primitive.operands);
    break;
  case EXPR_FIELD:
    lainir_free_expr(expr->data.field.base);
    break;
  case EXPR_EVAL:
    lainir_free_block_list(expr->data.eval.block);
    break;
  case EXPR_CALL_INDIRECT:
    lainir_free_expr(expr->data.call_indirect.fn_ptr);
    for (i = 0; i < expr->data.call_indirect.arg_count; i++)
      lainir_free_expr(expr->data.call_indirect.args[i]);
    free(expr->data.call_indirect.args);
    free(expr->data.call_indirect.param_tys);
    break;
  case EXPR_PROC_ADDR:
    free(expr->data.proc_addr.fn_name);
    break;
  default:
    break;
  }

  free(expr);
}

void lainir_free_expr_tree(L1Expr *expr) {
  lainir_free_expr(expr);
}

static void lainir_free_instruction_list(L1Instruction *inst) {
  while (inst) {
    L1Instruction *next = inst->next;
    switch (inst->kind) {
    case INST_LET:
      free(inst->data.let.name);
      lainir_free_expr(inst->data.let.val);
      break;
    case INST_SET:
      free(inst->data.set.name);
      lainir_free_expr(inst->data.set.val);
      break;
    case INST_STORE:
      lainir_free_expr(inst->data.store.dest);
      lainir_free_expr(inst->data.store.val);
      break;
    case INST_IF:
      lainir_free_expr(inst->data.if_stmt.condition);
      lainir_free_block_list(inst->data.if_stmt.then_body);
      lainir_free_block_list(inst->data.if_stmt.else_body);
      break;
    case INST_LOOP:
      free(inst->data.loop.label);
      lainir_free_block_list(inst->data.loop.body);
      break;
    case INST_BREAK:
    case INST_CONTINUE:
      free(inst->data.jump.label);
      break;
    case INST_RETURN:
      lainir_free_expr(inst->data.ret.val);
      break;
    case INST_CALL:
      lainir_free_expr(inst->data.call_inst.expr);
      break;
    }
    free(inst);
    inst = next;
  }
}

static void lainir_free_block_list(L1Block *block) {
  while (block) {
    L1Block *next = block->next;
    lainir_free_instruction_list(block->body);
    free(block);
    block = next;
  }
}

L1Type *lainir_new_type(L1TypeKind kind, uint32_t width) {
  L1Type *ty = calloc(1, sizeof(L1Type));
  ty->kind = kind;
  ty->width = width;
  return ty;
}

L1Expr *lainir_new_expr(L1ExprKind kind) {
  L1Expr *expr = calloc(1, sizeof(L1Expr));
  expr->kind = kind;
  return expr;
}

L1Instruction *lainir_new_instruction(L1InstKind kind) {
  L1Instruction *inst = calloc(1, sizeof(L1Instruction));
  inst->kind = kind;
  return inst;
}

L1Block *lainir_new_block(void) {
  L1Block *block = calloc(1, sizeof(L1Block));
  return block;
}

L1Subroutine *lainir_new_subroutine(const char *name) {
  L1Subroutine *sub = calloc(1, sizeof(L1Subroutine));
  sub->name = strdup(name);
  return sub;
}

void lainir_reset_module_state(void) {
  g_subroutines_head = NULL;
  g_current_sub = NULL;
  g_current_block = NULL;
  g_temp_counter = 0;
  g_scratch_count = 0;
  g_export_names_head = NULL;
  g_declared_module_names_head = NULL;
  g_declared_signature_names_head = NULL;
  g_has_explicit_exports = 0;
}

void lainir_free_subroutines(L1Subroutine *head) {
  while (head) {
    L1Subroutine *next = head->next;
    free(head->name);
    free(head->link_name);
    free(head->param_tys);
    if (head->param_names) {
      for (uint32_t i = 0; i < head->param_count; ++i)
        free(head->param_names[i]);
      free(head->param_names);
    }
    lainir_free_block_list(head->blocks);
    free(head);
    head = next;
  }
}

void append_instruction(L1Subroutine *sub, L1Instruction *inst) {
  (void)sub;
  if (!g_current_block) {
    abort();
  }
  if (!g_current_block->body) {
    g_current_block->body = inst;
    g_current_block->body_tail = inst;
  } else {
    g_current_block->body_tail->next = inst;
    g_current_block->body_tail = inst;
  }
}

void append_inst_to_block(L1Block *block, L1Instruction *inst) {
  if (block->body_tail) {
    block->body_tail->next = inst;
    block->body_tail = inst;
  } else {
    block->body = inst;
    block->body_tail = inst;
  }
}

L1Type *infer_expr_type(L1Expr *expr) {
  switch (expr->kind) {
  case EXPR_VAR:
    return expr->data.var.ty;
  case EXPR_CONST:
    return &g_inferred_bits64;
  case EXPR_LOAD:
    return expr->data.load.ty;
  case EXPR_ADD:
  case EXPR_SUB:
  case EXPR_MUL:
  case EXPR_DIV:
  case EXPR_SDIV:
  case EXPR_UDIV:
    return infer_expr_type(expr->data.bin.left);
  case EXPR_EQ:
  case EXPR_NE:
  case EXPR_LT:
  case EXPR_LE:
  case EXPR_GT:
  case EXPR_GE:
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
    return &g_inferred_bits1;
  case EXPR_STRING:
    /* A string literal denotes the address of immutable backing storage. */
    return expr->data.str_val.ty ? expr->data.str_val.ty
                                 : lainir_new_type(TY_ADDR, 64);
  case EXPR_CALL:
    return expr->data.call.ret_ty;
  case EXPR_EVAL:
    return expr->data.eval.ret_ty;
  case EXPR_ARG:
    return expr->data.arg.ty;
  case EXPR_FIELD:
    return expr->data.field.field_ty;
  case EXPR_ALLOCA:
    return expr->data.alloca.result_ty;
  case EXPR_LEA:
  case EXPR_INT2PTR:
  case EXPR_PROC_ADDR:
    return &g_inferred_addr64;
  case EXPR_PTR2INT:
    return &g_inferred_bits64;
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
  case EXPR_PRIMITIVE:
    return expr->data.primitive.result_ty;
  default:
    return &g_inferred_bits64;
  }
}

static void named_entry_append_unique(L1ExportName **head, const char *name) {
  L1ExportName *tail;
  L1ExportName *entry;

  for (tail = *head; tail; tail = tail->next) {
    if (strcmp(tail->name, name) == 0)
      return;
  }

  entry = calloc(1, sizeof(L1ExportName));
  entry->name = strdup(name);
  entry->next = NULL;

  if (!*head) {
    *head = entry;
    return;
  }

  for (tail = *head; tail->next; tail = tail->next)
    ;
  tail->next = entry;
}

void native_declare_module(const char *name) {
  named_entry_append_unique(&g_declared_module_names_head, name);
}

void native_declare_signature(const char *name) {
  named_entry_append_unique(&g_declared_signature_names_head, name);
}

void native_mark_export(const char *name) {
  g_has_explicit_exports = 1;
  named_entry_append_unique(&g_export_names_head, name);
}

int native_has_explicit_exports(void) {
  return g_has_explicit_exports;
}

int native_is_export_marked(const char *name) {
  L1ExportName *entry;

  for (entry = g_export_names_head; entry; entry = entry->next) {
    if (strcmp(entry->name, name) == 0)
      return 1;
  }
  return 0;
}
