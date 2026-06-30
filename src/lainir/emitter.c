/**
 * lainir/emitter.c — C Code Emission Backend
 *
 * Walks L1 IR and emits C source code.
 * Structured IR: IF/LOOP contain nested blocks, no terminators.
 */

#include "lainir.h"
#include <string.h>


// ============================================================================

static void emit_c_type(L1Type *ty, FILE *out) {
  if (!ty) {
    fprintf(out, "void");
    return;
  }
  if (ty->kind == TY_BITS) {
    switch (ty->width) {
    case 1:
    case 8:  fprintf(out, "uint8_t");  break;
    case 16: fprintf(out, "uint16_t"); break;
    case 32: fprintf(out, "uint32_t"); break;
    case 64: fprintf(out, "uint64_t"); break;
    default: fprintf(out, "uint64_t"); break;
    }
  } else if (ty->kind == TY_ADDR) {
    fprintf(out, "void*");
  } else {
    fprintf(out, "void");
  }
}

static void emit_c_value_type(L1Type *ty, FILE *out) {
  if (!ty) { fprintf(out, "uint64_t"); return; }
  emit_c_type(ty, out);
}

static void emit_c_expr(L1Expr *expr, FILE *out);

static int is_zero_arg_main(L1Subroutine *sub) {
  return sub && strcmp(sub->name, "main") == 0 && sub->param_count == 0;
}

static int expr_is_unit_value(L1Expr *expr) {
  if (!expr) return 1;
  L1Type *ty = infer_expr_type(expr);
  return ty && ty->kind == TY_UNIT;
}

static void emit_c_expr(L1Expr *expr, FILE *out) {
  if (!expr) { fprintf(out, "0"); return; }
  switch (expr->kind) {
  case EXPR_VAR:
    fprintf(out, "%s", expr->data.var.name);
    break;
  case EXPR_CONST:
    fprintf(out, "%lld", (long long)expr->data.const_val);
    break;
  case EXPR_ARG:
    fprintf(out, "arg%d", expr->data.arg_idx);
    break;
  case EXPR_LOAD:
    fprintf(out, "*("); emit_c_value_type(expr->data.load.ty, out);
    fprintf(out, "*)( "); emit_c_expr(expr->data.load.addr, out);
    fprintf(out, ")");
    break;
  case EXPR_LEA:
    if (expr->data.lea.idx) {
      fprintf(out, "(void*)((uintptr_t)(");
      emit_c_expr(expr->data.lea.base, out);
      fprintf(out, ") + (uintptr_t)(");
      emit_c_expr(expr->data.lea.idx, out);
      fprintf(out, ") * %d + %d)", expr->data.lea.scale, expr->data.lea.offset);
    } else {
      fprintf(out, "(void*)((uintptr_t)(");
      emit_c_expr(expr->data.lea.base, out);
      fprintf(out, ") + %d)", expr->data.lea.offset);
    }
    break;
  case EXPR_ADD:
    fprintf(out, "(");
    emit_c_expr(expr->data.bin.left, out);
    fprintf(out, " + ");
    emit_c_expr(expr->data.bin.right, out);
    fprintf(out, ")");
    break;
  case EXPR_SUB:
    fprintf(out, "(");
    emit_c_expr(expr->data.bin.left, out);
    fprintf(out, " - ");
    emit_c_expr(expr->data.bin.right, out);
    fprintf(out, ")");
    break;
  case EXPR_MUL:
    fprintf(out, "(");
    emit_c_expr(expr->data.bin.left, out);
    fprintf(out, " * ");
    emit_c_expr(expr->data.bin.right, out);
    fprintf(out, ")");
    break;
  case EXPR_DIV:
    fprintf(out, "(");
    emit_c_expr(expr->data.bin.left, out);
    fprintf(out, " / ");
    emit_c_expr(expr->data.bin.right, out);
    fprintf(out, ")");
    break;
  case EXPR_EQ:
    fprintf(out, "(");
    emit_c_expr(expr->data.bin.left, out);
    fprintf(out, " == ");
    emit_c_expr(expr->data.bin.right, out);
    fprintf(out, ")");
    break;
  case EXPR_NE:
    fprintf(out, "(");
    emit_c_expr(expr->data.bin.left, out);
    fprintf(out, " != ");
    emit_c_expr(expr->data.bin.right, out);
    fprintf(out, ")");
    break;
  case EXPR_LT:
    fprintf(out, "((int64_t)(");
    emit_c_expr(expr->data.bin.left, out);
    fprintf(out, ") < (int64_t)(");
    emit_c_expr(expr->data.bin.right, out);
    fprintf(out, "))");
    break;
  case EXPR_LE:
    fprintf(out, "((int64_t)(");
    emit_c_expr(expr->data.bin.left, out);
    fprintf(out, ") <= (int64_t)(");
    emit_c_expr(expr->data.bin.right, out);
    fprintf(out, "))");
    break;
  case EXPR_GT:
    fprintf(out, "((int64_t)(");
    emit_c_expr(expr->data.bin.left, out);
    fprintf(out, ") > (int64_t)(");
    emit_c_expr(expr->data.bin.right, out);
    fprintf(out, "))");
    break;
  case EXPR_GE:
    fprintf(out, "((int64_t)(");
    emit_c_expr(expr->data.bin.left, out);
    fprintf(out, ") >= (int64_t)(");
    emit_c_expr(expr->data.bin.right, out);
    fprintf(out, "))");
    break;
  case EXPR_FADD:
    fprintf(out, "(");
    emit_c_expr(expr->data.bin.left, out);
    fprintf(out, " + ");
    emit_c_expr(expr->data.bin.right, out);
    fprintf(out, ")");
    break;
  case EXPR_FSUB:
    fprintf(out, "(");
    emit_c_expr(expr->data.bin.left, out);
    fprintf(out, " - ");
    emit_c_expr(expr->data.bin.right, out);
    fprintf(out, ")");
    break;
  case EXPR_FMUL:
    fprintf(out, "(");
    emit_c_expr(expr->data.bin.left, out);
    fprintf(out, " * ");
    emit_c_expr(expr->data.bin.right, out);
    fprintf(out, ")");
    break;
  case EXPR_FDIV:
    fprintf(out, "(");
    emit_c_expr(expr->data.bin.left, out);
    fprintf(out, " / ");
    emit_c_expr(expr->data.bin.right, out);
    fprintf(out, ")");
    break;
  case EXPR_FEQ:
    fprintf(out, "(");
    emit_c_expr(expr->data.bin.left, out);
    fprintf(out, " == ");
    emit_c_expr(expr->data.bin.right, out);
    fprintf(out, ")");
    break;
  case EXPR_FLT:
    fprintf(out, "(");
    emit_c_expr(expr->data.bin.left, out);
    fprintf(out, " < ");
    emit_c_expr(expr->data.bin.right, out);
    fprintf(out, ")");
    break;
  case EXPR_POPCOUNT:
    fprintf(out, "__builtin_popcountll(");
    emit_c_expr(expr->data.unary.operand, out);
    fprintf(out, ")");
    break;
  case EXPR_CLZ:
    fprintf(out, "__builtin_clzll(");
    emit_c_expr(expr->data.unary.operand, out);
    fprintf(out, ")");
    break;
  case EXPR_ROTL:
    fprintf(out, "__builtin_rotl64(");
    emit_c_expr(expr->data.unary.operand, out);
    fprintf(out, ")");
    break;
  case EXPR_INT2PTR:
    fprintf(out, "(void*)(uintptr_t)(");
    emit_c_expr(expr->data.unary.operand, out);
    fprintf(out, ")");
    break;
  case EXPR_PTR2INT:
    fprintf(out, "(uint64_t)(uintptr_t)(");
    emit_c_expr(expr->data.unary.operand, out);
    fprintf(out, ")");
    break;
  case EXPR_CALL:
    fprintf(out, "%s(", expr->data.call.fn_name);
    for (uint32_t i = 0; i < expr->data.call.arg_count; i++) {
      emit_c_expr(expr->data.call.args[i], out);
      if (i < expr->data.call.arg_count - 1) fprintf(out, ", ");
    }
    fprintf(out, ")");
    break;
  case EXPR_STRING:
    fprintf(out, "\"");
    for (char *p = expr->data.str_val.content; *p; p++) {
      switch (*p) {
      case '\n': fprintf(out, "\\n"); break;
      case '\t': fprintf(out, "\\t"); break;
      case '\r': fprintf(out, "\\r"); break;
      case '\\': fprintf(out, "\\\\"); break;
      case '"':  fprintf(out, "\\\""); break;
      default:   fputc(*p, out); break;
      }
    }
    fprintf(out, "\"");
    break;
  case EXPR_PRIMITIVE: {
    const char *op = expr->data.primitive.opcode;
    uint32_t nops = expr->data.primitive.operand_count;
    if (nops == 1) {
      if (strcmp(op, "cpu.popcount") == 0) {
        fprintf(out, "__builtin_popcountll(");
        emit_c_expr(expr->data.primitive.operands[0], out); fprintf(out, ")");
      } else if (strcmp(op, "cpu.leading-zeros") == 0) {
        fprintf(out, "__builtin_clzll(");
        emit_c_expr(expr->data.primitive.operands[0], out); fprintf(out, ")");
      } else if (strcmp(op, "cpu.bswap") == 0) {
        fprintf(out, "__builtin_bswap64(");
        emit_c_expr(expr->data.primitive.operands[0], out); fprintf(out, ")");
      } else if (strcmp(op, "int2ptr") == 0) {
        fprintf(out, "(void*)(uintptr_t)(");
        emit_c_expr(expr->data.primitive.operands[0], out); fprintf(out, ")");
      } else if (strcmp(op, "ptr2int") == 0) {
        fprintf(out, "(uint64_t)(uintptr_t)(");
        emit_c_expr(expr->data.primitive.operands[0], out); fprintf(out, ")");
      } else {
        fprintf(out, "0");
      }
    } else if (nops >= 2) {
      L1Expr *left  = expr->data.primitive.operands[0];
      L1Expr *right = expr->data.primitive.operands[1];
      if (strcmp(op, "integer.add") == 0 || strcmp(op, "float.add") == 0) {
        fprintf(out, "("); emit_c_expr(left,out); fprintf(out," + "); emit_c_expr(right,out); fprintf(out,")");
      } else if (strcmp(op, "integer.sub") == 0 || strcmp(op, "float.sub") == 0) {
        fprintf(out, "("); emit_c_expr(left,out); fprintf(out," - "); emit_c_expr(right,out); fprintf(out,")");
      } else if (strcmp(op, "integer.mul") == 0 || strcmp(op, "float.mul") == 0) {
        fprintf(out, "("); emit_c_expr(left,out); fprintf(out," * "); emit_c_expr(right,out); fprintf(out,")");
      } else if (strcmp(op, "integer.div") == 0 || strcmp(op, "float.div") == 0) {
        fprintf(out, "("); emit_c_expr(left,out); fprintf(out," / "); emit_c_expr(right,out); fprintf(out,")");
      } else if (strcmp(op, "integer.eq") == 0 || strcmp(op, "float.eq") == 0) {
        fprintf(out, "("); emit_c_expr(left,out); fprintf(out," == "); emit_c_expr(right,out); fprintf(out,")");
      } else if (strcmp(op, "integer.ne") == 0) {
        fprintf(out, "("); emit_c_expr(left,out); fprintf(out," != "); emit_c_expr(right,out); fprintf(out,")");
      } else if (strcmp(op, "integer.lt") == 0 || strcmp(op, "float.lt") == 0) {
        fprintf(out, "((int64_t)("); emit_c_expr(left,out); fprintf(out,") < (int64_t)("); emit_c_expr(right,out); fprintf(out,"))");
      } else if (strcmp(op, "integer.le") == 0) {
        fprintf(out, "((int64_t)("); emit_c_expr(left,out); fprintf(out,") <= (int64_t)("); emit_c_expr(right,out); fprintf(out,"))");
      } else if (strcmp(op, "integer.gt") == 0) {
        fprintf(out, "((int64_t)("); emit_c_expr(left,out); fprintf(out,") > (int64_t)("); emit_c_expr(right,out); fprintf(out,"))");
      } else if (strcmp(op, "integer.ge") == 0) {
        fprintf(out, "((int64_t)("); emit_c_expr(left,out); fprintf(out,") >= (int64_t)("); emit_c_expr(right,out); fprintf(out,"))");
      } else if (strcmp(op, "cpu.rotate-left") == 0) {
        fprintf(out, "__builtin_rotateleft64("); emit_c_expr(left,out); fprintf(out,", "); emit_c_expr(right,out); fprintf(out,")");
      } else if (strcmp(op, "cpu.extract-bits") == 0) {
        fprintf(out, "("); emit_c_expr(left,out); fprintf(out," & "); emit_c_expr(right,out); fprintf(out,")");
      } else {
        fprintf(out, "0");
      }
    } else {
      fprintf(out, "0");
    }
  } break;
  case EXPR_ALLOCA:
    if (expr->data.alloca.byte_size > 0)
      fprintf(out, "alloca(%u)", expr->data.alloca.byte_size);
    else {
      fprintf(out, "alloca(sizeof(");
      emit_c_type(expr->data.alloca.element_ty, out);
      fprintf(out, "))");
    }
    break;
  case EXPR_FIELD:
    fprintf(out, "*(");
    emit_c_value_type(expr->data.field.field_ty, out);
    fprintf(out, "*)((uint8_t*)(");
    emit_c_expr(expr->data.field.base, out);
    fprintf(out, ") + %u)", expr->data.field.field_index);
    break;
  case EXPR_CALL_INDIRECT:
    fprintf(out, "((");
    emit_c_type(expr->data.call_indirect.ret_ty, out);
    fprintf(out, " (*)(");
    for (uint32_t i = 0; i < expr->data.call_indirect.param_count; i++) {
      emit_c_type(expr->data.call_indirect.param_tys[i], out);
      if (i < expr->data.call_indirect.param_count - 1) fprintf(out, ", ");
    }
    fprintf(out, "))(");
    emit_c_expr(expr->data.call_indirect.fn_ptr, out);
    fprintf(out, "))(");
    for (uint32_t i = 0; i < expr->data.call_indirect.arg_count; i++) {
      emit_c_expr(expr->data.call_indirect.args[i], out);
      if (i < expr->data.call_indirect.arg_count - 1) fprintf(out, ", ");
    }
    fprintf(out, ")");
    break;
  case EXPR_EVAL:
    fprintf(out, "/* #eval %s(...) */ 0", expr->data.eval.fn_name);
    break;
  }
}

static void emit_c_instructions_block(L1Block *block, L1Subroutine *sub,
                                       FILE *out, const char *indent);

static void emit_c_instructions_block(L1Block *block, L1Subroutine *sub,
                                       FILE *out, const char *indent) {
  int is_unit_main = (sub && is_zero_arg_main(sub) &&
                      (!sub->ret_ty || sub->ret_ty->kind == TY_UNIT));
  L1Instruction *inst = block->body;
  while (inst) {
    switch (inst->kind) {
    case INST_LET:
      {
        L1Type *ty = infer_expr_type(inst->data.let.val);
        fprintf(out, "%s", indent);
        if (ty) emit_c_type(ty, out);
        else   fprintf(out, "uint64_t");
        fprintf(out, " %s = ", inst->data.let.name);
        emit_c_expr(inst->data.let.val, out);
        fprintf(out, ";\n");
      }
      break;
    case INST_SET:
      {
        L1Type *ty = infer_expr_type(inst->data.set.val);
        fprintf(out, "%s", indent);
        fprintf(out, "%s = ", inst->data.set.name);
        emit_c_expr(inst->data.set.val, out);
        fprintf(out, ";\n");
      }
      break;
    case INST_STORE: {
      L1Type *store_ty = inst->data.store.store_ty;
      if (!store_ty) store_ty = infer_expr_type(inst->data.store.val);
      fprintf(out, "%s*(", indent);
      emit_c_type(store_ty, out);
      fprintf(out, "*)( ");
      emit_c_expr(inst->data.store.dest, out);
      fprintf(out, " ) = ");
      emit_c_expr(inst->data.store.val, out);
      fprintf(out, ";\n");
      break;
    }
    case INST_IF:
      fprintf(out, "%sif (", indent);
      emit_c_expr(inst->data.if_stmt.condition, out);
      fprintf(out, ") {\n");
      if (inst->data.if_stmt.then_body) {
        char sub_indent[64];
        snprintf(sub_indent, sizeof(sub_indent), "%s    ", indent);
        emit_c_instructions_block(inst->data.if_stmt.then_body,
                                   sub, out, sub_indent);
      }
      if (inst->data.if_stmt.else_body) {
        fprintf(out, "%s} else {\n", indent);
        char sub_indent[64];
        snprintf(sub_indent, sizeof(sub_indent), "%s    ", indent);
        emit_c_instructions_block(inst->data.if_stmt.else_body,
                                   sub, out, sub_indent);
      }
      fprintf(out, "%s}\n", indent);
      break;
    case INST_LOOP:
      fprintf(out, "%swhile (1) {\n", indent);
      if (inst->data.loop.body) {
        char sub_indent[64];
        snprintf(sub_indent, sizeof(sub_indent), "%s    ", indent);
        emit_c_instructions_block(inst->data.loop.body,
                                   sub, out, sub_indent);
      }
      fprintf(out, "%s}\n", indent);
      break;
    case INST_BREAK:
      fprintf(out, "%sbreak;\n", indent);
      break;
    case INST_CONTINUE:
      fprintf(out, "%scontinue;\n", indent);
      break;
    case INST_RETURN:
      if (is_unit_main) {
        fprintf(out, "%sreturn 0;\n", indent);
      } else if (is_zero_arg_main(sub) && expr_is_unit_value(inst->data.ret.val)) {
        fprintf(out, "%s", indent);
        emit_c_expr(inst->data.ret.val, out);
        fprintf(out, ";\n%sreturn 0;\n", indent);
      } else {
        fprintf(out, "%sreturn ", indent);
        emit_c_expr(inst->data.ret.val, out);
        fprintf(out, ";\n");
      }
      break;
    case INST_CALL:
      fprintf(out, "%s", indent);
      emit_c_expr(inst->data.call_inst.expr, out);
      fprintf(out, ";\n");
      break;
    }
    inst = inst->next;
  }
}

static void emit_c_subroutine(
    L1Subroutine *sub, FILE *out, int with_native_runtime) {
  if (!sub->blocks) return;
  int is_main = (strcmp(sub->name, "main") == 0 && sub->param_count == 0);
  if (is_main) {
    if (with_native_runtime) {
      fprintf(out, "int main(int argc, char **argv) {\n");
      fprintf(out, "    native_set_args(argc, argv);\n");
    } else {
      fprintf(out, "int main(void) {\n");
    }
  } else {
    emit_c_type(sub->ret_ty, out);
    fprintf(out, " %s(", sub->link_name ? sub->link_name : sub->name);
    for (uint32_t i = 0; i < sub->param_count; i++) {
      emit_c_type(sub->param_tys[i], out);
      fprintf(out, " arg%d%s", i, (i == sub->param_count - 1) ? "" : ", ");
    }
    fprintf(out, ") {\n");
  }

  L1Block *block = sub->blocks;
  while (block) {
    emit_c_instructions_block(block, sub, out, "    ");
    block = block->next;
  }
  fprintf(out, "}\n\n");
}

// ── Forward declarations & helpers (unchanged) ──

static int should_skip_native_decl(const char *name) {
  static const char *native_funcs[] = {
    "native_set_args", "native_get_arg_count", "native_get_arg",
    "native_read_file", "native_file_len", "native_lex_and_group",
    "native_init_scheme", "native_run_pipeline", "native_emit_module_to_file",
    "native_get_subroutines", NULL
  };
  for (int i = 0; native_funcs[i]; i++)
    if (strcmp(name, native_funcs[i]) == 0) return 1;
  return 0;
}

static const char *sub_emit_name(L1Subroutine *sub) {
  return sub->link_name ? sub->link_name : sub->name;
}

static int is_default_extern_stub(L1Subroutine *sub) {
  if (!sub->is_extern || sub->blocks || sub->link_name) return 0;
  if (!sub->ret_ty || sub->ret_ty->kind != TY_UNIT) return 0;
  if (sub->param_count != 2) return 0;
  if (!sub->param_tys || !sub->param_tys[0] || !sub->param_tys[1]) return 0;
  if (sub->param_tys[0]->kind != TY_ADDR) return 0;
  if (sub->param_tys[1]->kind != TY_BITS || sub->param_tys[1]->width != 32) return 0;
  return 1;
}

static int sub_decl_score(L1Subroutine *sub) {
  int score = 0;
  if (sub->blocks)      score += 8;
  if (sub->link_name)    score += 4;
  if (sub->ret_ty && sub->ret_ty->kind != TY_UNIT) score += 2;
  if (!is_default_extern_stub(sub)) score += 1;
  return score;
}

static void emit_forward_decls(
    FILE *out, L1Subroutine *head, int with_native_runtime) {
  const char *printed_symbols[1024];
  int printed_symbol_count = 0;

  if (with_native_runtime) {
    fprintf(out, "// Native runtime forward declarations\n");
    fprintf(out, "void native_set_args(int argc, char **argv);\n");
    fprintf(out, "int32_t native_get_arg_count(void);\n");
    fprintf(out, "const char *native_get_arg(int32_t idx);\n");
    fprintf(out, "const uint8_t *native_read_file(const char *path);\n");
    fprintf(out, "uint32_t native_file_len(void);\n");
    fprintf(out, "void *native_lex_and_group(const uint8_t *src, uint32_t len);\n");
    fprintf(out, "void *native_init_scheme(void);\n");
    fprintf(out, "int32_t native_run_pipeline(void *ctx, void *root_group);\n");
    fprintf(out, "void native_emit_module_to_file(void *subs, const char *output_path);\n");
    fprintf(out, "void *native_get_subroutines(void);\n\n");
  }

  for (L1Subroutine *sub = head; sub; sub = sub->next) {
    const char *emit_name;
    int already_printed = 0;
    int sub_score;

    if (!(sub->blocks || sub->is_extern)) continue;
    emit_name = sub_emit_name(sub);
    if (with_native_runtime && should_skip_native_decl(emit_name)) continue;

    for (int i = 0; i < printed_symbol_count; i++) {
      if (strcmp(printed_symbols[i], emit_name) == 0) {
        already_printed = 1; break;
      }
    }
    if (already_printed) continue;

    if (with_native_runtime) {
      if (should_skip_native_decl(sub->name)) continue;
      if (sub->link_name && should_skip_native_decl(sub->link_name)) continue;
    }

    sub_score = sub_decl_score(sub);
    int shadowed = 0;
    for (L1Subroutine *other = sub->next; other; other = other->next) {
      if ((other->blocks || other->is_extern) &&
          strcmp(sub_emit_name(other), emit_name) == 0 &&
          sub_decl_score(other) > sub_score) {
        shadowed = 1; break;
      }
    }
    if (shadowed) continue;

    if (strcmp(sub->name, "main") == 0 && sub->param_count == 0) {
      if (with_native_runtime) fprintf(out, "int main(int argc, char **argv);\n");
      else                     fprintf(out, "int main(void);\n");
    } else {
      emit_c_type(sub->ret_ty, out);
      fprintf(out, " %s(", emit_name);
      for (uint32_t i = 0; i < sub->param_count; i++) {
        emit_c_type(sub->param_tys[i], out);
        fprintf(out, " arg%d%s", i, (i == sub->param_count - 1) ? "" : ", ");
      }
      if (sub->param_count == 0) fprintf(out, "void");
      fprintf(out, ");\n");
    }

    if (printed_symbol_count < 1024)
      printed_symbols[printed_symbol_count++] = emit_name;
  }
  fprintf(out, "\n");
}

static void emit_default_extern_stubs(
    FILE *out, L1Subroutine *head, int with_native_runtime) {
  if (with_native_runtime) return;
  for (L1Subroutine *sub = head; sub; sub = sub->next) {
    if (!sub->is_extern || sub->blocks) continue;
    const char *emit_name = sub_emit_name(sub);
    if (strcmp(sub->name, "main") == 0 && sub->param_count == 0) continue;

    fprintf(out, "__attribute__((weak)) ");
    emit_c_type(sub->ret_ty, out);
    fprintf(out, " %s(", emit_name);
    for (uint32_t i = 0; i < sub->param_count; i++) {
      emit_c_type(sub->param_tys[i], out);
      fprintf(out, " arg%d%s", i, (i == sub->param_count - 1) ? "" : ", ");
    }
    if (sub->param_count == 0) fprintf(out, "void");
    fprintf(out, ") {\n");
    for (uint32_t i = 0; i < sub->param_count; i++)
      fprintf(out, "    (void)arg%d;\n", i);
    if (!sub->ret_ty || sub->ret_ty->kind == TY_UNIT) {
      fprintf(out, "    return;\n");
    } else if (sub->ret_ty->kind == TY_ADDR) {
      fprintf(out, "    return (void*)0;\n");
    } else {
      fprintf(out, "    return 0;\n");
    }
    fprintf(out, "}\n\n");
  }
}

void lainir_emit_c_module(FILE *out, L1Subroutine *head, int with_native_runtime) {
  int has_main = 0;
  fprintf(out, "#include <stdint.h>\n");
  fprintf(out, "#include <string.h>\n");
  fprintf(out, "#include <alloca.h>\n\n");

  emit_forward_decls(out, head, with_native_runtime);
  emit_default_extern_stubs(out, head, with_native_runtime);

  for (L1Subroutine *sub = head; sub; sub = sub->next) {
    if (strcmp(sub->name, "main") == 0 && !sub->is_extern && sub->blocks)
      has_main = 1;
    emit_c_subroutine(sub, out, with_native_runtime);
  }

  if (!with_native_runtime && !has_main) {
    fprintf(out, "int main(void) {\n");
    fprintf(out, "    return 0;\n");
    fprintf(out, "}\n");
  }
}

void lainir_emit_c_module_to_file(
    L1Subroutine *head, const char *output_path, int with_native_runtime) {
  FILE *out = fopen(output_path, "w");
  if (!out) {
    fprintf(stderr, "Error: cannot open output file: %s\n", output_path);
    return;
  }
  lainir_emit_c_module(out, head, with_native_runtime);
  fclose(out);
}
