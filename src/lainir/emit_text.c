/**
 * lainir/emit_text.c — L1 IR Text Dump
 *
 * Human-readable L1 IR debug output.  Structured IR, no terminators.
 */

#include "lainir.h"


// ============================================================================

static void emit_l1_type(L1Type *ty, FILE *out) {
  if (!ty) { fprintf(out, "#unit"); return; }
  switch (ty->kind) {
  case TY_BITS:   fprintf(out, "i%d", ty->width); break;
  case TY_ADDR:   fprintf(out, "addr"); break;
  case TY_UNIT:   fprintf(out, "#unit"); break;
  case TY_NEVER:  fprintf(out, "#never"); break;
  case TY_FLOATS: fprintf(out, "f%d", ty->width); break;
  case TY_SIMD:   fprintf(out, "simd%d", ty->width); break;
  }
}

static void emit_l1_expr(L1Expr *expr, FILE *out);

static void emit_l1_expr(L1Expr *expr, FILE *out) {
  if (!expr) { fprintf(out, "???"); return; }
  switch (expr->kind) {
  case EXPR_VAR:
    fprintf(out, "%%%s", expr->data.var.name);
    break;
  case EXPR_CONST:
    fprintf(out, "%lld", (long long)expr->data.const_val);
    break;
  case EXPR_ARG:
    fprintf(out, "%%arg%d", expr->data.arg_idx);
    break;
  case EXPR_ADD:
    fprintf(out, "#add(");
    emit_l1_expr(expr->data.bin.left, out);
    fprintf(out, ", ");
    emit_l1_expr(expr->data.bin.right, out);
    fprintf(out, ")");
    break;
  case EXPR_SUB:
    fprintf(out, "#sub(");
    emit_l1_expr(expr->data.bin.left, out);
    fprintf(out, ", ");
    emit_l1_expr(expr->data.bin.right, out);
    fprintf(out, ")");
    break;
  case EXPR_LOAD:
    fprintf(out, "#load(");
    emit_l1_expr(expr->data.load.addr, out);
    fprintf(out, ")");
    break;
  case EXPR_LEA:
    fprintf(out, "#lea(base=");
    emit_l1_expr(expr->data.lea.base, out);
    fprintf(out, ", idx=");
    emit_l1_expr(expr->data.lea.idx, out);
    fprintf(out, ", scale=%d, offset=%d)", expr->data.lea.scale,
            expr->data.lea.offset);
    break;
  case EXPR_CALL:
    fprintf(out, "#call %s(", expr->data.call.fn_name);
    for (uint32_t i = 0; i < expr->data.call.arg_count; i++) {
      emit_l1_expr(expr->data.call.args[i], out);
      if (i < expr->data.call.arg_count - 1) fprintf(out, ", ");
    }
    fprintf(out, ")");
    break;
  case EXPR_EVAL:
    fprintf(out, "#eval %s(", expr->data.eval.fn_name);
    for (uint32_t i = 0; i < expr->data.eval.arg_count; i++) {
      emit_l1_expr(expr->data.eval.args[i], out);
      if (i < expr->data.eval.arg_count - 1) fprintf(out, ", ");
    }
    fprintf(out, ")");
    break;
  case EXPR_STRING:
    fprintf(out, "\"%s\"", expr->data.str_val.content);
    break;
  case EXPR_PRIMITIVE:
    fprintf(out, "#primitive %s(", expr->data.primitive.opcode);
    for (uint32_t i = 0; i < expr->data.primitive.operand_count; i++) {
      emit_l1_expr(expr->data.primitive.operands[i], out);
      if (i < expr->data.primitive.operand_count - 1) fprintf(out, ", ");
    }
    fprintf(out, ")");
    break;
  case EXPR_ALLOCA:
    fprintf(out, "#alloca(");
    if (expr->data.alloca.byte_size > 0)
      fprintf(out, "%d", expr->data.alloca.byte_size);
    else
      emit_l1_type(expr->data.alloca.element_ty, out);
    fprintf(out, ")");
    break;
  case EXPR_FIELD:
    fprintf(out, "#field[%d](", expr->data.field.field_index);
    emit_l1_expr(expr->data.field.base, out);
    fprintf(out, ")");
    break;
  case EXPR_CALL_INDIRECT:
    fprintf(out, "#call_indirect(");
    emit_l1_expr(expr->data.call_indirect.fn_ptr, out);
    for (uint32_t i = 0; i < expr->data.call_indirect.arg_count; i++) {
      fprintf(out, ", ");
      emit_l1_expr(expr->data.call_indirect.args[i], out);
    }
    fprintf(out, ")");
    break;
  default:
    fprintf(out, "expr(kind=%d)", expr->kind);
    break;
  }
}

static void emit_l1_block(L1Block *block, FILE *out, const char *indent);

static void emit_l1_block(L1Block *block, FILE *out, const char *indent) {
  L1Instruction *inst = block->body;
  while (inst) {
    switch (inst->kind) {
    case INST_LET:
      fprintf(out, "%s%%%s = ", indent, inst->data.let.name);
      emit_l1_expr(inst->data.let.val, out);
      fprintf(out, "\n");
      break;
    case INST_SET:
      fprintf(out, "%s%%%s = ", indent, inst->data.set.name);
      emit_l1_expr(inst->data.set.val, out);
      fprintf(out, "\n");
      break;
    case INST_STORE:
      fprintf(out, "%s#store ", indent);
      emit_l1_expr(inst->data.store.val, out);
      fprintf(out, ", ");
      emit_l1_expr(inst->data.store.dest, out);
      fprintf(out, "\n");
      break;
    case INST_IF:
      fprintf(out, "%s#if ", indent);
      emit_l1_expr(inst->data.if_stmt.condition, out);
      fprintf(out, " {\n");
      if (inst->data.if_stmt.then_body) {
        char sub_indent[64];
        snprintf(sub_indent, sizeof(sub_indent), "%s  ", indent);
        emit_l1_block(inst->data.if_stmt.then_body, out, sub_indent);
      }
      if (inst->data.if_stmt.else_body) {
        fprintf(out, "%s} else {\n", indent);
        char sub_indent[64];
        snprintf(sub_indent, sizeof(sub_indent), "%s  ", indent);
        emit_l1_block(inst->data.if_stmt.else_body, out, sub_indent);
      }
      fprintf(out, "%s}\n", indent);
      break;
    case INST_LOOP:
      fprintf(out, "%s#loop", indent);
      if (inst->data.loop.label)
        fprintf(out, " :%s", inst->data.loop.label);
      fprintf(out, " {\n");
      if (inst->data.loop.body) {
        char sub_indent[64];
        snprintf(sub_indent, sizeof(sub_indent), "%s  ", indent);
        emit_l1_block(inst->data.loop.body, out, sub_indent);
      }
      fprintf(out, "%s}\n", indent);
      break;
    case INST_BREAK:
      fprintf(out, "%s#break", indent);
      if (inst->data.jump.label)
        fprintf(out, " :%s", inst->data.jump.label);
      fprintf(out, "\n");
      break;
    case INST_CONTINUE:
      fprintf(out, "%s#continue", indent);
      if (inst->data.jump.label)
        fprintf(out, " :%s", inst->data.jump.label);
      fprintf(out, "\n");
      break;
    case INST_RETURN:
      fprintf(out, "%s#return ", indent);
      if (inst->data.ret.val)
        emit_l1_expr(inst->data.ret.val, out);
      else
        fprintf(out, "#unit");
      fprintf(out, "\n");
      break;
    case INST_CALL:
      fprintf(out, "%s#call ", indent);
      emit_l1_expr(inst->data.call_inst.expr, out);
      fprintf(out, "\n");
      break;
    }
    inst = inst->next;
  }
}

static void emit_l1_subroutine(L1Subroutine *sub, FILE *out) {
  if (!sub->blocks) return;
  fprintf(out, "#proc %s(", sub->name);
  for (uint32_t i = 0; i < sub->param_count; i++) {
    emit_l1_type(sub->param_tys[i], out);
    fprintf(out, " %%arg%d", i);
    if (i < sub->param_count - 1) fprintf(out, ", ");
  }
  fprintf(out, ") -> ");
  emit_l1_type(sub->ret_ty, out);
  fprintf(out, " {\n");

  L1Block *block = sub->blocks;
  while (block) {
    emit_l1_block(block, out, "  ");
    block = block->next;
  }
  fprintf(out, "}\n\n");
}

void lainir_emit_text_module(FILE *out, L1Subroutine *head) {
  L1Subroutine *sub = head;
  while (sub) {
    emit_l1_subroutine(sub, out);
    sub = sub->next;
  }
}

void lainir_emit_text_module_to_file(L1Subroutine *head, const char *output_path) {
  FILE *out = fopen(output_path, "w");
  if (!out) {
    fprintf(stderr, "Error: cannot open L1 output: %s\n", output_path);
    return;
  }
  lainir_emit_text_module(out, head);
  fclose(out);
}
