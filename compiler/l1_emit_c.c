/**
 * compiler/l1_emit_c.c — C Code Emission Backend
 *
 * Walks L1 IR and emits C source code.
 */

#include "l1_types.h"


// ============================================================================

static void emit_c_type(L1Type *ty, FILE *out) {
  if (!ty) {
    fprintf(out, "void");
    return;
  }
  if (ty->kind == TY_BITS) {
    fprintf(out, "uint%d_t", ty->width);
  } else if (ty->kind == TY_ADDR) {
    fprintf(out, "void*");
  } else {
    fprintf(out, "void");
  }
}

static void emit_c_expr(L1Expr *expr, FILE *out);

static void emit_c_expr(L1Expr *expr, FILE *out) {
  if (!expr) {
    fprintf(out, "0");
    return;
  }
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
    fprintf(out, "*(");
    emit_c_type(expr->data.load.ty, out);
    fprintf(out, "*)( ");
    emit_c_expr(expr->data.load.addr, out);
    fprintf(out, ")");
    break;
  case EXPR_LEA:
    fprintf(out, "(void*)((uintptr_t)(");
    emit_c_expr(expr->data.lea.base, out);
    fprintf(out, ") + (uintptr_t)(");
    emit_c_expr(expr->data.lea.idx, out);
    fprintf(out, ") * %d + %d)", expr->data.lea.scale, expr->data.lea.offset);
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
  case EXPR_CALL:
    fprintf(out, "%s(", expr->data.call.fn_name);
    for (uint32_t i = 0; i < expr->data.call.arg_count; i++) {
      emit_c_expr(expr->data.call.args[i], out);
      if (i < expr->data.call.arg_count - 1)
        fprintf(out, ", ");
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
        emit_c_expr(expr->data.primitive.operands[0], out);
        fprintf(out, ")");
      } else if (strcmp(op, "cpu.leading-zeros") == 0) {
        fprintf(out, "__builtin_clzll(");
        emit_c_expr(expr->data.primitive.operands[0], out);
        fprintf(out, ")");
      } else if (strcmp(op, "cpu.bswap") == 0) {
        fprintf(out, "__builtin_bswap64(");
        emit_c_expr(expr->data.primitive.operands[0], out);
        fprintf(out, ")");
      } else {
        fprintf(out, "0");
      }
    } else if (nops >= 2) {
      L1Expr *left = expr->data.primitive.operands[0];
      L1Expr *right = expr->data.primitive.operands[1];
      if (strcmp(op, "integer.add") == 0 || strcmp(op, "float.add") == 0) {
        fprintf(out, "("); emit_c_expr(left, out); fprintf(out, " + "); emit_c_expr(right, out); fprintf(out, ")");
      } else if (strcmp(op, "integer.sub") == 0 || strcmp(op, "float.sub") == 0) {
        fprintf(out, "("); emit_c_expr(left, out); fprintf(out, " - "); emit_c_expr(right, out); fprintf(out, ")");
      } else if (strcmp(op, "integer.mul") == 0 || strcmp(op, "float.mul") == 0) {
        fprintf(out, "("); emit_c_expr(left, out); fprintf(out, " * "); emit_c_expr(right, out); fprintf(out, ")");
      } else if (strcmp(op, "integer.div") == 0 || strcmp(op, "float.div") == 0) {
        fprintf(out, "("); emit_c_expr(left, out); fprintf(out, " / "); emit_c_expr(right, out); fprintf(out, ")");
      } else if (strcmp(op, "integer.eq") == 0 || strcmp(op, "float.eq") == 0) {
        fprintf(out, "("); emit_c_expr(left, out); fprintf(out, " == "); emit_c_expr(right, out); fprintf(out, ")");
      } else if (strcmp(op, "integer.ne") == 0) {
        fprintf(out, "("); emit_c_expr(left, out); fprintf(out, " != "); emit_c_expr(right, out); fprintf(out, ")");
      } else if (strcmp(op, "integer.lt") == 0 || strcmp(op, "float.lt") == 0) {
        fprintf(out, "((int64_t)("); emit_c_expr(left, out); fprintf(out, ") < (int64_t)("); emit_c_expr(right, out); fprintf(out, "))");
      } else if (strcmp(op, "integer.le") == 0) {
        fprintf(out, "((int64_t)("); emit_c_expr(left, out); fprintf(out, ") <= (int64_t)("); emit_c_expr(right, out); fprintf(out, "))");
      } else if (strcmp(op, "integer.gt") == 0) {
        fprintf(out, "((int64_t)("); emit_c_expr(left, out); fprintf(out, ") > (int64_t)("); emit_c_expr(right, out); fprintf(out, "))");
      } else if (strcmp(op, "integer.ge") == 0) {
        fprintf(out, "((int64_t)("); emit_c_expr(left, out); fprintf(out, ") >= (int64_t)("); emit_c_expr(right, out); fprintf(out, "))");
      } else if (strcmp(op, "cpu.rotate-left") == 0) {
        fprintf(out, "__builtin_rotateleft64("); emit_c_expr(left, out); fprintf(out, ", "); emit_c_expr(right, out); fprintf(out, ")");
      } else if (strcmp(op, "cpu.extract-bits") == 0) {
        fprintf(out, "("); emit_c_expr(left, out); fprintf(out, " & "); emit_c_expr(right, out); fprintf(out, ")");
      } else {
        fprintf(out, "0");
      }
    } else {
      fprintf(out, "0");
    }
  } break;
  case EXPR_ALLOCA:
    if (expr->data.alloca.byte_size > 0) {
      fprintf(out, "alloca(%u)", expr->data.alloca.byte_size);
    } else {
      fprintf(out, "alloca(sizeof(");
      emit_c_type(expr->data.alloca.element_ty, out);
      fprintf(out, "))");
    }
    break;
  case EXPR_FIELD:
    fprintf(out, "*(");
    emit_c_type(expr->data.field.field_ty, out);
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
      if (i < expr->data.call_indirect.param_count - 1)
        fprintf(out, ", ");
    }
    fprintf(out, "))(");
    emit_c_expr(expr->data.call_indirect.fn_ptr, out);
    fprintf(out, "))(");
    for (uint32_t i = 0; i < expr->data.call_indirect.arg_count; i++) {
      emit_c_expr(expr->data.call_indirect.args[i], out);
      if (i < expr->data.call_indirect.arg_count - 1)
        fprintf(out, ", ");
    }
    fprintf(out, ")");
    break;
  }
}

// Forward declarations with sub parameter for void-main handling
static void emit_c_block_terminator(L1Block *block, L1Subroutine *sub, FILE *out);
static void emit_c_instruction(L1Block *block, L1Instruction *inst, L1Subroutine *sub, FILE *out);
static void emit_c_instructions(L1Block *block, L1Subroutine *sub, FILE *out);

// Helper: emit a single instruction (used for nested if bodies)
static void emit_c_instruction(L1Block *block, L1Instruction *inst, L1Subroutine *sub, FILE *out) {
  int is_unit_main = (sub && strcmp(sub->name, "main") == 0 && sub->param_count == 0 &&
                      (!sub->ret_ty || sub->ret_ty->kind == TY_UNIT));
  switch (inst->kind) {
  case INST_SET:
    fprintf(out, "        auto %s = ", inst->data.set.name);
    emit_c_expr(inst->data.set.val, out);
    fprintf(out, ";\n");
    break;
  case INST_STORE: {
    L1Type *store_ty = inst->data.store.store_ty;
    if (!store_ty) store_ty = infer_expr_type(inst->data.store.val);
    fprintf(out, "        *(");
    emit_c_type(store_ty, out);
    fprintf(out, "*)(");
    emit_c_expr(inst->data.store.dest, out);
    fprintf(out, ") = ");
    emit_c_expr(inst->data.store.val, out);
    fprintf(out, ";\n");
    break;
  }
  case INST_CALL:
    fprintf(out, "        ");
    emit_c_expr(inst->data.call_inst.expr, out);
    fprintf(out, ";\n");
    break;
  case INST_RETURN:
    if (is_unit_main) {
      fprintf(out, "        return 0;\n");
    } else {
      fprintf(out, "        return ");
      emit_c_expr(inst->data.ret.val, out);
      fprintf(out, ";\n");
    }
    break;
  default:
    break;
  }
}

static void emit_c_instructions(L1Block *block, L1Subroutine *sub, FILE *out) {
  int is_unit_main = (sub && strcmp(sub->name, "main") == 0 && sub->param_count == 0 &&
                      (!sub->ret_ty || sub->ret_ty->kind == TY_UNIT));
  L1Instruction *inst = block->body;
  while (inst) {
    switch (inst->kind) {
    case INST_SET:
      fprintf(out, "    auto %s = ", inst->data.set.name);
      emit_c_expr(inst->data.set.val, out);
      fprintf(out, ";\n");
      break;
    case INST_STORE: {
      L1Type *store_ty = inst->data.store.store_ty;
      if (!store_ty) store_ty = infer_expr_type(inst->data.store.val);
      fprintf(out, "    *(");
      emit_c_type(store_ty, out);
      fprintf(out, "*)(");
      emit_c_expr(inst->data.store.dest, out);
      fprintf(out, ") = ");
      emit_c_expr(inst->data.store.val, out);
      fprintf(out, ";\n");
      break;
    }
    case INST_LOOP:
      fprintf(out, "    while (1) {\n");
      {
        L1Instruction *li = inst->data.loop_stmt.body;
        while (li) {
          if (li->kind == INST_BREAK) {
            fprintf(out, "    break;\n");
          } else if (li->kind == INST_SET) {
            fprintf(out, "        auto %s = ", li->data.set.name);
            emit_c_expr(li->data.set.val, out);
            fprintf(out, ";\n");
          }
          li = li->next;
        }
      }
      fprintf(out, "    }\n");
      break;
    case INST_IF:
      fprintf(out, "    if (");
      emit_c_expr(inst->data.if_stmt.condition, out);
      fprintf(out, ") {\n");
      {
        L1Instruction *li = inst->data.if_stmt.then_body;
        while (li) {
          emit_c_instruction(block, li, sub, out);
          li = li->next;
        }
      }
      if (inst->data.if_stmt.else_body) {
        fprintf(out, "    } else {\n");
        L1Instruction *li = inst->data.if_stmt.else_body;
        while (li) {
          emit_c_instruction(block, li, sub, out);
          li = li->next;
        }
      }
      fprintf(out, "    }\n");
      break;
    case INST_CALL:
      if (block->terminator && block->terminator->kind == TERM_RETURN &&
          block->terminator->data.ret_val == inst->data.call_inst.expr) {
        break;
      }
      fprintf(out, "    ");
      emit_c_expr(inst->data.call_inst.expr, out);
      fprintf(out, ";\n");
      break;
    case INST_RETURN:
      if (is_unit_main) {
        fprintf(out, "    return 0;\n");
      } else {
        fprintf(out, "    return ");
        emit_c_expr(inst->data.ret.val, out);
        fprintf(out, ";\n");
      }
      break;
    case INST_BREAK:
    default:
      break;
    }
    inst = inst->next;
  }
}

static void emit_c_block_terminator(L1Block *block, L1Subroutine *sub, FILE *out) {
  if (!block->terminator)
    return;
  int is_unit_main = (sub && strcmp(sub->name, "main") == 0 && sub->param_count == 0 &&
                      (!sub->ret_ty || sub->ret_ty->kind == TY_UNIT));
  switch (block->terminator->kind) {
  case TERM_RETURN:
    // void main: C requires int main(), so emit return 0 instead of return void
    if (is_unit_main) {
      fprintf(out, "    return 0;\n");
    } else if (block->terminator->data.ret_val) {
      fprintf(out, "    return ");
      emit_c_expr(block->terminator->data.ret_val, out);
      fprintf(out, ";\n");
    } else {
      fprintf(out, "    return;\n");
    }
    break;
  case TERM_BRANCH:
    fprintf(out, "    goto block_%d;\n", block->terminator->data.target_id);
    break;
  case TERM_COND_BRANCH:
    fprintf(out, "    if (");
    emit_c_expr(block->terminator->data.cond_branch.condition, out);
    fprintf(out, ") { goto block_%d; } else { goto block_%d; }\n",
            block->terminator->data.cond_branch.true_id,
            block->terminator->data.cond_branch.false_id);
    break;
  default:
    break;
  }
}

static void emit_c_subroutine(L1Subroutine *sub, FILE *out) {
  if (!sub->blocks)
    return;
  int is_main = (strcmp(sub->name, "main") == 0 && sub->param_count == 0);
  if (is_main) {
    fprintf(out, "int main(int argc, char **argv) {\n");
    fprintf(out, "    native_set_args(argc, argv);\n");
  } else {
    emit_c_type(sub->ret_ty, out);
    fprintf(out, " %s(", sub->name);
    for (uint32_t i = 0; i < sub->param_count; i++) {
      emit_c_type(sub->param_tys[i], out);
      fprintf(out, " arg%d%s", i, (i == sub->param_count - 1) ? "" : ", ");
    }
    fprintf(out, ") {\n");
  }

  L1Block *block = sub->blocks;
  while (block) {
    fprintf(out, "\nblock_%d:\n", block->id);
    emit_c_instructions(block, sub, out);
    emit_c_block_terminator(block, sub, out);
    block = block->next;
  }
  fprintf(out, "}\n\n");
}

// ============================================================================
