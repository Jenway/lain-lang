#include "lain/backend/c99.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lain/ir/infer.h"

typedef struct {
  const char *ir_name;
  char *c_name;
  L1Type ty;
} C99Binding;

typedef struct {
  const char *label;
  uint32_t id;
  uint32_t result_count;
  const L1Type *const *results;
  char *result_vars[L1_MAX_RESULTS];
  uint32_t param_count;
  const L1RegionParam *params;
  char **param_vars;
} C99LoopTarget;

typedef struct {
  uint32_t result_count;
  const L1Type *const *results;
  char *result_vars[L1_MAX_RESULTS];
} C99YieldTarget;

typedef struct {
  const L1Inst *inst;
  uint32_t id;
  uint64_t bytes;
  bool in_loop;
} C99AllocaInfo;

typedef struct {
  const L1Module *module;
  FILE *out;
  L1Diagnostic *diag;
  LainIrTypes types;

  /* 当前过程状态 */
  const L1Subroutine *cur_sub;
  uint32_t next_id;
  bool has_loop_alloca;

  C99Binding *binds;
  uint32_t bind_len;
  uint32_t bind_cap;

  C99LoopTarget *loops;
  uint32_t loop_len;
  uint32_t loop_cap;

  C99AllocaInfo *allocas;
  uint32_t alloca_len;
  uint32_t alloca_cap;
} C99Ctx;

static void set_diag(C99Ctx *ctx, int code, const L1Inst *inst,
                     const char *fmt, ...) {
  if (!ctx->diag) return;
  ctx->diag->code = code;
  ctx->diag->line = inst ? inst->line : 0;
  ctx->diag->column = inst ? inst->column : 0;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(ctx->diag->message, sizeof(ctx->diag->message), fmt, ap);
  va_end(ap);
}

static char *xstrdup(const char *s) {
  size_t n = strlen(s);
  char *p = (char *)malloc(n + 1);
  if (p) memcpy(p, s, n + 1);
  return p;
}

/* 将任意 IR 名字清洗为合法 C99 标识符片段。 */
static void emit_sanitized(FILE *out, const char *s) {
  if (!s || !*s) {
    fputs("anon", out);
    return;
  }
  for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
    if (isalnum(*p) || *p == '_') {
      fputc(*p, out);
    } else if (*p == '.') {
      fputs("_d_", out);
    } else if (*p == ':') {
      fputs("_c_", out);
    } else if (*p == '-') {
      fputs("_m_", out);
    } else {
      fprintf(out, "_%02x", (unsigned)*p);
    }
  }
}

static char *make_c_ident(const char *prefix, const char *name, uint32_t id) {
  size_t len = strlen(prefix) + (name ? strlen(name) * 3 : 4) + 24;
  char *buf = (char *)malloc(len);
  if (!buf) return NULL;
  size_t pos = 0;
  pos += (size_t)snprintf(buf + pos, len - pos, "%s", prefix);
  if (!name || !*name) {
    pos += (size_t)snprintf(buf + pos, len - pos, "anon");
  } else {
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p) {
      if (isalnum(*p) || *p == '_') {
        buf[pos++] = (char)*p;
      } else if (*p == '.') {
        pos += (size_t)snprintf(buf + pos, len - pos, "_d_");
      } else if (*p == ':') {
        pos += (size_t)snprintf(buf + pos, len - pos, "_c_");
      } else {
        pos += (size_t)snprintf(buf + pos, len - pos, "_%02x", (unsigned)*p);
      }
    }
  }
  snprintf(buf + pos, len - pos, "_%u", id);
  return buf;
}

static const char *c_type_str(const L1Type *ty) {
  if (!ty) return "void";
  switch (ty->kind) {
  case TY_BITS:
    if (ty->width <= 8) return "uint8_t";
    if (ty->width <= 16) return "uint16_t";
    if (ty->width <= 32) return "uint32_t";
    return "uint64_t";
  case TY_FLOATS:
    return (ty->width <= 32) ? "float" : "double";
  case TY_ADDR:
    return "void *";
  case TY_VEC:
    return "/*vec*/uint64_t";
  }
  return "uint64_t";
}

static void emit_type(FILE *out, const L1Type *ty) {
  fputs(c_type_str(ty), out);
}

static void emit_sub_ret_type(FILE *out, const L1Subroutine *sub) {
  if (sub->result_count == 0) {
    fputs("void", out);
  } else if (sub->result_count == 1) {
    emit_type(out, sub->results[0]);
  } else {
    fputs("struct { ", out);
    for (uint32_t i = 0; i < sub->result_count; ++i) {
      emit_type(out, sub->results[i]);
      fprintf(out, " r%u; ", i);
    }
    fputc('}', out);
  }
}

static const L1Subroutine *c99_find_subroutine(const L1Module *module,
                                               const char *name) {
  if (!module || !name) return NULL;
  for (uint32_t i = 0; i < module->subroutine_count; ++i) {
    const L1Subroutine *s = &module->subroutines[i];
    if (!(s->flags & SUBROUTINE_EXTERN) && strcmp(s->name, name) == 0) {
      return s;
    }
  }
  const L1Subroutine *ext = lainir_find_subroutine(module, name);
  if (ext && (ext->flags & SUBROUTINE_EXTERN) && ext->link_name) {
    for (uint32_t i = 0; i < module->subroutine_count; ++i) {
      const L1Subroutine *s = &module->subroutines[i];
      if (!(s->flags & SUBROUTINE_EXTERN) &&
          strcmp(s->name, ext->link_name) == 0) {
        return s;
      }
    }
  }
  return ext;
}

static void emit_sub_c_name(FILE *out, const L1Subroutine *sub) {
  if (sub->flags & SUBROUTINE_EXTERN) {
    fputs(sub->link_name ? sub->link_name : sub->name, out);
  } else {
    fputs("lain_fn_", out);
    emit_sanitized(out, sub->name);
  }
}

static void emit_data_c_name(FILE *out, const L1Data *d) {
  fputs("lain_data_", out);
  emit_sanitized(out, d->symbol);
}

static bool push_binding(C99Ctx *ctx, const char *ir_name, const char *c_name,
                         const L1Type *ty) {
  if (ctx->bind_len == ctx->bind_cap) {
    uint32_t ncap = ctx->bind_cap ? ctx->bind_cap * 2 : 32;
    C99Binding *nb =
        (C99Binding *)realloc(ctx->binds, ncap * sizeof(C99Binding));
    if (!nb) return false;
    ctx->binds = nb;
    ctx->bind_cap = ncap;
  }
  ctx->binds[ctx->bind_len].ir_name = ir_name;
  ctx->binds[ctx->bind_len].c_name = xstrdup(c_name);
  ctx->binds[ctx->bind_len].ty = *ty;
  ctx->bind_len++;
  return true;
}

static const C99Binding *find_binding(const C99Ctx *ctx, const char *ir_name) {
  for (uint32_t i = ctx->bind_len; i > 0; --i) {
    if (strcmp(ctx->binds[i - 1].ir_name, ir_name) == 0) {
      return &ctx->binds[i - 1];
    }
  }
  return NULL;
}

static void pop_bindings(C99Ctx *ctx, uint32_t to_len) {
  while (ctx->bind_len > to_len) {
    ctx->bind_len--;
    free(ctx->binds[ctx->bind_len].c_name);
  }
}

static const C99LoopTarget *find_loop(const C99Ctx *ctx, const char *label) {
  for (uint32_t i = ctx->loop_len; i > 0; --i) {
    const C99LoopTarget *lt = &ctx->loops[i - 1];
    if (!label && !lt->label) return lt;
    if (label && lt->label && strcmp(lt->label, label) == 0) return lt;
  }
  return NULL;
}

static void indent(FILE *out, int depth) {
  for (int i = 0; i < depth; ++i) fputs("  ", out);
}

static void emit_operand(C99Ctx *ctx, const L1Operand *op,
                         const L1Type *expected_ty) {
  FILE *out = ctx->out;
  switch (op->kind) {
  case OPERAND_INT: {
    uint32_t w = (expected_ty && expected_ty->kind == TY_BITS)
                     ? expected_ty->width
                     : 64u;
    uint64_t mask = (w >= 64u) ? ~UINT64_C(0) : ((UINT64_C(1) << w) - 1u);
    uint64_t val = op->bits & mask;
    if (expected_ty && expected_ty->kind == TY_ADDR) {
      fprintf(out, "(void *)(uintptr_t)UINT64_C(%llu)",
              (unsigned long long)op->bits);
    } else if (!expected_ty) {
      fprintf(out, "(uint64_t)UINT64_C(%llu)",
              (unsigned long long)val);
    } else {
      fprintf(out, "(%s)UINT64_C(%llu)", c_type_str(expected_ty),
              (unsigned long long)val);
    }
    break;
  }
  case OPERAND_FLOAT:
    if (expected_ty && expected_ty->width <= 32) {
      fprintf(out, "lain_bitcast_u32_f32((uint32_t)UINT64_C(%llu))",
              (unsigned long long)(op->bits & 0xFFFFFFFFu));
    } else {
      fprintf(out, "lain_bitcast_u64_f64(UINT64_C(%llu))",
              (unsigned long long)op->bits);
    }
    break;
  case OPERAND_VALUE:
    if (op->is_global) {
      const L1Data *d = lainir_find_data(ctx->module, op->name);
      if (d) {
        fputs("(void *)", out);
        emit_data_c_name(out, d);
        return;
      }
      const L1Subroutine *sub = c99_find_subroutine(ctx->module, op->name);
      if (sub) {
        fputs("(void *)&", out);
        emit_sub_c_name(out, sub);
        return;
      }
      fprintf(out, "/* unknown global @%s */NULL", op->name);
    } else {
      const C99Binding *b = find_binding(ctx, op->name);
      if (b) {
        fputs(b->c_name, out);
      } else {
        const L1Data *d = lainir_find_data(ctx->module, op->name);
        if (d) {
          fputs("(void *)", out);
          emit_data_c_name(out, d);
          return;
        }
        const L1Subroutine *sub = c99_find_subroutine(ctx->module, op->name);
        if (sub) {
          fputs("(void *)&", out);
          emit_sub_c_name(out, sub);
          return;
        }
        fprintf(out, "/* undef %%%s */0", op->name);
      }
    }
    break;
  }
}

static const L1Type *operand_type(const C99Ctx *ctx, const L1Operand *op,
                                  const L1Type *fallback) {
  if (op->kind == OPERAND_VALUE && !op->is_global) {
    const C99Binding *b = find_binding(ctx, op->name);
    if (b) return &b->ty;
  }
  if (op->kind == OPERAND_VALUE && op->is_global) return &ctx->types.addr;
  return fallback;
}

static uint32_t type_bits_width(const L1Type *ty) {
  if (!ty) return 64u;
  if (ty->kind == TY_BITS) return ty->width;
  if (ty->kind == TY_ADDR) return 64u;
  return ty->width;
}

/* 预扫描区域内的 #alloca：循环外的全部提升到过程顶部。 */
static bool prescan_allocas(C99Ctx *ctx, const L1Region *r, bool in_loop) {
  if (!r) return true;
  for (uint32_t i = 0; i < r->inst_count; ++i) {
    const L1Inst *inst = &r->insts[i];
    if (inst->kind == INST_ALLOCA) {
      if (ctx->alloca_len == ctx->alloca_cap) {
        uint32_t ncap = ctx->alloca_cap ? ctx->alloca_cap * 2 : 16;
        C99AllocaInfo *na = (C99AllocaInfo *)realloc(
            ctx->allocas, ncap * sizeof(C99AllocaInfo));
        if (!na) return false;
        ctx->allocas = na;
        ctx->alloca_cap = ncap;
      }
      uint64_t elem_bytes = (lainir_type_width(inst->ty) + 7u) / 8u;
      if (elem_bytes == 0) elem_bytes = 1;
      uint64_t count =
          (inst->operand_count > 0 && inst->operands[0].kind == OPERAND_INT)
              ? inst->operands[0].bits
              : 1u;
      uint64_t total = elem_bytes * count;
      if (total == 0) total = 1;
      ctx->allocas[ctx->alloca_len].inst = inst;
      ctx->allocas[ctx->alloca_len].id = ctx->alloca_len;
      ctx->allocas[ctx->alloca_len].bytes = total;
      ctx->allocas[ctx->alloca_len].in_loop = in_loop;
      if (in_loop) ctx->has_loop_alloca = true;
      ctx->alloca_len++;
    } else if (inst->kind == INST_IF) {
      if (!prescan_allocas(ctx, inst->body, in_loop)) return false;
      if (!prescan_allocas(ctx, inst->else_body, in_loop)) return false;
    } else if (inst->kind == INST_LOOP) {
      if (!prescan_allocas(ctx, inst->body, true)) return false;
    } else if (inst->kind == INST_SWITCH) {
      for (uint32_t c = 0; c < inst->case_count; ++c) {
        if (!prescan_allocas(ctx, inst->cases[c].body, in_loop)) return false;
      }
      if (!prescan_allocas(ctx, inst->default_case, in_loop)) return false;
    }
  }
  return true;
}

static int find_alloca_id(const C99Ctx *ctx, const L1Inst *inst) {
  for (uint32_t i = 0; i < ctx->alloca_len; ++i) {
    if (ctx->allocas[i].inst == inst) return (int)i;
  }
  return -1;
}

static void emit_free_loop_allocas(C99Ctx *ctx, int depth) {
  if (!ctx->has_loop_alloca) return;
  indent(ctx->out, depth);
  fputs("lain_alloca_free(&_alloca_head);\n", ctx->out);
}

static bool emit_region(C99Ctx *ctx, const L1Region *r, int depth,
                        const C99YieldTarget *yield_target);

static bool emit_inst(C99Ctx *ctx, const L1Inst *inst, int depth,
                      const C99YieldTarget *yield_target) {
  FILE *out = ctx->out;
  char *res_names[L1_MAX_RESULTS] = {NULL, NULL};
  const L1Type *res_types[L1_MAX_RESULTS] = {NULL, NULL};

  for (uint32_t r = 0; r < inst->result_count; ++r) {
    res_types[r] = lainir_inst_result_type(&ctx->types, inst, r);
    if (!res_types[r]) {
      set_diag(ctx, 4001, inst, "c99: cannot infer result type for inst");
      return false;
    }
    res_names[r] = make_c_ident("v_", inst->results[r], ctx->next_id++);
    if (!res_names[r]) return false;
  }

  switch (inst->kind) {
  /* ---------------- 整数算术（无符号包裹 + 显式 trap） ---------------- */
  case INST_ADD:
  case INST_SUB:
  case INST_MUL:
  case INST_AND:
  case INST_OR:
  case INST_XOR: {
    uint32_t w = inst->ty->width;
    const char *op = "+";
    if (inst->kind == INST_SUB) op = "-";
    else if (inst->kind == INST_MUL) op = "*";
    else if (inst->kind == INST_AND) op = "&";
    else if (inst->kind == INST_OR) op = "|";
    else if (inst->kind == INST_XOR) op = "^";
    indent(out, depth);
    fprintf(out, "%s %s = (%s)((((uint64_t)", c_type_str(res_types[0]),
            res_names[0], c_type_str(res_types[0]));
    emit_operand(ctx, &inst->operands[0], inst->ty);
    fprintf(out, ") %s ((uint64_t)", op);
    emit_operand(ctx, &inst->operands[1], inst->ty);
    fprintf(out, ")) & lain_mask(%u));\n", w);
    break;
  }

  case INST_SDIV:
  case INST_SREM:
  case INST_UDIV:
  case INST_UREM:
  case INST_SHL:
  case INST_LSHR:
  case INST_ASHR: {
    uint32_t w = inst->ty->width;
    const char *fn = "lain_sdiv";
    if (inst->kind == INST_SREM) fn = "lain_srem";
    else if (inst->kind == INST_UDIV) fn = "lain_udiv";
    else if (inst->kind == INST_UREM) fn = "lain_urem";
    else if (inst->kind == INST_SHL) fn = "lain_shl";
    else if (inst->kind == INST_LSHR) fn = "lain_lshr";
    else if (inst->kind == INST_ASHR) fn = "lain_ashr";
    indent(out, depth);
    fprintf(out, "%s %s = (%s)%s((uint64_t)", c_type_str(res_types[0]),
            res_names[0], c_type_str(res_types[0]), fn);
    emit_operand(ctx, &inst->operands[0], inst->ty);
    fputs(", (uint64_t)", out);
    emit_operand(ctx, &inst->operands[1], inst->ty);
    fprintf(out, ", %u);\n", w);
    break;
  }

  /* ---------------- 浮点算术 ---------------- */
  case INST_FNEG:
    indent(out, depth);
    fprintf(out, "%s %s = -(", c_type_str(res_types[0]), res_names[0]);
    emit_operand(ctx, &inst->operands[0], inst->ty);
    fputs(");\n", out);
    break;

  case INST_FADD:
  case INST_FSUB:
  case INST_FMUL:
  case INST_FDIV: {
    const char *op = "+";
    if (inst->kind == INST_FSUB) op = "-";
    else if (inst->kind == INST_FMUL) op = "*";
    else if (inst->kind == INST_FDIV) op = "/";
    indent(out, depth);
    fprintf(out, "%s %s = (", c_type_str(res_types[0]), res_names[0]);
    emit_operand(ctx, &inst->operands[0], inst->ty);
    fprintf(out, ") %s (", op);
    emit_operand(ctx, &inst->operands[1], inst->ty);
    fputs(");\n", out);
    break;
  }

  /* ---------------- 整数比较 ---------------- */
  case INST_EQ:
  case INST_NE:
  case INST_ULT:
  case INST_ULE:
  case INST_UGT:
  case INST_UGE: {
    const L1Type *oty = operand_type(ctx, &inst->operands[0], inst->ty);
    if (!oty) oty = operand_type(ctx, &inst->operands[1], inst->ty);
    uint32_t w = type_bits_width(oty);
    const char *op = "==";
    if (inst->kind == INST_NE) op = "!=";
    else if (inst->kind == INST_ULT) op = "<";
    else if (inst->kind == INST_ULE) op = "<=";
    else if (inst->kind == INST_UGT) op = ">";
    else if (inst->kind == INST_UGE) op = ">=";
    indent(out, depth);
    if (oty && oty->kind == TY_ADDR) {
      fprintf(out, "uint8_t %s = (uint8_t)((uintptr_t)(", res_names[0]);
      emit_operand(ctx, &inst->operands[0], oty);
      fprintf(out, ") %s (uintptr_t)(", op);
      emit_operand(ctx, &inst->operands[1], oty);
      fputs("));\n", out);
    } else {
      fprintf(out, "uint8_t %s = (uint8_t)((((uint64_t)", res_names[0]);
      emit_operand(ctx, &inst->operands[0], oty);
      fprintf(out, " & lain_mask(%u)) %s ((uint64_t)", w, op);
      emit_operand(ctx, &inst->operands[1], oty);
      fprintf(out, " & lain_mask(%u))));\n", w);
    }
    break;
  }

  case INST_SLT:
  case INST_SLE:
  case INST_SGT:
  case INST_SGE: {
    uint32_t w = inst->ty->width;
    const char *op = "<";
    if (inst->kind == INST_SLE) op = "<=";
    else if (inst->kind == INST_SGT) op = ">";
    else if (inst->kind == INST_SGE) op = ">=";
    indent(out, depth);
    fprintf(out, "uint8_t %s = (uint8_t)(lain_sext((uint64_t)", res_names[0]);
    emit_operand(ctx, &inst->operands[0], inst->ty);
    fprintf(out, ", %u) %s lain_sext((uint64_t)", w, op);
    emit_operand(ctx, &inst->operands[1], inst->ty);
    fprintf(out, ", %u));\n", w);
    break;
  }

  /* ---------------- 浮点比较 ---------------- */
  case INST_FOEQ:
  case INST_FONE:
  case INST_FOLT:
  case INST_FOLE:
  case INST_FOGT:
  case INST_FOGE:
  case INST_FUEQ:
  case INST_FUNE:
  case INST_FULT:
  case INST_FULE:
  case INST_FUGT:
  case INST_FUGE: {
    const char *cmp = "==";
    bool ordered = (inst->kind <= INST_FOGE);
    switch (inst->kind) {
    case INST_FOEQ: case INST_FUEQ: cmp = "=="; break;
    case INST_FONE: case INST_FUNE: cmp = "!="; break;
    case INST_FOLT: case INST_FULT: cmp = "<"; break;
    case INST_FOLE: case INST_FULE: cmp = "<="; break;
    case INST_FOGT: case INST_FUGT: cmp = ">"; break;
    case INST_FOGE: case INST_FUGE: cmp = ">="; break;
    default: break;
    }
    indent(out, depth);
    fprintf(out, "uint8_t %s = (uint8_t)(", res_names[0]);
    const char *fn = ordered ? "lain_ford" : "lain_funord";
    (void)fn;
    if (ordered) {
      fputs("((", out);
      emit_operand(ctx, &inst->operands[0], inst->ty);
      fputs(" == ", out);
      emit_operand(ctx, &inst->operands[0], inst->ty);
      fputs(") && (", out);
      emit_operand(ctx, &inst->operands[1], inst->ty);
      fputs(" == ", out);
      emit_operand(ctx, &inst->operands[1], inst->ty);
      fputs(") && (", out);
      emit_operand(ctx, &inst->operands[0], inst->ty);
      fprintf(out, " %s ", cmp);
      emit_operand(ctx, &inst->operands[1], inst->ty);
      fputs("))", out);
    } else {
      fputs("(!((", out);
      emit_operand(ctx, &inst->operands[0], inst->ty);
      fputs(" == ", out);
      emit_operand(ctx, &inst->operands[0], inst->ty);
      fputs(") && (", out);
      emit_operand(ctx, &inst->operands[1], inst->ty);
      fputs(" == ", out);
      emit_operand(ctx, &inst->operands[1], inst->ty);
      fputs(")) || (", out);
      emit_operand(ctx, &inst->operands[0], inst->ty);
      fprintf(out, " %s ", cmp);
      emit_operand(ctx, &inst->operands[1], inst->ty);
      fputs("))", out);
    }
    fputs(");\n", out);
    break;
  }

  /* ---------------- 转换 ---------------- */
  case INST_ZEXT: {
    const L1Type *oty = operand_type(ctx, &inst->operands[0], inst->ty);
    uint32_t sw = type_bits_width(oty);
    indent(out, depth);
    fprintf(out, "%s %s = (%s)(((uint64_t)", c_type_str(res_types[0]),
            res_names[0], c_type_str(res_types[0]));
    emit_operand(ctx, &inst->operands[0], oty);
    fprintf(out, ") & lain_mask(%u));\n", sw);
    break;
  }

  case INST_SEXT: {
    const L1Type *oty = operand_type(ctx, &inst->operands[0], inst->ty);
    uint32_t sw = type_bits_width(oty);
    uint32_t dw = inst->ty->width;
    indent(out, depth);
    fprintf(out, "%s %s = (%s)((uint64_t)lain_sext((uint64_t)",
            c_type_str(res_types[0]), res_names[0], c_type_str(res_types[0]));
    emit_operand(ctx, &inst->operands[0], oty);
    fprintf(out, ", %u) & lain_mask(%u));\n", sw, dw);
    break;
  }

  case INST_TRUNC: {
    uint32_t dw = inst->ty->width;
    indent(out, depth);
    fprintf(out, "%s %s = (%s)(((uint64_t)", c_type_str(res_types[0]),
            res_names[0], c_type_str(res_types[0]));
    emit_operand(ctx, &inst->operands[0], NULL);
    fprintf(out, ") & lain_mask(%u));\n", dw);
    break;
  }

  case INST_BITCAST: {
    const L1Type *oty = operand_type(ctx, &inst->operands[0], inst->ty);
    indent(out, depth);
    if (inst->ty->kind == TY_ADDR) {
      fprintf(out, "void *%s = (void *)(uintptr_t)", res_names[0]);
      emit_operand(ctx, &inst->operands[0], oty);
      fputs(";\n", out);
    } else if (oty && oty->kind == TY_ADDR) {
      fprintf(out, "%s %s = (%s)(uintptr_t)", c_type_str(res_types[0]),
              res_names[0], c_type_str(res_types[0]));
      emit_operand(ctx, &inst->operands[0], oty);
      fputs(";\n", out);
    } else if (inst->ty->kind == TY_FLOATS && inst->ty->width <= 32) {
      fprintf(out, "float %s = lain_bitcast_u32_f32((uint32_t)", res_names[0]);
      emit_operand(ctx, &inst->operands[0], oty);
      fputs(");\n", out);
    } else if (inst->ty->kind == TY_FLOATS) {
      fprintf(out, "double %s = lain_bitcast_u64_f64((uint64_t)", res_names[0]);
      emit_operand(ctx, &inst->operands[0], oty);
      fputs(");\n", out);
    } else if (oty && oty->kind == TY_FLOATS && oty->width <= 32) {
      fprintf(out, "%s %s = (%s)lain_bitcast_f32_u32(",
              c_type_str(res_types[0]), res_names[0], c_type_str(res_types[0]));
      emit_operand(ctx, &inst->operands[0], oty);
      fputs(");\n", out);
    } else if (oty && oty->kind == TY_FLOATS) {
      fprintf(out, "%s %s = (%s)lain_bitcast_f64_u64(",
              c_type_str(res_types[0]), res_names[0], c_type_str(res_types[0]));
      emit_operand(ctx, &inst->operands[0], oty);
      fputs(");\n", out);
    } else {
      fprintf(out, "%s %s = (%s)(((uint64_t)", c_type_str(res_types[0]),
              res_names[0], c_type_str(res_types[0]));
      emit_operand(ctx, &inst->operands[0], oty);
      fprintf(out, ") & lain_mask(%u));\n", inst->ty->width);
    }
    break;
  }

  case INST_FPEXT:
  case INST_FPTRUNC:
    indent(out, depth);
    fprintf(out, "%s %s = (%s)(", c_type_str(res_types[0]), res_names[0],
            c_type_str(res_types[0]));
    emit_operand(ctx, &inst->operands[0], NULL);
    fputs(");\n", out);
    break;

  case INST_FPTOSI:
    indent(out, depth);
    fprintf(out, "%s %s = (%s)((uint64_t)(int64_t)(", c_type_str(res_types[0]),
            res_names[0], c_type_str(res_types[0]));
    emit_operand(ctx, &inst->operands[0], NULL);
    fprintf(out, ") & lain_mask(%u));\n", inst->ty->width);
    break;

  case INST_FPTOUI:
    indent(out, depth);
    fprintf(out, "%s %s = (%s)((uint64_t)(", c_type_str(res_types[0]),
            res_names[0], c_type_str(res_types[0]));
    emit_operand(ctx, &inst->operands[0], NULL);
    fprintf(out, ") & lain_mask(%u));\n", inst->ty->width);
    break;

  case INST_SITOFP: {
    const L1Type *oty = operand_type(ctx, &inst->operands[0], NULL);
    uint32_t sw = type_bits_width(oty);
    indent(out, depth);
    fprintf(out, "%s %s = (%s)lain_sext((uint64_t)", c_type_str(res_types[0]),
            res_names[0], c_type_str(res_types[0]));
    emit_operand(ctx, &inst->operands[0], oty);
    fprintf(out, ", %u);\n", sw);
    break;
  }

  case INST_UITOFP: {
    const L1Type *oty = operand_type(ctx, &inst->operands[0], NULL);
    uint32_t sw = type_bits_width(oty);
    indent(out, depth);
    fprintf(out, "%s %s = (%s)(((uint64_t)", c_type_str(res_types[0]),
            res_names[0], c_type_str(res_types[0]));
    emit_operand(ctx, &inst->operands[0], oty);
    fprintf(out, ") & lain_mask(%u));\n", sw);
    break;
  }

  /* ---------------- 内存与取址 ---------------- */
  case INST_LEA:
    indent(out, depth);
    fprintf(out, "void *%s = (void *)(uintptr_t)((uint64_t)(uintptr_t)(",
            res_names[0]);
    emit_operand(ctx, &inst->operands[0], &ctx->types.addr);
    fputs(") + ((uint64_t)", out);
    emit_operand(ctx, &inst->operands[1], NULL);
    fputs(") * ((uint64_t)", out);
    emit_operand(ctx, &inst->operands[2], NULL);
    fputs(") + ((uint64_t)", out);
    emit_operand(ctx, &inst->operands[3], NULL);
    fputs("));\n", out);
    break;

  case INST_ALLOCA: {
    int aid = find_alloca_id(ctx, inst);
    indent(out, depth);
    if (aid >= 0 && !ctx->allocas[aid].in_loop) {
      fprintf(out, "void *%s = (void *)_alloca_%d;\n", res_names[0], aid);
    } else {
      uint64_t elem_bytes = (lainir_type_width(inst->ty) + 7u) / 8u;
      if (elem_bytes == 0) elem_bytes = 1;
      fprintf(out,
              "void *%s = lain_alloca_push(&_alloca_head, "
              "(size_t)%llu * (size_t)",
              res_names[0], (unsigned long long)elem_bytes);
      emit_operand(ctx, &inst->operands[0], NULL);
      fputs(");\n", out);
    }
    break;
  }

  case INST_LOAD:
    indent(out, depth);
    if (inst->ty->kind == TY_ADDR) {
      fprintf(out, "void *%s = lain_load_addr(", res_names[0]);
      emit_operand(ctx, &inst->operands[0], &ctx->types.addr);
      fputs(");\n", out);
    } else if (inst->ty->kind == TY_FLOATS && inst->ty->width <= 32) {
      fprintf(out, "float %s = lain_load_f32(", res_names[0]);
      emit_operand(ctx, &inst->operands[0], &ctx->types.addr);
      fputs(");\n", out);
    } else if (inst->ty->kind == TY_FLOATS) {
      fprintf(out, "double %s = lain_load_f64(", res_names[0]);
      emit_operand(ctx, &inst->operands[0], &ctx->types.addr);
      fputs(");\n", out);
    } else {
      fprintf(out, "%s %s = (%s)lain_load_bits(", c_type_str(res_types[0]),
              res_names[0], c_type_str(res_types[0]));
      emit_operand(ctx, &inst->operands[0], &ctx->types.addr);
      fprintf(out, ", %u);\n", inst->ty->width);
    }
    break;

  case INST_STORE:
    indent(out, depth);
    if (inst->ty->kind == TY_ADDR) {
      fputs("lain_store_addr(", out);
      emit_operand(ctx, &inst->operands[1], &ctx->types.addr);
      fputs(", ", out);
      emit_operand(ctx, &inst->operands[0], inst->ty);
      fputs(");\n", out);
    } else if (inst->ty->kind == TY_FLOATS && inst->ty->width <= 32) {
      fputs("lain_store_f32(", out);
      emit_operand(ctx, &inst->operands[1], &ctx->types.addr);
      fputs(", ", out);
      emit_operand(ctx, &inst->operands[0], inst->ty);
      fputs(");\n", out);
    } else if (inst->ty->kind == TY_FLOATS) {
      fputs("lain_store_f64(", out);
      emit_operand(ctx, &inst->operands[1], &ctx->types.addr);
      fputs(", ", out);
      emit_operand(ctx, &inst->operands[0], inst->ty);
      fputs(");\n", out);
    } else {
      fputs("lain_store_bits(", out);
      emit_operand(ctx, &inst->operands[1], &ctx->types.addr);
      fputs(", (uint64_t)", out);
      emit_operand(ctx, &inst->operands[0], inst->ty);
      fprintf(out, ", %u);\n", inst->ty->width);
    }
    break;

  /* ---------------- 调用 ---------------- */
  case INST_CALL: {
    const L1Subroutine *callee =
        inst->symbol ? c99_find_subroutine(ctx->module, inst->symbol) : NULL;
    char *tmp_ret = NULL;
    indent(out, depth);
    if (inst->result_count == 1) {
      fprintf(out, "%s %s = ", c_type_str(res_types[0]), res_names[0]);
    } else if (inst->result_count > 1) {
      tmp_ret = make_c_ident("_callret_", "", ctx->next_id++);
      fputs("struct { ", out);
      for (uint32_t r = 0; r < inst->result_count; ++r) {
        emit_type(out, res_types[r]);
        fprintf(out, " r%u; ", r);
      }
      fprintf(out, "} %s = ", tmp_ret);
    }

    if (callee) {
      emit_sub_c_name(out, callee);
      fputc('(', out);
      for (uint32_t a = 0; a < inst->operand_count; ++a) {
        if (a > 0) fputs(", ", out);
        const L1Type *pty =
            (a < callee->param_count) ? callee->params[a].ty : NULL;
        emit_operand(ctx, &inst->operands[a], pty);
      }
      fputs(");\n", out);
    } else {
      /* 间接调用：operands[0] 是函数指针，operands[1..] 是实参。 */
      fputs("((", out);
      if (inst->result_count == 0) {
        fputs("void", out);
      } else if (inst->result_count == 1) {
        emit_type(out, res_types[0]);
      } else {
        fputs("struct { ", out);
        for (uint32_t r = 0; r < inst->result_count; ++r) {
          emit_type(out, res_types[r]);
          fprintf(out, " r%u; ", r);
        }
        fputc('}', out);
      }
      fputs(" (*)(", out);
      if (inst->call_param_count == 0) {
        fputs("void", out);
      } else {
        for (uint32_t p = 0; p < inst->call_param_count; ++p) {
          if (p > 0) fputs(", ", out);
          emit_type(out, inst->call_params[p]);
        }
      }
      fputs("))(", out);
      emit_operand(ctx, &inst->operands[0], &ctx->types.addr);
      fputs("))(", out);
      for (uint32_t a = 1; a < inst->operand_count; ++a) {
        if (a > 1) fputs(", ", out);
        const L1Type *pty =
            ((a - 1) < inst->call_param_count) ? inst->call_params[a - 1] : NULL;
        emit_operand(ctx, &inst->operands[a], pty);
      }
      fputs(");\n", out);
    }

    if (tmp_ret) {
      for (uint32_t r = 0; r < inst->result_count; ++r) {
        indent(out, depth);
        fprintf(out, "%s %s = %s.r%u;\n", c_type_str(res_types[r]),
                res_names[r], tmp_ret, r);
      }
      free(tmp_ret);
    }
    break;
  }

  /* ---------------- 结构控制流 ---------------- */
  case INST_IF: {
    C99YieldTarget yt = {0};
    yt.result_count = inst->result_count;
    yt.results = inst->body ? inst->body->results : NULL;
    for (uint32_t r = 0; r < inst->result_count; ++r) {
      indent(out, depth);
      fprintf(out, "%s %s = 0;\n", c_type_str(res_types[r]), res_names[r]);
      yt.result_vars[r] = res_names[r];
    }
    indent(out, depth);
    fputs("if (", out);
    emit_operand(ctx, &inst->operands[0], &ctx->types.bits1);
    fputs(" & 1u) {\n", out);
    if (!emit_region(ctx, inst->body, depth + 1, &yt)) {
      for (uint32_t r = 0; r < inst->result_count; ++r) free(res_names[r]);
      return false;
    }
    if (inst->else_body) {
      indent(out, depth);
      fputs("} else {\n", out);
      if (!emit_region(ctx, inst->else_body, depth + 1, &yt)) {
        for (uint32_t r = 0; r < inst->result_count; ++r) free(res_names[r]);
        return false;
      }
    }
    indent(out, depth);
    fputs("}\n", out);
    break;
  }

  case INST_LOOP: {
    const L1Region *body = inst->body;
    uint32_t lid = ctx->next_id++;
    for (uint32_t r = 0; r < inst->result_count; ++r) {
      indent(out, depth);
      fprintf(out, "%s %s = 0;\n", c_type_str(res_types[r]), res_names[r]);
    }

    if (ctx->loop_len == ctx->loop_cap) {
      uint32_t ncap = ctx->loop_cap ? ctx->loop_cap * 2 : 8;
      C99LoopTarget *nl = (C99LoopTarget *)realloc(
          ctx->loops, ncap * sizeof(C99LoopTarget));
      if (!nl) return false;
      ctx->loops = nl;
      ctx->loop_cap = ncap;
    }

    C99LoopTarget *lt = &ctx->loops[ctx->loop_len++];
    memset(lt, 0, sizeof(*lt));
    lt->label = inst->label;
    lt->id = lid;
    lt->result_count = inst->result_count;
    lt->results = body ? body->results : NULL;
    for (uint32_t r = 0; r < inst->result_count; ++r) {
      lt->result_vars[r] = res_names[r];
    }
    lt->param_count = body ? body->param_count : 0;
    lt->params = body ? body->params : NULL;
    lt->param_vars = lt->param_count
                         ? (char **)calloc(lt->param_count, sizeof(char *))
                         : NULL;

    uint32_t saved_binds = ctx->bind_len;
    for (uint32_t p = 0; p < lt->param_count; ++p) {
      const L1RegionParam *rp = &body->params[p];
      lt->param_vars[p] = make_c_ident("lp_", rp->param.name, ctx->next_id++);
      indent(out, depth);
      fprintf(out, "%s %s = ", c_type_str(rp->param.ty), lt->param_vars[p]);
      emit_operand(ctx, &rp->init, rp->param.ty);
      fputs(";\n", out);
      push_binding(ctx, rp->param.name, lt->param_vars[p], rp->param.ty);
    }

    indent(out, depth);
    fputs("for (;;) {\n", out);
    bool ok = emit_region(ctx, body, depth + 1, NULL);
    indent(out, depth + 1);
    fprintf(out, "lbl_cont_%u:;\n", lid);
    indent(out, depth);
    fputs("}\n", out);
    indent(out, depth);
    fprintf(out, "lbl_end_%u:;\n", lid);

    pop_bindings(ctx, saved_binds);
    for (uint32_t p = 0; p < lt->param_count; ++p) free(lt->param_vars[p]);
    free(lt->param_vars);
    ctx->loop_len--;
    if (!ok) {
      for (uint32_t r = 0; r < inst->result_count; ++r) free(res_names[r]);
      return false;
    }
    break;
  }

  case INST_SWITCH: {
    C99YieldTarget yt = {0};
    yt.result_count = inst->result_count;
    yt.results = inst->default_case ? inst->default_case->results : NULL;
    for (uint32_t r = 0; r < inst->result_count; ++r) {
      indent(out, depth);
      fprintf(out, "%s %s = 0;\n", c_type_str(res_types[r]), res_names[r]);
      yt.result_vars[r] = res_names[r];
    }
    uint32_t w = inst->ty ? inst->ty->width : 64u;
    uint64_t mask = (w >= 64u) ? ~UINT64_C(0) : ((UINT64_C(1) << w) - 1u);
    indent(out, depth);
    fputs("switch (((uint64_t)", out);
    emit_operand(ctx, &inst->operands[0], inst->ty);
    fprintf(out, ") & lain_mask(%u)) {\n", w);
    for (uint32_t c = 0; c < inst->case_count; ++c) {
      indent(out, depth);
      fprintf(out, "case UINT64_C(%llu): {\n",
              (unsigned long long)(inst->cases[c].value & mask));
      if (!emit_region(ctx, inst->cases[c].body, depth + 1, &yt)) {
        for (uint32_t r = 0; r < inst->result_count; ++r) free(res_names[r]);
        return false;
      }
      indent(out, depth + 1);
      fputs("break;\n", out);
      indent(out, depth);
      fputs("}\n", out);
    }
    indent(out, depth);
    fputs("default: {\n", out);
    if (inst->default_case &&
        !emit_region(ctx, inst->default_case, depth + 1, &yt)) {
      for (uint32_t r = 0; r < inst->result_count; ++r) free(res_names[r]);
      return false;
    }
    indent(out, depth + 1);
    fputs("break;\n", out);
    indent(out, depth);
    fputs("}\n", out);
    indent(out, depth);
    fputs("}\n", out);
    break;
  }

  /* ---------------- 终结子 ---------------- */
  case INST_YIELD:
    if (yield_target) {
      for (uint32_t i = 0;
           i < inst->operand_count && i < yield_target->result_count; ++i) {
        indent(out, depth);
        fprintf(out, "%s = ", yield_target->result_vars[i]);
        emit_operand(ctx, &inst->operands[i], yield_target->results[i]);
        fputs(";\n", out);
      }
    }
    break;

  case INST_BREAK: {
    const C99LoopTarget *lt = find_loop(ctx, inst->label);
    if (!lt) {
      set_diag(ctx, 4002, inst, "c99: unknown loop label in #break");
      return false;
    }
    for (uint32_t i = 0; i < inst->operand_count && i < lt->result_count;
         ++i) {
      indent(out, depth);
      fprintf(out, "%s = ", lt->result_vars[i]);
      emit_operand(ctx, &inst->operands[i], lt->results[i]);
      fputs(";\n", out);
    }
    indent(out, depth);
    fprintf(out, "goto lbl_end_%u;\n", lt->id);
    break;
  }

  case INST_CONTINUE: {
    const C99LoopTarget *lt = find_loop(ctx, inst->label);
    if (!lt) {
      set_diag(ctx, 4003, inst, "c99: unknown loop label in #continue");
      return false;
    }
    indent(out, depth);
    fputs("{\n", out);
    for (uint32_t i = 0; i < inst->operand_count && i < lt->param_count; ++i) {
      indent(out, depth + 1);
      fprintf(out, "%s _next_%u = ", c_type_str(lt->params[i].param.ty), i);
      emit_operand(ctx, &inst->operands[i], lt->params[i].param.ty);
      fputs(";\n", out);
    }
    for (uint32_t i = 0; i < inst->operand_count && i < lt->param_count; ++i) {
      indent(out, depth + 1);
      fprintf(out, "%s = _next_%u;\n", lt->param_vars[i], i);
    }
    indent(out, depth + 1);
    fprintf(out, "goto lbl_cont_%u;\n", lt->id);
    indent(out, depth);
    fputs("}\n", out);
    break;
  }

  case INST_RETURN:
    if (ctx->cur_sub->result_count == 0) {
      emit_free_loop_allocas(ctx, depth);
      indent(out, depth);
      fputs("return;\n", out);
    } else if (ctx->cur_sub->result_count == 1) {
      if (ctx->has_loop_alloca) {
        indent(out, depth);
        fprintf(out, "%s _ret_val = ", c_type_str(ctx->cur_sub->results[0]));
        emit_operand(ctx, &inst->operands[0], ctx->cur_sub->results[0]);
        fputs(";\n", out);
        emit_free_loop_allocas(ctx, depth);
        indent(out, depth);
        fputs("return _ret_val;\n", out);
      } else {
        indent(out, depth);
        fputs("return ", out);
        emit_operand(ctx, &inst->operands[0], ctx->cur_sub->results[0]);
        fputs(";\n", out);
      }
    } else {
      indent(out, depth);
      emit_sub_ret_type(out, ctx->cur_sub);
      fputs(" _ret_pair = { ", out);
      for (uint32_t r = 0; r < ctx->cur_sub->result_count; ++r) {
        if (r > 0) fputs(", ", out);
        emit_operand(ctx, &inst->operands[r], ctx->cur_sub->results[r]);
      }
      fputs(" };\n", out);
      emit_free_loop_allocas(ctx, depth);
      indent(out, depth);
      fputs("return _ret_pair;\n", out);
    }
    break;

  case INST_UNREACHABLE:
    emit_free_loop_allocas(ctx, depth);
    indent(out, depth);
    fputs("lain_trap(\"unreachable\");\n", out);
    break;

  default:
    set_diag(ctx, 4004, inst, "c99: unsupported instruction kind %d",
             (int)inst->kind);
    for (uint32_t r = 0; r < inst->result_count; ++r) free(res_names[r]);
    return false;
  }

  for (uint32_t r = 0; r < inst->result_count; ++r) {
    push_binding(ctx, inst->results[r], res_names[r], res_types[r]);
    free(res_names[r]);
  }
  return true;
}

static bool emit_region(C99Ctx *ctx, const L1Region *r, int depth,
                        const C99YieldTarget *yield_target) {
  if (!r) return true;
  uint32_t saved_binds = ctx->bind_len;
  for (uint32_t i = 0; i < r->inst_count; ++i) {
    if (!emit_inst(ctx, &r->insts[i], depth, yield_target)) {
      pop_bindings(ctx, saved_binds);
      return false;
    }
  }
  pop_bindings(ctx, saved_binds);
  return true;
}

static void emit_preamble(FILE *out) {
  fputs(
      "/* Generated by lainir-c (LAINIR -> C99) */\n"
      "#include <stdint.h>\n"
      "#include <stdbool.h>\n"
      "#include <stddef.h>\n"
      "#include <stdio.h>\n"
      "#include <stdlib.h>\n"
      "#include <string.h>\n\n"
      "static void lain_trap(const char *msg) {\n"
      "  fprintf(stderr, \"lain trap: %s\\n\", msg);\n"
      "  abort();\n"
      "}\n\n"
      "static inline uint64_t lain_mask(uint32_t w) {\n"
      "  return (w >= 64u) ? ~UINT64_C(0) : ((UINT64_C(1) << w) - 1u);\n"
      "}\n\n"
      "static inline int64_t lain_sext(uint64_t v, uint32_t w) {\n"
      "  uint64_t m = lain_mask(w);\n"
      "  v &= m;\n"
      "  if (w > 0u && w < 64u && (v & (UINT64_C(1) << (w - 1u)))) v |= ~m;\n"
      "  return (int64_t)v;\n"
      "}\n\n"
      "static inline int64_t lain_smin(uint32_t w) {\n"
      "  if (w == 0u) return 0;\n"
      "  if (w >= 64u) return INT64_MIN;\n"
      "  return -(int64_t)(UINT64_C(1) << (w - 1u));\n"
      "}\n\n"
      "static inline uint64_t lain_sdiv(uint64_t a, uint64_t b, uint32_t w) {\n"
      "  int64_t sa = lain_sext(a, w);\n"
      "  int64_t sb = lain_sext(b, w);\n"
      "  if (sb == 0) lain_trap(\"signed div by zero\");\n"
      "  if (sa == lain_smin(w) && sb == -1) lain_trap(\"signed div overflow\");\n"
      "  return (uint64_t)(sa / sb) & lain_mask(w);\n"
      "}\n\n"
      "static inline uint64_t lain_srem(uint64_t a, uint64_t b, uint32_t w) {\n"
      "  int64_t sa = lain_sext(a, w);\n"
      "  int64_t sb = lain_sext(b, w);\n"
      "  if (sb == 0) lain_trap(\"signed rem by zero\");\n"
      "  if (sa == lain_smin(w) && sb == -1) lain_trap(\"signed rem overflow\");\n"
      "  return (uint64_t)(sa % sb) & lain_mask(w);\n"
      "}\n\n"
      "static inline uint64_t lain_udiv(uint64_t a, uint64_t b, uint32_t w) {\n"
      "  uint64_t m = lain_mask(w);\n"
      "  uint64_t ua = a & m, ub = b & m;\n"
      "  if (ub == 0) lain_trap(\"unsigned div by zero\");\n"
      "  return (ua / ub) & m;\n"
      "}\n\n"
      "static inline uint64_t lain_urem(uint64_t a, uint64_t b, uint32_t w) {\n"
      "  uint64_t m = lain_mask(w);\n"
      "  uint64_t ua = a & m, ub = b & m;\n"
      "  if (ub == 0) lain_trap(\"unsigned rem by zero\");\n"
      "  return (ua % ub) & m;\n"
      "}\n\n"
      "static inline uint64_t lain_shl(uint64_t a, uint64_t b, uint32_t w) {\n"
      "  uint64_t m = lain_mask(w);\n"
      "  uint64_t sh = b & m;\n"
      "  if (sh >= w) lain_trap(\"shl >= width\");\n"
      "  return ((a & m) << sh) & m;\n"
      "}\n\n"
      "static inline uint64_t lain_lshr(uint64_t a, uint64_t b, uint32_t w) {\n"
      "  uint64_t m = lain_mask(w);\n"
      "  uint64_t sh = b & m;\n"
      "  if (sh >= w) lain_trap(\"lshr >= width\");\n"
      "  return (a & m) >> sh;\n"
      "}\n\n"
      "static inline uint64_t lain_ashr(uint64_t a, uint64_t b, uint32_t w) {\n"
      "  uint64_t m = lain_mask(w);\n"
      "  uint64_t sh = b & m;\n"
      "  if (sh >= w) lain_trap(\"ashr >= width\");\n"
      "  int64_t sa = lain_sext(a, w);\n"
      "  uint64_t u = (sa < 0) ? ~(~(uint64_t)sa >> sh) : ((uint64_t)sa >> sh);\n"
      "  return u & m;\n"
      "}\n\n"
      "static inline uint64_t lain_load_bits(const void *p, uint32_t w) {\n"
      "  uint64_t v = 0;\n"
      "  uint32_t n = (w + 7u) / 8u;\n"
      "  if (n > 0u) memcpy(&v, p, n);\n"
      "  return v & lain_mask(w);\n"
      "}\n\n"
      "static inline void lain_store_bits(void *p, uint64_t v, uint32_t w) {\n"
      "  uint32_t n = (w + 7u) / 8u;\n"
      "  uint64_t masked = v & lain_mask(w);\n"
      "  if (n > 0u) memcpy(p, &masked, n);\n"
      "}\n\n"
      "static inline void *lain_load_addr(const void *p) {\n"
      "  void *v = NULL;\n"
      "  memcpy(&v, p, sizeof(v));\n"
      "  return v;\n"
      "}\n\n"
      "static inline void lain_store_addr(void *p, void *v) {\n"
      "  memcpy(p, &v, sizeof(v));\n"
      "}\n\n"
      "static inline float lain_load_f32(const void *p) {\n"
      "  float v = 0.0f; memcpy(&v, p, sizeof(v)); return v;\n"
      "}\n"
      "static inline double lain_load_f64(const void *p) {\n"
      "  double v = 0.0; memcpy(&v, p, sizeof(v)); return v;\n"
      "}\n"
      "static inline void lain_store_f32(void *p, float v) {\n"
      "  memcpy(p, &v, sizeof(v));\n"
      "}\n"
      "static inline void lain_store_f64(void *p, double v) {\n"
      "  memcpy(p, &v, sizeof(v));\n"
      "}\n"
      "static inline float lain_bitcast_u32_f32(uint32_t v) {\n"
      "  float f = 0.0f; memcpy(&f, &v, sizeof(f)); return f;\n"
      "}\n"
      "static inline uint32_t lain_bitcast_f32_u32(float f) {\n"
      "  uint32_t v = 0; memcpy(&v, &f, sizeof(v)); return v;\n"
      "}\n"
      "static inline double lain_bitcast_u64_f64(uint64_t v) {\n"
      "  double d = 0.0; memcpy(&d, &v, sizeof(d)); return d;\n"
      "}\n"
      "static inline uint64_t lain_bitcast_f64_u64(double d) {\n"
      "  uint64_t v = 0; memcpy(&v, &d, sizeof(v)); return v;\n"
      "}\n\n"
      "typedef struct LainAllocaNode {\n"
      "  struct LainAllocaNode *next;\n"
      "  uint64_t _align;\n"
      "} LainAllocaNode;\n\n"
      "static inline void *lain_alloca_push(LainAllocaNode **head, size_t n) {\n"
      "  LainAllocaNode *node = (LainAllocaNode *)calloc(1, sizeof(LainAllocaNode) + n);\n"
      "  if (!node) lain_trap(\"alloca out of memory\");\n"
      "  node->next = *head;\n"
      "  *head = node;\n"
      "  return (void *)(node + 1);\n"
      "}\n\n"
      "static inline void lain_alloca_free(LainAllocaNode **head) {\n"
      "  LainAllocaNode *cur = *head;\n"
      "  while (cur) {\n"
      "    LainAllocaNode *nxt = cur->next;\n"
      "    free(cur);\n"
      "    cur = nxt;\n"
      "  }\n"
      "  *head = NULL;\n"
      "}\n\n",
      out);
}

static void emit_data(C99Ctx *ctx) {
  FILE *out = ctx->out;
  for (uint32_t i = 0; i < ctx->module->data_count; ++i) {
    const L1Data *d = &ctx->module->data[i];
    if (!d->is_writable) fputs("static const ", out);
    else fputs("static ", out);
    fputs("uint8_t ", out);
    emit_data_c_name(out, d);
    uint32_t sz = d->size ? d->size : 1u;
    fprintf(out, "[%u] = {", sz);
    if (d->size == 0) {
      fputc('0', out);
    } else {
      for (uint32_t b = 0; b < d->size; ++b) {
        if (b > 0) fputc(',', out);
        if ((b % 16u) == 0u) fputs("\n  ", out);
        fprintf(out, "0x%02x", d->bytes ? d->bytes[b] : 0);
      }
    }
    fputs("\n};\n", out);
  }
  if (ctx->module->data_count > 0) fputc('\n', out);
  fputs("const void *lain_c99_data_anchor(void) {\n", out);
  if (ctx->module->data_count > 0) {
    fputs("  return (const void *)", out);
    emit_data_c_name(out, &ctx->module->data[0]);
    fputs(";\n", out);
  } else {
    fputs("  return NULL;\n", out);
  }
  fputs("}\n\n", out);
}

static void emit_sub_signature(FILE *out, const L1Subroutine *sub,
                               bool is_extern_decl) {
  if (!is_extern_decl && !(sub->flags & SUBROUTINE_EXTERN) &&
      strcmp(sub->name, "lain_meta_v2_entry") != 0) {
    fputs("static ", out);
  }
  emit_sub_ret_type(out, sub);
  fputc(' ', out);
  emit_sub_c_name(out, sub);
  fputc('(', out);
  if (sub->param_count == 0) {
    fputs("void", out);
  } else {
    for (uint32_t p = 0; p < sub->param_count; ++p) {
      if (p > 0) fputs(", ", out);
      emit_type(out, sub->params[p].ty);
      if (!is_extern_decl) {
        fprintf(out, " arg_%u", p);
      }
    }
  }
  fputc(')', out);
}

static bool emit_subroutine_body(C99Ctx *ctx, const L1Subroutine *sub) {
  FILE *out = ctx->out;
  ctx->cur_sub = sub;
  ctx->next_id = 0;
  ctx->alloca_len = 0;
  ctx->has_loop_alloca = false;

  if (!prescan_allocas(ctx, sub->body, false)) return false;

  emit_sub_signature(out, sub, false);
  fputs(" {\n", out);

  if (ctx->has_loop_alloca) {
    fputs("  LainAllocaNode *_alloca_head = NULL;\n", out);
  }
  for (uint32_t i = 0; i < ctx->alloca_len; ++i) {
    if (!ctx->allocas[i].in_loop) {
      uint64_t words = (ctx->allocas[i].bytes + 7u) / 8u;
      if (words == 0) words = 1;
      fprintf(out, "  uint64_t _alloca_%u[%llu] = {0};\n", ctx->allocas[i].id,
              (unsigned long long)words);
    }
  }

  uint32_t saved_binds = ctx->bind_len;
  for (uint32_t p = 0; p < sub->param_count; ++p) {
    char c_arg[32];
    snprintf(c_arg, sizeof(c_arg), "arg_%u", p);
    push_binding(ctx, sub->params[p].name, c_arg, sub->params[p].ty);
  }

  bool ok = emit_region(ctx, sub->body, 1, NULL);
  pop_bindings(ctx, saved_binds);
  fputs("}\n\n", out);
  return ok;
}

static bool emit_main_wrapper(C99Ctx *ctx, const LainC99Options *opts) {
  if (!opts || !opts->entry) return true;
  const L1Subroutine *entry = c99_find_subroutine(ctx->module, opts->entry);
  if (!entry) {
    set_diag(ctx, 4005, NULL, "c99: entry subroutine '%s' not found",
             opts->entry);
    return false;
  }
  FILE *out = ctx->out;
  fputs("int main(void) {\n", out);
  if (entry->result_count == 0) {
    fputs("  ", out);
    emit_sub_c_name(out, entry);
    fputs("();\n", out);
    if (opts->print_result) fputs("  printf(\"result=void\\n\");\n", out);
    fputs("  return 0;\n", out);
  } else {
    fputs("  ", out);
    emit_type(out, entry->results[0]);
    fputs(" r = ", out);
    emit_sub_c_name(out, entry);
    fputs("();\n", out);
    if (opts->print_result) {
      if (entry->results[0]->kind == TY_BITS) {
        fprintf(out,
                "  printf(\"result=%%lld (u64=%%llu)\\n\", "
                "(long long)lain_sext((uint64_t)r, %u), "
                "(unsigned long long)((uint64_t)r & lain_mask(%u)));\n",
                entry->results[0]->width, entry->results[0]->width);
      } else if (entry->results[0]->kind == TY_ADDR) {
        fputs("  printf(\"result=%p\\n\", r);\n", out);
      } else {
        fputs("  printf(\"result=%g\\n\", (double)r);\n", out);
      }
      fputs("  return 0;\n", out);
    } else {
      fputs("  return (int)r;\n", out);
    }
  }
  fputs("}\n", out);
  return true;
}

int lainir_emit_c99(const L1Module *module, FILE *out,
                    const LainC99Options *opts, L1Diagnostic *diag) {
  if (!module || !out) return 1;

  C99Ctx ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.module = module;
  ctx.out = out;
  ctx.diag = diag;
  lainir_types_init(&ctx.types, module);

  emit_preamble(out);
  emit_data(&ctx);

  /* 前向声明所有 extern 与内部子过程（若存在同名定义则跳过 extern，并对 extern 去重）。 */
  for (uint32_t i = 0; i < module->subroutine_count; ++i) {
    const L1Subroutine *sub = &module->subroutines[i];
    if (sub->flags & SUBROUTINE_EXTERN) {
      if (c99_find_subroutine(module, sub->name) != sub) continue;
      bool dup = false;
      const char *lname = sub->link_name ? sub->link_name : sub->name;
      for (uint32_t j = 0; j < i; ++j) {
        const L1Subroutine *prev = &module->subroutines[j];
        if ((prev->flags & SUBROUTINE_EXTERN) &&
            c99_find_subroutine(module, prev->name) == prev) {
          const char *plname = prev->link_name ? prev->link_name : prev->name;
          if (strcmp(lname, plname) == 0) {
            dup = true;
            break;
          }
        }
      }
      if (dup) continue;
      fputs("extern ", out);
      emit_sub_signature(out, sub, true);
      fputs(";\n", out);
    } else {
      emit_sub_signature(out, sub, false);
      fputs(";\n", out);
    }
  }
  fputc('\n', out);

  int rc = 0;
  for (uint32_t i = 0; i < module->subroutine_count; ++i) {
    const L1Subroutine *sub = &module->subroutines[i];
    if (sub->flags & SUBROUTINE_EXTERN) continue;
    if (!emit_subroutine_body(&ctx, sub)) {
      rc = (diag && diag->code) ? diag->code : 4000;
      break;
    }
  }

  if (rc == 0 && !emit_main_wrapper(&ctx, opts)) {
    rc = (diag && diag->code) ? diag->code : 4005;
  }

  pop_bindings(&ctx, 0);
  free(ctx.binds);
  free(ctx.loops);
  free(ctx.allocas);
  return rc;
}
