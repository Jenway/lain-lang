#include "lainir.h"

#include <stdlib.h>
#include <string.h>

L1Subroutine *g_subroutines_head = NULL;
L1Subroutine *g_current_sub = NULL;
L1Block *g_current_block = NULL;
uint32_t g_temp_counter = 0;
uint32_t g_block_id_counter = 0;
L1Block *g_scratch_blocks[8];
int g_scratch_count = 0;
L1ExportName *g_export_names_head = NULL;
L1ExportName *g_declared_module_names_head = NULL;
L1ExportName *g_declared_signature_names_head = NULL;
int g_has_explicit_exports = 0;

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
    lainir_free_expr(expr->data.bin.left);
    lainir_free_expr(expr->data.bin.right);
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
  case EXPR_CALL_INDIRECT:
    lainir_free_expr(expr->data.call_indirect.fn_ptr);
    for (i = 0; i < expr->data.call_indirect.arg_count; i++)
      lainir_free_expr(expr->data.call_indirect.args[i]);
    free(expr->data.call_indirect.args);
    free(expr->data.call_indirect.param_tys);
    break;
  default:
    break;
  }

  free(expr);
}

static void lainir_free_instruction_list(L1Instruction *inst) {
  while (inst) {
    L1Instruction *next = inst->next;
    switch (inst->kind) {
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
      lainir_free_instruction_list(inst->data.if_stmt.then_body);
      lainir_free_instruction_list(inst->data.if_stmt.else_body);
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
    if (block->terminator) {
      switch (block->terminator->kind) {
      case TERM_RETURN:
        lainir_free_expr(block->terminator->data.ret_val);
        break;
      case TERM_COND_BRANCH:
        lainir_free_expr(block->terminator->data.cond_branch.condition);
        break;
      default:
        break;
      }
      free(block->terminator);
    }
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

L1Terminator *lainir_new_terminator(L1TerminatorKind kind) {
  L1Terminator *term = calloc(1, sizeof(L1Terminator));
  term->kind = kind;
  return term;
}

L1Block *lainir_new_block(int id) {
  L1Block *block = calloc(1, sizeof(L1Block));
  block->id = id;
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
  g_block_id_counter = 0;
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
    lainir_free_block_list(head->blocks);
    free(head);
    head = next;
  }
}

void append_instruction(L1Subroutine *sub, L1Instruction *inst) {
  (void)sub;
  if (!g_current_block) {
    fprintf(stderr, "ERROR: append_instruction with no current block\n");
    exit(1);
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
    return lainir_new_type(TY_BITS, 64);
  case EXPR_LOAD:
    return expr->data.load.ty;
  case EXPR_ADD:
  case EXPR_SUB:
    return infer_expr_type(expr->data.bin.left);
  case EXPR_CALL:
    return expr->data.call.ret_ty;
  case EXPR_ARG:
    return lainir_new_type(TY_BITS, 64);
  case EXPR_FIELD:
    return expr->data.field.field_ty;
  case EXPR_ALLOCA:
    return expr->data.alloca.result_ty;
  default:
    return lainir_new_type(TY_BITS, 64);
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

void scratch_terminator_to_inst(L1Block *block) {
  L1Instruction *ret;

  if (!block->terminator || block->terminator->kind != TERM_RETURN)
    return;

  ret = calloc(1, sizeof(L1Instruction));
  ret->kind = INST_RETURN;
  ret->data.ret.val = block->terminator->data.ret_val;
  if (!block->body) {
    block->body = ret;
    block->body_tail = ret;
  } else {
    block->body_tail->next = ret;
    block->body_tail = ret;
  }
  free(block->terminator);
  block->terminator = NULL;
}
