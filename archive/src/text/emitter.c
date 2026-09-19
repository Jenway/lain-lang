/**
 * lainir/emit_text.c — L1 IR Text Dump
 *
 * Human-readable L1 IR debug output.  Structured IR, no terminators.
 */

#include "lainir/artifact.h"
#include "lainir/emit.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================

typedef struct {
  LainirWriter writer;
  int failed;
} EmitState;

static int emitf(EmitState *state, const char *format, ...) {
  char local[256];
  char *buffer = local;
  va_list args;
  va_list copy;
  int length;

  if (state->failed)
    return 0;

  va_start(args, format);
  va_copy(copy, args);
  length = vsnprintf(local, sizeof(local), format, args);
  va_end(args);
  if (length < 0) {
    va_end(copy);
    state->failed = 1;
    return 0;
  }
  if ((size_t)length >= sizeof(local)) {
    buffer = malloc((size_t)length + 1);
    if (!buffer) {
      va_end(copy);
      state->failed = 1;
      return 0;
    }
    vsnprintf(buffer, (size_t)length + 1, format, copy);
  }
  va_end(copy);

  if (!state->writer.write(
          state->writer.context, buffer, (size_t)length))
    state->failed = 1;
  if (buffer != local)
    free(buffer);
  return !state->failed;
}

static void emit_l1_type(L1Type *ty, EmitState *out) {
  if (!ty) {
    emitf(out, "#unit");
    return;
  }
  switch (ty->kind) {
  case TY_BITS:
    emitf(out, "#bits<%d>", ty->width);
    break;
  case TY_ADDR:
    emitf(out, "#addr");
    break;
  case TY_UNIT:
    emitf(out, "#unit");
    break;
  case TY_NEVER:
    emitf(out, "#never");
    break;
  case TY_FLOATS:
    emitf(out, "#float<%d>", ty->width);
    break;
  case TY_SIMD:
    emitf(out, "simd%d", ty->width);
    break;
  }
}

static void emit_l1_expr(L1Expr *expr, EmitState *out);
static void emit_l1_block(L1Block *block, EmitState *out, const char *indent);

static void emit_c_string_literal(const char *text, EmitState *out) {
  emitf(out, "\"");
  for (const unsigned char *p = (const unsigned char *)(text ? text : "");
       *p; p++) {
    switch (*p) {
    case '\\': emitf(out, "\\\\"); break;
    case '"': emitf(out, "\\\""); break;
    case '\n': emitf(out, "\\n"); break;
    case '\r': emitf(out, "\\r"); break;
    case '\t': emitf(out, "\\t"); break;
    default:
      if (*p < 32 || *p >= 127)
        emitf(out, "\\x%02x", (unsigned)*p);
      else
        emitf(out, "%c", *p);
      break;
    }
  }
  emitf(out, "\"");
}

static void emit_data_literal(const uint8_t *bytes, uint32_t length,
                              EmitState *out) {
  emitf(out, "\"");
  for (uint32_t i = 0; i < length; i++) {
    unsigned char byte = bytes[i];
    switch (byte) {
    case '\\': emitf(out, "\\\\"); break;
    case '"': emitf(out, "\\\""); break;
    case '\n': emitf(out, "\\n"); break;
    case '\r': emitf(out, "\\r"); break;
    case '\t': emitf(out, "\\t"); break;
    default:
      if (byte < 32 || byte >= 127) emitf(out, "\\x%02x", (unsigned)byte);
      else emitf(out, "%c", byte);
      break;
    }
  }
  emitf(out, "\"");
}

static void emit_l1_expr(L1Expr *expr, EmitState *out) {
  const char *explicit_integer_op = NULL;
  if (!expr) {
    emitf(out, "???");
    return;
  }
  switch (expr->kind) {
  case EXPR_SDIV: explicit_integer_op = "sdiv"; break;
  case EXPR_UDIV: explicit_integer_op = "udiv"; break;
  case EXPR_SLT: explicit_integer_op = "slt"; break;
  case EXPR_SLE: explicit_integer_op = "sle"; break;
  case EXPR_SGT: explicit_integer_op = "sgt"; break;
  case EXPR_SGE: explicit_integer_op = "sge"; break;
  case EXPR_ULT: explicit_integer_op = "ult"; break;
  case EXPR_ULE: explicit_integer_op = "ule"; break;
  case EXPR_UGT: explicit_integer_op = "ugt"; break;
  case EXPR_UGE: explicit_integer_op = "uge"; break;
  default: break;
  }
  if (explicit_integer_op) {
    emitf(out, "#%s(", explicit_integer_op);
    emit_l1_expr(expr->data.bin.left, out);
    emitf(out, ", ");
    emit_l1_expr(expr->data.bin.right, out);
    emitf(out, ")");
    return;
  }
  switch (expr->kind) {
  case EXPR_VAR:
    emitf(out, "%%%s", expr->data.var.name);
    break;
  case EXPR_CONST:
    emitf(out, "%lld", (long long)expr->data.const_val);
    break;
  case EXPR_ARG:
    emitf(out, "%%arg%d", expr->data.arg.index);
    break;
  case EXPR_ADD:
    emitf(out, "#add(");
    emit_l1_expr(expr->data.bin.left, out);
    emitf(out, ", ");
    emit_l1_expr(expr->data.bin.right, out);
    emitf(out, ")");
    break;
  case EXPR_SUB:
    emitf(out, "#sub(");
    emit_l1_expr(expr->data.bin.left, out);
    emitf(out, ", ");
    emit_l1_expr(expr->data.bin.right, out);
    emitf(out, ")");
    break;
  case EXPR_MUL:
    emitf(out, "#mul(");
    emit_l1_expr(expr->data.bin.left, out);
    emitf(out, ", ");
    emit_l1_expr(expr->data.bin.right, out);
    emitf(out, ")");
    break;
  case EXPR_EQ:
    emitf(out, "#eq(");
    emit_l1_expr(expr->data.bin.left, out);
    emitf(out, ", ");
    emit_l1_expr(expr->data.bin.right, out);
    emitf(out, ")");
    break;
  case EXPR_NE:
    emitf(out, "#ne(");
    emit_l1_expr(expr->data.bin.left, out);
    emitf(out, ", ");
    emit_l1_expr(expr->data.bin.right, out);
    emitf(out, ")");
    break;
  case EXPR_FADD:
    emitf(out, "#fadd(");
    emit_l1_expr(expr->data.bin.left, out);
    emitf(out, ", ");
    emit_l1_expr(expr->data.bin.right, out);
    emitf(out, ")");
    break;
  case EXPR_FSUB:
    emitf(out, "#fsub(");
    emit_l1_expr(expr->data.bin.left, out);
    emitf(out, ", ");
    emit_l1_expr(expr->data.bin.right, out);
    emitf(out, ")");
    break;
  case EXPR_FMUL:
    emitf(out, "#fmul(");
    emit_l1_expr(expr->data.bin.left, out);
    emitf(out, ", ");
    emit_l1_expr(expr->data.bin.right, out);
    emitf(out, ")");
    break;
  case EXPR_FDIV:
    emitf(out, "#fdiv(");
    emit_l1_expr(expr->data.bin.left, out);
    emitf(out, ", ");
    emit_l1_expr(expr->data.bin.right, out);
    emitf(out, ")");
    break;
  case EXPR_FEQ:
    emitf(out, "#feq(");
    emit_l1_expr(expr->data.bin.left, out);
    emitf(out, ", ");
    emit_l1_expr(expr->data.bin.right, out);
    emitf(out, ")");
    break;
  case EXPR_FLT:
    emitf(out, "#flt(");
    emit_l1_expr(expr->data.bin.left, out);
    emitf(out, ", ");
    emit_l1_expr(expr->data.bin.right, out);
    emitf(out, ")");
    break;
  case EXPR_POPCOUNT:
    emitf(out, "#popcount(");
    emit_l1_expr(expr->data.unary.operand, out);
    emitf(out, ")");
    break;
  case EXPR_CLZ:
    emitf(out, "#clz(");
    emit_l1_expr(expr->data.unary.operand, out);
    emitf(out, ")");
    break;
  case EXPR_ROTL:
    emitf(out, "#rotl(");
    emit_l1_expr(expr->data.unary.operand, out);
    emitf(out, ")");
    break;
  case EXPR_INT2PTR:
    emitf(out, "#int2ptr(");
    emit_l1_expr(expr->data.unary.operand, out);
    emitf(out, ")");
    break;
  case EXPR_PTR2INT:
    emitf(out, "#ptr2int(");
    emit_l1_expr(expr->data.unary.operand, out);
    emitf(out, ")");
    break;
  case EXPR_ZEXT:
  case EXPR_SEXT:
  case EXPR_TRUNC:
    emitf(out, expr->kind == EXPR_ZEXT ? "#zext[" :
               expr->kind == EXPR_SEXT ? "#sext[" : "#trunc[");
    emit_l1_type(expr->data.conversion.target_ty, out);
    emitf(out, "](");
    emit_l1_expr(expr->data.conversion.operand, out);
    emitf(out, ")");
    break;
  case EXPR_BITCAST:
    emitf(out, "#bitcast[");
    emit_l1_type(expr->data.conversion.target_ty, out);
    emitf(out, "](");
    emit_l1_expr(expr->data.conversion.operand, out);
    emitf(out, ")");
    break;
  case EXPR_PROC_ADDR:
    emitf(out, "#proc_addr(%s)", expr->data.proc_addr.fn_name);
    break;
  case EXPR_LOAD:
    emitf(out, "#load");
    if (expr->data.load.ty) {
      emitf(out, "[");
      emit_l1_type(expr->data.load.ty, out);
      emitf(out, "]");
    }
    emitf(out, "(");
    emit_l1_expr(expr->data.load.addr, out);
    emitf(out, ")");
    break;
  case EXPR_LEA:
    emitf(out, "#lea(base=");
    emit_l1_expr(expr->data.lea.base, out);
    emitf(out, ", idx=");
    if (expr->data.lea.idx)
      emit_l1_expr(expr->data.lea.idx, out);
    else
      emitf(out, "0");
    emitf(out, ", scale=%d, offset=%d)", expr->data.lea.scale,
            expr->data.lea.offset);
    break;
  case EXPR_CALL:
    emitf(out, "#call %s(", expr->data.call.fn_name);
    for (uint32_t i = 0; i < expr->data.call.arg_count; i++) {
      emit_l1_expr(expr->data.call.args[i], out);
      if (i < expr->data.call.arg_count - 1)
        emitf(out, ", ");
    }
    emitf(out, ")");
    break;
  case EXPR_EVAL:
    emitf(out, "#eval {\n");
    emit_l1_block(expr->data.eval.block, out, "  ");
    emitf(out, "}");
    break;
  case EXPR_STRING:
    emit_c_string_literal(expr->data.str_val.content, out);
    break;
  case EXPR_DATA_ADDR:
    emitf(out, "#data_addr(%s)", expr->data.data_addr.name);
    break;
  case EXPR_ALLOCA:
    emitf(out, "#alloca(");
    if (expr->data.alloca.byte_size > 0)
      emitf(out, "%d", expr->data.alloca.byte_size);
    else
      emit_l1_type(expr->data.alloca.element_ty, out);
    emitf(out, ")");
    break;
  case EXPR_CALL_INDIRECT:
    emitf(out, "#call_indirect[(");
    for (uint32_t i = 0; i < expr->data.call_indirect.param_count; i++) {
      emit_l1_type(expr->data.call_indirect.param_tys[i], out);
      if (i + 1 < expr->data.call_indirect.param_count)
        emitf(out, ", ");
    }
    emitf(out, ") -> ");
    emit_l1_type(expr->data.call_indirect.ret_ty, out);
    emitf(out, "](");
    emit_l1_expr(expr->data.call_indirect.fn_ptr, out);
    for (uint32_t i = 0; i < expr->data.call_indirect.arg_count; i++) {
      emitf(out, ", ");
      emit_l1_expr(expr->data.call_indirect.args[i], out);
    }
    emitf(out, ")");
    break;
  default:
    emitf(out, "expr(kind=%d)", expr->kind);
    break;
  }
}

static void emit_l1_block(
    L1Block *block,
    EmitState *out,
    const char *indent) {
  L1Instruction *inst = block->body;
  while (inst) {
    switch (inst->kind) {
    case INST_LET:
      emitf(out, "%s#let %%%s: ", indent, inst->data.let.name);
      emit_l1_type(inst->data.let.ty ? inst->data.let.ty
                                     : infer_expr_type(inst->data.let.val),
                   out);
      emitf(out, " = ");
      emit_l1_expr(inst->data.let.val, out);
      emitf(out, "\n");
      break;
    case INST_SET:
      emitf(out, "%s%%%s: ", indent, inst->data.set.name);
      emit_l1_type(inst->data.set.ty ? inst->data.set.ty
                                     : infer_expr_type(inst->data.set.val),
                   out);
      emitf(out, " = ");
      emit_l1_expr(inst->data.set.val, out);
      emitf(out, "\n");
      break;
    case INST_STORE:
      emitf(out, "%s#store[", indent);
      emit_l1_type(inst->data.store.store_ty
                       ? inst->data.store.store_ty
                       : infer_expr_type(inst->data.store.val),
                   out);
      emitf(out, "] ");
      emit_l1_expr(inst->data.store.val, out);
      emitf(out, ", ");
      emit_l1_expr(inst->data.store.dest, out);
      emitf(out, "\n");
      break;
    case INST_IF:
      emitf(out, "%s#if ", indent);
      emit_l1_expr(inst->data.if_stmt.condition, out);
      emitf(out, " {\n");
      if (inst->data.if_stmt.then_body) {
        char sub_indent[64];
        snprintf(sub_indent, sizeof(sub_indent), "%s  ", indent);
        emit_l1_block(inst->data.if_stmt.then_body, out, sub_indent);
      }
      if (inst->data.if_stmt.else_body) {
        emitf(out, "%s} else {\n", indent);
        char sub_indent[64];
        snprintf(sub_indent, sizeof(sub_indent), "%s  ", indent);
        emit_l1_block(inst->data.if_stmt.else_body, out, sub_indent);
      }
      emitf(out, "%s}\n", indent);
      break;
    case INST_LOOP:
      emitf(out, "%s#loop", indent);
      if (inst->data.loop.label)
        emitf(out, " :%s", inst->data.loop.label);
      emitf(out, " {\n");
      if (inst->data.loop.body) {
        char sub_indent[64];
        snprintf(sub_indent, sizeof(sub_indent), "%s  ", indent);
        emit_l1_block(inst->data.loop.body, out, sub_indent);
      }
      emitf(out, "%s}\n", indent);
      break;
    case INST_BREAK:
      emitf(out, "%s#break", indent);
      if (inst->data.jump.label)
        emitf(out, " :%s", inst->data.jump.label);
      emitf(out, "\n");
      break;
    case INST_CONTINUE:
      emitf(out, "%s#continue", indent);
      if (inst->data.jump.label)
        emitf(out, " :%s", inst->data.jump.label);
      emitf(out, "\n");
      break;
    case INST_RETURN:
      emitf(out, "%s#return", indent);
      if (inst->data.ret.val) {
        emitf(out, " ");
        emit_l1_expr(inst->data.ret.val, out);
      }
      emitf(out, "\n");
      break;
    case INST_CALL:
      emitf(out, "%s", indent);
      emit_l1_expr(inst->data.call_inst.expr, out);
      emitf(out, "\n");
      break;
    }
    inst = inst->next;
  }
}

static void emit_l1_subroutine(L1Subroutine *sub, EmitState *out) {
  if (sub->is_data) {
    uint32_t used = sub->data_size;
    while (used && sub->data_bytes[used - 1] == 0) used--;
    emitf(out, "#data %s(%u, %u, ", sub->name, sub->data_size,
          sub->data_alignment);
    emit_data_literal(sub->data_bytes, used, out);
    emitf(out, ");\n\n");
    return;
  }
  if (sub->is_extern && !sub->blocks) {
    emitf(out, "#extern #proc %s(",
            sub->link_name ? sub->link_name : sub->name);
    for (uint32_t i = 0; i < sub->param_count; i++) {
      emit_l1_type(sub->param_tys[i], out);
      emitf(out, " %%%s", sub->param_names && sub->param_names[i]
                            ? sub->param_names[i] : "arg");
      if ((!sub->param_names || !sub->param_names[i])) emitf(out, "%d", i);
      if (i < sub->param_count - 1)
        emitf(out, ", ");
    }
    emitf(out, ") -> ");
    emit_l1_type(sub->ret_ty, out);
    emitf(out, ";\n\n");
    return;
  }
  if (!sub->blocks)
    return;
  emitf(out, "#proc %s(", sub->name);
  for (uint32_t i = 0; i < sub->param_count; i++) {
    emit_l1_type(sub->param_tys[i], out);
    emitf(out, " %%%s", sub->param_names && sub->param_names[i]
                          ? sub->param_names[i] : "arg");
    if ((!sub->param_names || !sub->param_names[i])) emitf(out, "%d", i);
    if (i < sub->param_count - 1)
      emitf(out, ", ");
  }
  emitf(out, ") -> ");
  emit_l1_type(sub->ret_ty, out);
  emitf(out, " {\n");

  L1Block *block = sub->blocks;
  while (block) {
    emit_l1_block(block, out, "  ");
    block = block->next;
  }
  emitf(out, "}\n\n");
}

int lainir_emit_text_module(
    LainirWriter writer,
    L1Subroutine *head,
    L1Diagnostic *diagnostic) {
  EmitState state = {
      .writer = writer,
      .failed = 0,
  };
  L1Subroutine *sub = head;

  if (!writer.write) {
    if (diagnostic) {
      memset(diagnostic, 0, sizeof(*diagnostic));
      diagnostic->code = 3001;
      snprintf(
          diagnostic->message,
          sizeof(diagnostic->message),
          "text emitter requires a writer");
    }
    return 0;
  }
  while (sub) {
    emit_l1_subroutine(sub, &state);
    if (state.failed)
      break;
    sub = sub->next;
  }
  if (state.failed && diagnostic) {
    memset(diagnostic, 0, sizeof(*diagnostic));
    diagnostic->code = 3002;
    snprintf(
        diagnostic->message,
        sizeof(diagnostic->message),
        "text writer rejected output");
  }
  return !state.failed;
}
