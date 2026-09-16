/* LAINIR 验证器。规则见 seed/docs/LAINIR.md §11。 */
#include "lainir/verify.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lainir/infer.h"

/* 绑定表的**起始**容量，不是上限：按需翻倍。一个过程的活跃绑定数只该由
 * 这个过程自己决定。 */
#define L1V_MIN_BINDINGS 256u

typedef struct Binding {
  const char *name;
  const L1Type *ty;
  const struct Binding *next;
} Binding;

typedef struct LoopScope {
  const char *label;
  const L1Region *region;
  const struct LoopScope *outer;
} LoopScope;

typedef struct {
  const L1Module *module;
  LainIrTypes types;
  L1Diagnostic *diag;
  const L1Subroutine *sub;
  Binding *bindings;
  uint32_t binding_count;
  uint32_t binding_cap;
  bool failed;
} Verifier;

static bool fail(Verifier *v, int code, uint32_t line, uint32_t column,
                 const char *fmt, ...) {
  va_list args;
  if (v->failed) return false;
  v->failed = true;
  if (!v->diag) return false;
  v->diag->code = code;
  v->diag->line = line;
  v->diag->column = column;
  va_start(args, fmt);
  vsnprintf(v->diag->message, sizeof(v->diag->message), fmt, args);
  va_end(args);
  return false;
}

static const Binding *find_binding(const Verifier *v, const Binding *head,
                                   const char *name) {
  const Binding *b = head;
  (void)v;
  if (!name) return NULL;
  while (b) {
    if (b->name && strcmp(b->name, name) == 0) return b;
    b = b->next;
  }
  return NULL;
}

static const Binding *push_binding(Verifier *v, const Binding *head,
                                   const char *name, const L1Type *ty) {
  Binding *b;
  if (v->binding_count >= v->binding_cap) {
    uint32_t next = v->binding_cap ? v->binding_cap * 2u : L1V_MIN_BINDINGS;
    Binding *grown =
        (Binding *)realloc(v->bindings, sizeof(Binding) * (size_t)next);
    if (!grown) {
      fail(v, L1V_OUT_OF_MEMORY, 0, 0, "out of memory for bindings");
      return head;
    }
    v->bindings = grown;
    v->binding_cap = next;
  }
  b = &v->bindings[v->binding_count++];
  b->name = name;
  b->ty = ty;
  b->next = head;
  return b;
}

static bool same_type(const L1Type *a, const L1Type *b) {
  if (!a || !b) return false;
  if (a->kind != b->kind) return false;
  if (a->kind == TY_ADDR) return true;
  return a->width == b->width;
}

static bool byte_addressable(const L1Type *ty) {
  uint32_t width = lainir_type_width(ty);
  if (ty && ty->kind == TY_ADDR) return true;
  if (width == 0 || width > 64 || (width % 8) != 0) return false;
  return true;
}

static const LoopScope *find_loop(const LoopScope *loops, const char *label) {
  const LoopScope *it = loops;
  if (!label) return NULL;
  while (it) {
    if (it->label && strcmp(it->label, label) == 0) return it;
    it = it->outer;
  }
  return NULL;
}

/* --- 字面量的宽度从哪来（文档 §4.1） -------------------------------------
 * 字面量**不带类型**，只要求有人声明它的宽度；类别由算子决定。
 * 返回 0 表示没有任何声明给出宽度 —— 那才是错误。 */

static uint32_t literal_width(Verifier *v, const L1Region *region,
                              const L1Inst *inst, uint32_t index,
                              const LoopScope *loops) {
  if (inst->has_ty && inst->ty) return lainir_type_width(inst->ty);
  switch (inst->kind) {
  case INST_YIELD:
    return index < region->result_count
               ? lainir_type_width(region->results[index])
               : 0;
  case INST_RETURN:
    return v->sub->result_count > 0
               ? lainir_type_width(v->sub->results[0])
               : 0;
  case INST_LEA:
    return 64; /* scale / offset 是地址宽度的量 */
  case INST_CALL:
  case INST_CALL_INDIRECT: {
    const L1Subroutine *callee =
        inst->kind == INST_CALL
            ? lainir_find_subroutine(v->module, inst->symbol)
            : NULL;
    if (callee && index < callee->param_count)
      return lainir_type_width(callee->params[index].ty);
    return 0;
  }
  case INST_CONTINUE: {
    const LoopScope *loop = find_loop(loops, inst->label);
    if (loop && index < loop->region->param_count)
      return lainir_type_width(loop->region->params[index].param.ty);
    return 0;
  }
  case INST_BREAK: {
    const LoopScope *loop = find_loop(loops, inst->label);
    if (loop && index < loop->region->result_count)
      return lainir_type_width(loop->region->results[index]);
    return 0;
  }
  default:
    return 0;
  }
}

/* 值操作数的类型；字面量返回 NULL（它没有类别）。 */
static const L1Type *operand_ty(Verifier *v, const Binding *names,
                                const L1Region *region, const L1Inst *inst,
                                uint32_t index, const LoopScope *loops) {
  const L1Operand *op = &inst->operands[index];
  const Binding *b;
  (void)region;
  (void)loops;
  if (op->kind != OPERAND_VALUE) return NULL;
  b = find_binding(v, names, op->name);
  return b ? b->ty : NULL;
}

/* --- 分类 ----------------------------------------------------------------- */

static bool is_int_arith(L1InstKind kind) {
  switch (kind) {
  case INST_ADD: case INST_SUB: case INST_MUL:
  case INST_SDIV: case INST_UDIV: case INST_SREM: case INST_UREM:
  case INST_AND: case INST_OR: case INST_XOR:
  case INST_SHL: case INST_LSHR: case INST_ASHR:
    return true;
  default:
    return false;
  }
}

static bool is_int_compare(L1InstKind kind) {
  switch (kind) {
  case INST_EQ: case INST_NE:
  case INST_SLT: case INST_SLE: case INST_SGT: case INST_SGE:
  case INST_ULT: case INST_ULE: case INST_UGT: case INST_UGE:
    return true;
  default:
    return false;
  }
}

static bool is_float_arith(L1InstKind kind) {
  return kind == INST_FADD || kind == INST_FSUB || kind == INST_FMUL ||
         kind == INST_FDIV;
}

static bool is_float_compare(L1InstKind kind) {
  switch (kind) {
  case INST_FOEQ: case INST_FONE: case INST_FOLT: case INST_FOLE:
  case INST_FOGT: case INST_FOGE:
  case INST_FUEQ: case INST_FUNE: case INST_FULT: case INST_FULE:
  case INST_FUGT: case INST_FUGE:
    return true;
  default:
    return false;
  }
}

static bool is_vector(L1InstKind kind) {
  return kind >= INST_VADD && kind <= INST_VCMPGT;
}

static bool is_atomic(L1InstKind kind) {
  return kind >= INST_XCHG && kind <= INST_RMW_XOR;
}

static bool is_int_conv(L1InstKind kind) {
  return kind == INST_ZEXT || kind == INST_SEXT || kind == INST_TRUNC ||
         kind == INST_BITCAST;
}

static bool is_float_conv(L1InstKind kind) {
  return kind == INST_FPEXT || kind == INST_FPTRUNC || kind == INST_FPTOSI ||
         kind == INST_FPTOUI || kind == INST_SITOFP || kind == INST_UITOFP;
}

static bool produces_value(L1InstKind kind) {
  switch (kind) {
  case INST_STORE: case INST_YIELD: case INST_BREAK: case INST_CONTINUE:
  case INST_RETURN:
    return false;
  default:
    return true;
  }
}

/* 区域里有没有一个针对 label 的 #break，参数个数是 want。 */
static bool contains_break_for(const L1Region *region, const char *label,
                               uint32_t want) {
  uint32_t i;
  if (!region) return false;
  for (i = 0; i < region->inst_count; i++) {
    const L1Inst *inst = &region->insts[i];
    if (inst->kind == INST_BREAK && inst->label && label &&
        strcmp(inst->label, label) == 0 && inst->operand_count == want)
      return true;
    if (contains_break_for(inst->body, label, want)) return true;
    if (contains_break_for(inst->else_body, label, want)) return true;
  }
  return false;
}

/* 区域的每条路径都交了值。做结构判定，不做可达性分析。
 * loop_label 非空表示「这是一个循环体」，那种区域的出口值来自 #break。 */
static bool yields_values(Verifier *v, const L1Region *region, uint32_t want,
                          const char *loop_label) {
  const L1Inst *last;
  (void)v;
  if (!region || region->inst_count == 0) return false;
  if (loop_label) return contains_break_for(region, loop_label, want);
  last = &region->insts[region->inst_count - 1];
  if (last->kind == INST_YIELD) return last->operand_count == want;
  if (last->kind == INST_RETURN || last->kind == INST_BREAK) return true;
  if (last->kind == INST_IF && last->else_body)
    return yields_values(v, last->body, want, NULL) &&
           yields_values(v, last->else_body, want, NULL);
  return false;
}

/* --- 指令 ----------------------------------------------------------------- */

static bool check_operands(Verifier *v, const L1Region *region,
                           const L1Inst *inst, const Binding *names,
                           const LoopScope *loops) {
  uint32_t i;
  for (i = 0; i < inst->operand_count; i++) {
    const L1Operand *op = &inst->operands[i];
    if (op->kind == OPERAND_VALUE) {
      if (!find_binding(v, names, op->name))
        return fail(v, L1V_UNDEFINED_VALUE, inst->line, inst->column,
                    "undefined value `%s`", op->name ? op->name : "?");
    } else if (literal_width(v, region, inst, i, loops) == 0) {
      return fail(v, L1V_NO_TYPE_FOR_LITERAL, inst->line, inst->column,
                  "literal operand %u has no declared width (kind %d)", i,
                  (int)inst->kind);
    }
  }
  return true;
}

/* 所有操作数都要是指定的类型类别和宽度。 */
static bool check_operand_class(Verifier *v, const Binding *names,
                                const L1Region *region, const L1Inst *inst,
                                const LoopScope *loops, L1TypeKind want_kind,
                                bool check_width) {
  uint32_t want = inst->ty ? lainir_type_width(inst->ty) : 0;
  uint32_t i;
  for (i = 0; i < inst->operand_count; i++) {
    const L1Type *ty = operand_ty(v, names, region, inst, i, loops);
    if (!ty) continue; /* 上面已经报过 */
    if (ty->kind != want_kind ||
        (check_width && want != 0 && lainir_type_width(ty) != want))
      return fail(v, L1V_BAD_OPERAND_TYPE, inst->line, inst->column,
                  "operand %u has the wrong physical type (kind %d)", i,
                  (int)inst->kind);
  }
  return true;
}

/* 物理类型的宽度上限：一个值就是机器的一个字。
 * 位串和浮点超过 64 位需要多字表示——VM 的 L1Value 装不下，后端也没合法化，
 * 所以规范里先不允许。伪装的「能编译」比拒绝更糟。 */
#define L1V_MAX_VALUE_WIDTH 64u

static bool check_type_width(Verifier *v, const L1Type *ty, uint32_t line,
                             uint32_t column) {
  if (!ty || ty->kind == TY_ADDR) return true;
  if (ty->width == 0)
    return fail(v, L1V_ZERO_WIDTH, line, column, "physical type has width 0");
  if (ty->kind != TY_VEC && ty->width > L1V_MAX_VALUE_WIDTH)
    return fail(v, L1V_WIDTH_TOO_WIDE, line, column,
                "width %u is above one machine word; wide values need "
                "multi-word legalization",
                ty->width);
  return true;
}

static bool check_type_arg(Verifier *v, const L1Inst *inst) {
  if (!inst->has_ty || !inst->ty)
    return fail(v, L1V_MISSING_TYPE, inst->line, inst->column,
                "instruction kind %d needs an explicit type argument",
                (int)inst->kind);
  return check_type_width(v, inst->ty, inst->line, inst->column);
}

static bool check_result_count(Verifier *v, const L1Inst *inst, uint32_t want) {
  if (inst->result_count != want)
    return fail(v, L1V_BAD_RESULT_COUNT, inst->line, inst->column,
                "instruction kind %d defines %u result(s), expected %u",
                (int)inst->kind, inst->result_count, want);
  return true;
}

static bool verify_region(Verifier *v, const L1Region *region,
                          const Binding *incoming, const LoopScope *loops,
                          uint32_t scope_start);

/* 元数先查：元数不对的时候，逐操作数的类型检查没有意义
 * （也会报出更含糊的「字面量没有类型」）。 */
static bool check_arity(Verifier *v, const L1Inst *inst) {
  switch (inst->kind) {
  case INST_CALL: {
    const L1Subroutine *callee =
        lainir_find_subroutine(v->module, inst->symbol);
    if (!callee)
      return fail(v, L1V_UNKNOWN_CALLEE, inst->line, inst->column,
                  "#call: unknown callee `%s`",
                  inst->symbol ? inst->symbol : "?");
    if (inst->operand_count != callee->param_count)
      return fail(v, L1V_BAD_CALL_ARITY, inst->line, inst->column,
                  "#call to `%s` passes %u argument(s), expected %u",
                  callee->name, inst->operand_count, callee->param_count);
    return true;
  }
  case INST_CALL_INDIRECT:
    if (inst->operand_count < 1)
      return fail(v, L1V_BAD_CALL_ARITY, inst->line, inst->column,
                  "#call_indirect needs a target");
    return true;
  case INST_LEA:
    if (inst->operand_count != 4)
      return fail(v, L1V_BAD_OPERAND_TYPE, inst->line, inst->column,
                  "#lea takes base, idx, scale, offset");
    return true;
  case INST_STORE:
    if (inst->operand_count != 2)
      return fail(v, L1V_BAD_OPERAND_TYPE, inst->line, inst->column,
                  "#store takes value, destination");
    return true;
  case INST_ALLOCA:
    if (inst->operand_count != 1)
      return fail(v, L1V_ALLOCA_NOT_CONST, inst->line, inst->column,
                  "#alloca takes one count");
    return true;
  case INST_IF:
    if (inst->operand_count != 1)
      return fail(v, L1V_BAD_CONDITION, inst->line, inst->column,
                  "#if takes one condition");
    return true;
  case INST_LOOP:
    if (inst->operand_count != 0)
      return fail(v, L1V_BAD_CONDITION, inst->line, inst->column,
                  "#loop takes no operands");
    return true;
  default:
    return true;
  }
}

static bool verify_inst(Verifier *v, const L1Region *region, uint32_t position,
                        const L1Inst *inst, const Binding *names,
                        const LoopScope *loops, bool *terminated) {
  uint32_t i;

  (void)position;
  if (!check_arity(v, inst)) return false;
  if (!check_operands(v, region, inst, names, loops)) return false;

  /* #eval 的实参必须是编译期已知的（文档 §11 第 12 条）。 */
  if (inst->is_eval) {
    for (i = 0; i < inst->operand_count; i++) {
      if (inst->operands[i].kind != OPERAND_INT &&
          inst->operands[i].kind != OPERAND_FLOAT)
        return fail(v, L1V_EVAL_ARG_NOT_CONST, inst->line, inst->column,
                    "#eval argument %u is not compile-time known", i);
    }
  }

  if (is_int_arith(inst->kind)) {
    if (!check_type_arg(v, inst) || !check_result_count(v, inst, 1)) return false;
    return check_operand_class(v, names, region, inst, loops, TY_BITS, true);
  }
  if (is_int_compare(inst->kind)) {
    if (!check_type_arg(v, inst) || !check_result_count(v, inst, 1)) return false;
    return check_operand_class(v, names, region, inst, loops, TY_BITS, true);
  }
  if (is_float_arith(inst->kind) || is_float_compare(inst->kind)) {
    if (!check_type_arg(v, inst) || !check_result_count(v, inst, 1)) return false;
    return check_operand_class(v, names, region, inst, loops, TY_FLOATS, true);
  }
  if (is_vector(inst->kind)) {
    if (!check_type_arg(v, inst) || !check_result_count(v, inst, 1)) return false;
    return check_operand_class(v, names, region, inst, loops, TY_VEC, true);
  }
  if (is_atomic(inst->kind)) {
    if (!check_type_arg(v, inst)) return false;
    return true;
  }
  if (is_int_conv(inst->kind)) {
    if (!check_type_arg(v, inst)) return false;
    return check_operand_class(v, names, region, inst, loops, TY_BITS, false);
  }
  if (is_float_conv(inst->kind)) {
    L1TypeKind want = (inst->kind == INST_SITOFP || inst->kind == INST_UITOFP)
                          ? TY_BITS
                          : TY_FLOATS;
    if (!check_type_arg(v, inst)) return false;
    return check_operand_class(v, names, region, inst, loops, want, false);
  }

  switch (inst->kind) {
  case INST_INT2PTR:
    if (!check_type_arg(v, inst)) return false;
    return check_operand_class(v, names, region, inst, loops, TY_BITS, false);
  case INST_PTR2INT:
    if (!check_type_arg(v, inst)) return false;
    return check_operand_class(v, names, region, inst, loops, TY_ADDR, false);

  case INST_LEA: {
    const L1Type *base;
    if (inst->operand_count != 4) {
      return fail(v, L1V_BAD_CONDITION, inst->line, inst->column,
                  "#lea takes base, idx, scale, offset");
    }
    if (inst->result_count != 1) return check_result_count(v, inst, 1);
    base = operand_ty(v, names, region, inst, 0, loops);
    if (base && base->kind != TY_ADDR)
      return fail(v, L1V_BAD_OPERAND_TYPE, inst->line, inst->column,
                  "#lea base must be #addr");
    return true;
  }

  case INST_LOAD: {
    const L1Type *addr_ty;
    if (!check_type_arg(v, inst) || !check_result_count(v, inst, 1)) return false;
    if (!byte_addressable(inst->ty))
      return fail(v, L1V_BAD_MEMORY_WIDTH, inst->line, inst->column,
                  "#load width is not byte addressable");
    addr_ty = operand_ty(v, names, region, inst, 0, loops);
    if (addr_ty && addr_ty->kind != TY_ADDR)
      return fail(v, L1V_BAD_OPERAND_TYPE, inst->line, inst->column,
                  "#load needs an #addr operand");
    return true;
  }
  case INST_STORE: {
    const L1Type *addr_ty;
    const L1Type *value_ty;
    if (!check_type_arg(v, inst) || !check_result_count(v, inst, 0)) return false;
    if (!byte_addressable(inst->ty))
      return fail(v, L1V_BAD_MEMORY_WIDTH, inst->line, inst->column,
                  "#store width is not byte addressable");
    if (inst->operand_count != 2)
      return fail(v, L1V_BAD_CONDITION, inst->line, inst->column,
                  "#store takes value, destination");
    value_ty = operand_ty(v, names, region, inst, 0, loops);
    addr_ty = operand_ty(v, names, region, inst, 1, loops);
    if (addr_ty && addr_ty->kind != TY_ADDR)
      return fail(v, L1V_BAD_OPERAND_TYPE, inst->line, inst->column,
                  "#store destination must be #addr");
    if (value_ty && !same_type(value_ty, inst->ty) &&
        !(inst->ty->kind == TY_ADDR && value_ty->kind == TY_ADDR))
      return fail(v, L1V_BAD_OPERAND_TYPE, inst->line, inst->column,
                  "#store value does not match the physical type");
    return true;
  }
  case INST_ALLOCA: {
    if (!check_type_arg(v, inst) || !check_result_count(v, inst, 1)) return false;
    if (inst->operand_count != 1 || inst->operands[0].kind == OPERAND_VALUE)
      return fail(v, L1V_ALLOCA_NOT_CONST, inst->line, inst->column,
                  "#alloca needs a constant count");
    if (inst->operands[0].bits == 0)
      return fail(v, L1V_BAD_ALLOCA_COUNT, inst->line, inst->column,
                  "#alloca count must be positive");
    return true;
  }

  case INST_DATA_ADDR:
    if (!check_result_count(v, inst, 1)) return false;
    if (!lainir_find_data(v->module, inst->symbol))
      return fail(v, L1V_UNKNOWN_SYMBOL, inst->line, inst->column,
                  "#data_addr: unknown symbol `%s`",
                  inst->symbol ? inst->symbol : "?");
    return true;
  case INST_PROC_ADDR:
    if (!check_result_count(v, inst, 1)) return false;
    if (!lainir_find_subroutine(v->module, inst->symbol))
      return fail(v, L1V_UNKNOWN_PROC, inst->line, inst->column,
                  "#proc_addr: unknown subroutine `%s`",
                  inst->symbol ? inst->symbol : "?");
    return true;

  case INST_CALL: {
    const L1Subroutine *callee =
        lainir_find_subroutine(v->module, inst->symbol);
    if (!callee)
      return fail(v, L1V_UNKNOWN_CALLEE, inst->line, inst->column,
                  "#call: unknown callee `%s`",
                  inst->symbol ? inst->symbol : "?");
    if (inst->operand_count != callee->param_count)
      return fail(v, L1V_BAD_CALL_ARITY, inst->line, inst->column,
                  "#call to `%s` passes %u argument(s), expected %u",
                  callee->name, inst->operand_count, callee->param_count);
    if (!check_result_count(v, inst, callee->result_count)) return false;
    for (i = 0; i < inst->operand_count; i++) {
      const L1Type *ty = operand_ty(v, names, region, inst, i, loops);
      const L1Type *want = callee->params[i].ty;
      if (ty && want && !same_type(ty, want))
        return fail(v, L1V_BAD_OPERAND_TYPE, inst->line, inst->column,
                    "#call to `%s`: argument %u has the wrong type",
                    callee->name, i);
    }
    return true;
  }
  case INST_CALL_INDIRECT:
    if (inst->operand_count < 1)
      return fail(v, L1V_BAD_CALL_ARITY, inst->line, inst->column,
                  "#call_indirect needs a target");
    return true;

  case INST_IF: {
    const L1Type *cond;
    if (inst->operand_count != 1)
      return fail(v, L1V_BAD_CONDITION, inst->line, inst->column,
                  "#if takes one condition");
    cond = operand_ty(v, names, region, inst, 0, loops);
    if (cond && !(cond->kind == TY_BITS && lainir_type_width(cond) == 1))
      return fail(v, L1V_BAD_CONDITION, inst->line, inst->column,
                  "#if condition must be #bits<1>");
    if (!check_result_count(v, inst,
                            inst->body ? inst->body->result_count : 0))
      return false;
    if (inst->body && inst->body->result_count > 0 &&
        !yields_values(v, inst->body, inst->body->result_count, NULL))
      return fail(v, L1V_MISSING_YIELD, inst->line, inst->column,
                  "#if region produces a value on a path that does not yield");
    {
      uint32_t saved = v->binding_count;
      const Binding *inner = names;
      uint32_t k;
      for (k = 0; k < inst->body->param_count; k++)
        inner = push_binding(v, inner, inst->body->params[k].param.name,
                             inst->body->params[k].param.ty);
      if (!verify_region(v, inst->body, inner, loops, saved)) return false;
      v->binding_count = saved;
    }
    if (inst->else_body) {
      uint32_t saved = v->binding_count;
      if (!verify_region(v, inst->else_body, names, loops, saved)) return false;
      v->binding_count = saved;
    }
    return true;
  }

  case INST_LOOP: {
    LoopScope scope;
    if (inst->operand_count != 0)
      return fail(v, L1V_BAD_CONDITION, inst->line, inst->column,
                  "#loop takes no operands");
    if (!inst->label)
      return fail(v, L1V_UNKNOWN_LOOP, inst->line, inst->column,
                  "#loop needs a label");
    if (!check_result_count(v, inst,
                            inst->body ? inst->body->result_count : 0))
      return false;
    scope.label = inst->label;
    scope.region = inst->body;
    scope.outer = loops;
    if (inst->body) {
      uint32_t saved = v->binding_count;
      const Binding *inner = names;
      uint32_t k;
      for (k = 0; k < inst->body->param_count; k++)
        inner = push_binding(v, inner, inst->body->params[k].param.name,
                             inst->body->params[k].param.ty);
      if (!verify_region(v, inst->body, inner, &scope, saved)) return false;
      v->binding_count = saved;
    }
    if (inst->body && inst->body->result_count > 0 &&
        !contains_break_for(inst->body, inst->label,
                            inst->body->result_count))
      return fail(v, L1V_MISSING_YIELD, inst->line, inst->column,
                  "#loop produces a value but no matching #break is present");
    return true;
  }

  case INST_SWITCH: {
    uint32_t a;
    uint32_t b;
    for (a = 0; a < inst->case_count; a++) {
      for (b = 0; b < a; b++) {
        if (inst->cases[a].value == inst->cases[b].value)
          return fail(v, L1V_DUPLICATE_CASE, inst->line, inst->column,
                      "#switch case value %llu appears twice",
                      (unsigned long long)inst->cases[a].value);
      }
    }
    return fail(v, L1V_SWITCH_UNSUPPORTED, inst->line, inst->column,
                "#switch is not supported yet");
  }

  case INST_YIELD:
    if (!check_result_count(v, inst, 0)) return false;
    if (inst->operand_count != region->result_count)
      return fail(v, L1V_MISSING_YIELD, inst->line, inst->column,
                  "#yield delivers %u value(s), the region declares %u",
                  inst->operand_count, region->result_count);
    for (i = 0; i < inst->operand_count; i++) {
      const L1Type *ty = operand_ty(v, names, region, inst, i, loops);
      if (ty && !same_type(ty, region->results[i]))
        return fail(v, L1V_BAD_OPERAND_TYPE, inst->line, inst->column,
                    "#yield value %u does not match the declared result", i);
    }
    break;

  case INST_BREAK:
  case INST_CONTINUE: {
    const LoopScope *loop = find_loop(loops, inst->label);
    uint32_t want;
    if (!loop)
      return fail(v, L1V_UNKNOWN_LOOP, inst->line, inst->column,
                  "jump to unknown loop `%s`", inst->label ? inst->label : "?");
    if (!check_result_count(v, inst, 0)) return false;
    want = inst->kind == INST_BREAK ? loop->region->result_count
                                    : loop->region->param_count;
    if (inst->operand_count != want)
      return fail(v, L1V_BAD_JUMP_ARITY, inst->line, inst->column,
                  "jump to `%s` passes %u value(s), expected %u", loop->label,
                  inst->operand_count, want);
    break;
  }

  case INST_RETURN:
    if (!check_result_count(v, inst, 0)) return false;
    if (inst->operand_count != v->sub->result_count)
      return fail(v, L1V_BAD_RETURN, inst->line, inst->column,
                  "#return delivers %u value(s), the signature declares %u",
                  inst->operand_count, v->sub->result_count);
    if (inst->operand_count == 1) {
      const L1Type *ty = operand_ty(v, names, region, inst, 0, loops);
      if (ty && !same_type(ty, v->sub->results[0]))
        return fail(v, L1V_BAD_RETURN, inst->line, inst->column,
                    "#return value does not match the declared result");
    }
    break;

  default:
    /* 上面没覆盖到的、产出值的算子。 */
    if (produces_value(inst->kind) && !check_result_count(v, inst, 1))
      return false;
    break;
  }

  switch (inst->kind) {
  case INST_YIELD:
  case INST_BREAK:
  case INST_CONTINUE:
  case INST_RETURN:
    *terminated = true;
    break;
  default:
    break;
  }
  return true;
}

static bool verify_region(Verifier *v, const L1Region *region,
                          const Binding *incoming, const LoopScope *loops,
                          uint32_t scope_start) {
  const Binding *names = incoming;
  bool terminated = false;
  uint32_t i;

  if (!region) return true;

  for (i = 0; i < region->param_count; i++) {
    if (!region->params[i].param.ty)
      return fail(v, L1V_MISSING_TYPE, 0, 0, "region parameter without a type");
    if (!check_type_width(v, region->params[i].param.ty, 0, 0)) return false;
  }
  for (i = 0; i < region->result_count; i++) {
    if (!region->results[i])
      return fail(v, L1V_MISSING_TYPE, 0, 0, "region result without a type");
    if (!check_type_width(v, region->results[i], 0, 0)) return false;
  }

  for (i = 0; i < region->inst_count; i++) {
    const L1Inst *inst = &region->insts[i];
    uint32_t k;
    if (terminated)
      return fail(v, L1V_AFTER_TERMINATOR, inst->line, inst->column,
                  "instruction follows a terminator");
    if (!verify_inst(v, region, i, inst, names, loops, &terminated))
      return false;
    /* 结果在**后面**的指令里可见；同一区域内重复定义是错误，
     * 但外层同名可以被内层遮蔽（所以只看本区域新加的那些）。 */
    for (k = 0; k < inst->result_count; k++) {
      const Binding *found = find_binding(v, names, inst->results[k]);
      const L1Type *ty = lainir_inst_result_type(&v->types, inst, k);
      if (!ty)
        return fail(v, L1V_MISSING_TYPE, inst->line, inst->column,
                    "cannot determine the type of result `%s`",
                    inst->results[k] ? inst->results[k] : "?");
      if (found && (uint32_t)(found - v->bindings) >= scope_start)
        return fail(v, L1V_DUPLICATE_BINDING, inst->line, inst->column,
                    "duplicate binding `%s` in the same region",
                    inst->results[k]);
      names = push_binding(v, names, inst->results[k], ty);
    }
  }
  return true;
}

/* --- 模块 ----------------------------------------------------------------- */

static bool verify_subroutine(Verifier *v, const L1Subroutine *sub) {
  const Binding *names = NULL;
  uint32_t i;

  if (!sub->name)
    return fail(v, L1V_MISSING_TYPE, 0, 0, "subroutine without a name");
  if (sub->flags & SUBROUTINE_EXTERN) {
    if (sub->body)
      return fail(v, L1V_BODY_DECLARES_RESULTS, 0, 0,
                  "an extern subroutine must not have a body");
  } else if (!sub->body) {
    return fail(v, L1V_MISSING_TYPE, 0, 0, "subroutine without a body");
  }

  for (i = 0; i < sub->param_count; i++) {
    if (!sub->params[i].ty)
      return fail(v, L1V_MISSING_TYPE, 0, 0, "parameter without a type");
    if (!check_type_width(v, sub->params[i].ty, 0, 0)) return false;
    names = push_binding(v, names, sub->params[i].name, sub->params[i].ty);
  }
  for (i = 0; i < sub->result_count; i++) {
    if (!sub->results[i])
      return fail(v, L1V_MISSING_TYPE, 0, 0, "result without a type");
    if (!check_type_width(v, sub->results[i], 0, 0)) return false;
  }
  if (sub->body && sub->body->result_count > 0)
    return fail(v, L1V_BODY_DECLARES_RESULTS, 0, 0,
                "a procedure body must not declare region results");

  v->sub = sub;
  if (sub->body && !verify_region(v, sub->body, names, NULL, 0)) return false;
  return true;
}

/* 模块级检查。**这里每个失败都必须返回非 0**：`fail()` 返回的是 bool，
 * 写成 `return fail(...)` 会返回 0 = 成功，而 diag 里却写着错误——
 * 「重复子过程」和「重复数据」曾经就是这样漏出去的（返回 0，
 * 于是装载器照样收下，find_sub 按名字静默取第一个）。 */
static int verify_module(Verifier *v, const L1Module *module) {
  uint32_t i;
  uint32_t j;

  for (i = 0; i < module->subroutine_count; i++) {
    for (j = 0; j < i; j++) {
      if (module->subroutines[i].name && module->subroutines[j].name &&
          strcmp(module->subroutines[i].name, module->subroutines[j].name) == 0) {
        fail(v, L1V_DUPLICATE_SUBROUTINE, 0, 0, "duplicate subroutine `%s`",
             module->subroutines[i].name);
        return 1;
      }
    }
  }
  for (i = 0; i < module->data_count; i++) {
    for (j = 0; j < i; j++) {
      if (module->data[i].symbol && module->data[j].symbol &&
          strcmp(module->data[i].symbol, module->data[j].symbol) == 0) {
        fail(v, L1V_DUPLICATE_DATA, 0, 0, "duplicate data `%s`",
             module->data[i].symbol);
        return 1;
      }
    }
  }

  for (i = 0; i < module->subroutine_count; i++) {
    v->binding_count = 0;
    if (!verify_subroutine(v, &module->subroutines[i])) return 1;
  }
  return 0;
}

int lainir_verify(const L1Module *module, L1Diagnostic *diag) {
  Verifier v;
  int result;

  if (!module) return 1;
  memset(&v, 0, sizeof(v));
  v.module = module;
  v.diag = diag;
  lainir_types_init(&v.types, module);
  if (diag) {
    diag->code = 0;
    diag->message[0] = '\0';
  }

  result = verify_module(&v, module);
  free(v.bindings);
  return result;
}
