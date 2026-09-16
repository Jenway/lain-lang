/* lainir/parse.h 的实现。递归下降，一次扫过去。 */
#include "lainir/parse.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lainir/opname.h"

#define L1P_MAX_RESULTS 4u
#define L1P_MAX_PARAMS 16u

typedef struct {
  const L1Inst **items;
  uint32_t count;
  uint32_t cap;
} InstList;

typedef struct {
  L1Builder *builder;
  const char *src;
  uint32_t pos;
  uint32_t line;
  uint32_t column;
  L1Diagnostic *diag;
  bool failed;
  bool inline_only;
  uint32_t temp_seq;
} Parser;

/* 把一张表按需加倍扩容。返回 false 只表示 arena 也拿不出内存了。
 *
 * 解析器里凡「随模块增长」的表都走这里——过程表、数据表、单个数据对象的
 * 字节。定长上限只该来自模块本身，不该来自解析器。 */
static bool grow_table(L1Builder *builder, void **items, uint32_t *cap,
                       uint32_t count, size_t elem_size) {
  uint32_t next;
  void *fresh;
  if (count < *cap) return true;
  next = *cap ? *cap * 2u : 32u;
  fresh = lainir_builder_alloc(builder, elem_size * (size_t)next);
  if (!fresh) return false;
  if (*items && count) memcpy(fresh, *items, elem_size * (size_t)count);
  *items = fresh;
  *cap = next;
  return true;
}

/* --- 词法 ----------------------------------------------------------------- */

static char cur(const Parser *p) { return p->src[p->pos]; }

static char at(const Parser *p, uint32_t ahead) {
  if (!p->src[p->pos]) return 0;
  return p->src[p->pos + ahead];
}

static void advance(Parser *p) {
  char c = p->src[p->pos];
  if (!c) return;
  p->pos++;
  if (c == '\n') {
    p->line++;
    p->column = 1;
  } else {
    p->column++;
  }
}

static bool fail(Parser *p, int code, const char *fmt, ...) {
  va_list args;
  if (p->failed) return false;
  p->failed = true;
  if (!p->diag) return false;
  p->diag->code = code;
  p->diag->line = p->line;
  p->diag->column = p->column;
  va_start(args, fmt);
  vsnprintf(p->diag->message, sizeof(p->diag->message), fmt, args);
  va_end(args);
  return false;
}

static void skip(Parser *p) {
  for (;;) {
    char c = cur(p);
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      advance(p);
      continue;
    }
    if (c == '/' && at(p, 1) == '/') {
      while (cur(p) && cur(p) != '\n') advance(p);
      continue;
    }
    return;
  }
}

/* 只吃行内空白与行尾注释，**不吃换行**。
 * `#return %r` / `#yield %a` 的操作数是不带括号的，必须靠行尾界定，
 * 否则下一行的 `#op` 会被当成嵌套糖吃掉。 */
static void skip_inline(Parser *p) {
  for (;;) {
    char c = cur(p);
    if (c == ' ' || c == '\t' || c == '\r') {
      advance(p);
      continue;
    }
    if (c == '/' && at(p, 1) == '/') {
      while (cur(p) && cur(p) != '\n') advance(p);
      continue;
    }
    return;
  }
}

static void skip_ws(Parser *p) {
  if (p->inline_only)
    skip_inline(p);
  else
    skip(p);
}

static bool match(Parser *p, char c) {
  if (cur(p) == c) {
    advance(p);
    return true;
  }
  return false;
}

static bool expect(Parser *p, char c, const char *what) {
  if (match(p, c)) return true;
  return fail(p, 3001, "expected `%c` %s, got `%c`", c, what,
              cur(p) ? cur(p) : ' ');
}

static bool ident(Parser *p, char *out, uint32_t cap) {
  uint32_t n = 0;
  char c = cur(p);
  if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'))
    return fail(p, 3001, "expected an identifier");
  while (n + 1 < cap) {
    c = cur(p);
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_'))
      break;
    out[n++] = c;
    advance(p);
  }
  out[n] = '\0';
  return true;
}

/* 符号名：允许点和短横线（外部链接名习惯）。 */
static bool symbol_name(Parser *p, char *out, uint32_t cap) {
  uint32_t n = 0;
  char c = cur(p);
  if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'))
    return fail(p, 3001, "expected a symbol name");
  while (n + 1 < cap) {
    c = cur(p);
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-'))
      break;
    out[n++] = c;
    advance(p);
  }
  out[n] = '\0';
  return true;
}

static bool value_name(Parser *p, char *out, uint32_t cap) {
  if (!match(p, '%')) return fail(p, 3001, "expected a value (`%%name`)");
  return ident(p, out, cap);
}

static bool integer(Parser *p, uint64_t *out) {
  uint64_t value = 0;
  uint32_t digits = 0;
  if (cur(p) == '0' && (at(p, 1) == 'x' || at(p, 1) == 'X')) {
    advance(p);
    advance(p);
    while (1) {
      char c = cur(p);
      uint32_t d;
      if (c >= '0' && c <= '9')
        d = (uint32_t)(c - '0');
      else if (c >= 'a' && c <= 'f')
        d = (uint32_t)(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F')
        d = (uint32_t)(c - 'A' + 10);
      else
        break;
      value = value * 16u + d;
      advance(p);
      digits++;
    }
    if (!digits) return fail(p, 3004, "malformed hex literal");
    *out = value;
    return true;
  }
  while (cur(p) >= '0' && cur(p) <= '9') {
    value = value * 10u + (uint64_t)(cur(p) - '0');
    advance(p);
    digits++;
  }
  if (!digits) return fail(p, 3004, "expected an integer literal");
  *out = value;
  return true;
}

static bool at_word(Parser *p, const char *word) {
  size_t n = strlen(word);
  if (strncmp(p->src + p->pos, word, n) != 0) return false;
  {
    char c = p->src[p->pos + n];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_')
      return false;
  }
  return true;
}

/* --- 列表 ----------------------------------------------------------------- */

static bool list_push(Parser *p, InstList *list, const L1Inst *inst) {
  if (list->count == list->cap) {
    uint32_t cap = list->cap ? list->cap * 2 : 8;
    const L1Inst **grown =
        (const L1Inst **)realloc((void *)list->items, sizeof(*grown) * cap);
    if (!grown) return fail(p, 3002, "out of memory");
    list->items = grown;
    list->cap = cap;
  }
  list->items[list->count++] = inst;
  return true;
}

/* --- 类型 ----------------------------------------------------------------- */

static bool parse_type(Parser *p, const L1Type **out) {
  if (!match(p, '#')) return fail(p, 3003, "expected a type");
  if (at_word(p, "addr")) {
    p->pos += 4;
    p->column += 4;
    *out = lainir_type(p->builder, TY_ADDR, 0);
    return true;
  }
  {
    L1TypeKind kind;
    uint64_t width = 0;
    if (at_word(p, "bits")) {
      kind = TY_BITS;
      p->pos += 4;
      p->column += 4;
    } else if (at_word(p, "f")) {
      kind = TY_FLOATS;
      p->pos += 1;
      p->column += 1;
    } else if (at_word(p, "vec")) {
      kind = TY_VEC;
      p->pos += 3;
      p->column += 3;
    } else {
      return fail(p, 3003, "unknown physical type");
    }
    if (!expect(p, '<', "after the type name")) return false;
    if (!integer(p, &width)) return false;
    if (!expect(p, '>', "after the width")) return false;
    *out = lainir_type(p->builder, kind, (uint32_t)width);
  }
  return true;
}

/* --- 操作数 --------------------------------------------------------------- */

static bool parse_instruction_into(Parser *p, InstList *into, bool *terminated,
                                   bool require_result);

/* 操作数：值、整数、浮点，或者**嵌套的指令**（输入糖，展平成一条指令）。 */
static bool parse_operand(Parser *p, L1Operand *out, InstList *into,
                          const L1Type *ty) {
  skip_ws(p);
  memset(out, 0, sizeof(*out));
  if (cur(p) == '%') {
    char name[128];
    if (!value_name(p, name, sizeof(name))) return false;
    out->kind = OPERAND_VALUE;
    out->name = lainir_builder_string(p->builder, name);
    return true;
  }
  if (cur(p) == '#') {
    /* 糖：把这条指令先放出去，操作数变成它的结果。 */
    uint32_t before;
    const L1Inst *hoisted;
    if (!into)
      return fail(p, 3005, "a nested instruction is not allowed here");
    before = into->count;
    if (!parse_instruction_into(p, into, NULL, true)) return false;
    if (into->count != before + 1)
      return fail(p, 3005, "a nested instruction must be a single instruction");
    hoisted = into->items[into->count - 1];
    if (hoisted->result_count != 1)
      return fail(p, 3005, "a nested instruction must produce exactly one value");
    out->kind = OPERAND_VALUE;
    out->name = hoisted->results[0];
    return true;
  }
  if ((cur(p) >= '0' && cur(p) <= '9') ||
      (cur(p) == '-' && at(p, 1) >= '0' && at(p, 1) <= '9')) {
    bool negative = match(p, '-');
    uint64_t bits = 0;
    if (!integer(p, &bits)) return false;
    if (cur(p) == '.') {
      /* 浮点字面量：按指令的类型实参决定格式 */
      char buffer[64];
      uint32_t n = 0;
      uint32_t width = ty ? ty->width : 64;
      while (n + 1 < sizeof(buffer) &&
             ((cur(p) >= '0' && cur(p) <= '9') || cur(p) == '.' ||
              cur(p) == 'e' || cur(p) == 'E' || cur(p) == '+' ||
              cur(p) == '-')) {
        buffer[n++] = cur(p);
        advance(p);
      }
      buffer[n] = '\0';
      if (width == 32) {
        float f = (float)strtod(buffer, NULL);
        uint32_t raw;
        memcpy(&raw, &f, sizeof(raw));
        out->kind = OPERAND_FLOAT;
        out->bits = (uint64_t)raw;
      } else {
        double d = strtod(buffer, NULL);
        uint64_t raw;
        memcpy(&raw, &d, sizeof(raw));
        out->kind = OPERAND_FLOAT;
        out->bits = raw;
      }
    } else {
      out->kind = OPERAND_INT;
      out->bits = bits;
    }
    if (negative) out->bits = (uint64_t)(-(int64_t)out->bits);
    return true;
  }
  return fail(p, 3001, "expected an operand");
}

/* 整数形式的操作数也要能当「地址宽度的量」用（scale / offset）。 */
static bool parse_operand_list_into(Parser *p, InstList *into, L1Operand *out,
                                    uint32_t cap, uint32_t *count,
                                    const L1Type *ty) {
  uint32_t n = 0;
  if (!expect(p, '(', "to start the operand list")) return false;
  skip(p);
  if (match(p, ')')) {
    *count = 0;
    return true;
  }
  for (;;) {
    if (n >= cap) return fail(p, 3006, "too many operands");
    skip(p);
    if (!parse_operand(p, &out[n], into, ty)) return false;
    n++;
    skip(p);
    if (match(p, ',')) continue;
    break;
  }
  if (!expect(p, ')', "to end the operand list")) return false;
  *count = n;
  return true;
}

/* --- 区域 ----------------------------------------------------------------- */

static bool parse_region_results(Parser *p, const L1Type **out, uint32_t cap,
                                 uint32_t *count) {
  uint32_t n = 0;
  *count = 0;
  skip(p);
  if (cur(p) != '-' || at(p, 1) != '>') return true;
  advance(p);
  advance(p);
  skip(p);
  if (!expect(p, '(', "to start the result list")) return false;
  skip(p);
  if (match(p, ')')) return true;
  for (;;) {
    if (n >= cap) return fail(p, 3007, "too many region results");
    if (!parse_type(p, &out[n])) return false;
    n++;
    skip(p);
    if (match(p, ',')) {
      skip(p);
      continue;
    }
    break;
  }
  if (!expect(p, ')', "to end the result list")) return false;
  *count = n;
  return true;
}

static const L1Region *parse_block(Parser *p, const L1RegionParam *params,
                                  uint32_t param_count, const L1Type **results,
                                  uint32_t result_count) {
  InstList list;
  const L1Region *region;
  memset(&list, 0, sizeof(list));
  skip(p);
  if (!expect(p, '{', "to start a region")) return NULL;
  for (;;) {
    bool terminated = false;
    skip(p);
    if (p->failed) break;
    if (match(p, '}')) break;
    if (!cur(p)) {
      fail(p, 3001, "unexpected end of input inside a region");
      break;
    }
    if (!parse_instruction_into(p, &list, &terminated, false)) break;
  }
  if (p->failed) {
    free((void *)list.items);
    return NULL;
  }
  region = lainir_region(p->builder, params, param_count, results, result_count,
                         list.items, list.count);
  free((void *)list.items);
  return region;
}

/* --- 指令 ----------------------------------------------------------------- */

static bool parse_result_prefix(Parser *p, char storage[][128],
                                const char **names, uint32_t cap,
                                uint32_t *count) {
  uint32_t n = 0;
  *count = 0;
  skip(p);
  if (cur(p) != '%') return true;
  for (;;) {
    char name[128];
    if (n >= cap) return fail(p, 3008, "too many results");
    if (!value_name(p, name, sizeof(name))) return false;
    snprintf(storage[n], 128, "%s", name);
    names[n] = lainir_builder_string(p->builder, name);
    n++;
    skip(p);
    if (match(p, ',')) {
      skip(p);
      continue;
    }
    break;
  }
  skip(p);
  if (!expect(p, '=', "after the result list")) return false;
  *count = n;
  return true;
}

static bool parse_loop_params(Parser *p, L1RegionParam *out, uint32_t cap,
                              uint32_t *count) {
  uint32_t n = 0;
  *count = 0;
  if (!expect(p, '(', "to start the loop parameters")) return false;
  skip(p);
  if (match(p, ')')) return true;
  for (;;) {
    const L1Type *ty = NULL;
    L1Operand init;
    char name[128];
    if (n >= cap) return fail(p, 3009, "too many loop parameters");
    if (!value_name(p, name, sizeof(name))) return false;
    skip(p);
    if (!expect(p, ':', "after the parameter name")) return false;
    skip(p);
    if (!parse_type(p, &ty)) return false;
    skip(p);
    if (!expect(p, '=', "after the parameter type")) return false;
    skip(p);
    memset(&init, 0, sizeof(init));
    if (!parse_operand(p, &init, NULL, ty)) return false;
    out[n].param.name = lainir_builder_string(p->builder, name);
    out[n].param.ty = ty;
    out[n].init = init;
    n++;
    skip(p);
    if (match(p, ',')) {
      skip(p);
      continue;
    }
    break;
  }
  if (!expect(p, ')', "to end the loop parameters")) return false;
  *count = n;
  return true;
}

/* 把结果名塞进一条已经建好的指令。 */
static void set_results(const L1Inst *built, const char **names, uint32_t count) {
  L1Inst *inst = (L1Inst *)built;
  uint32_t i;
  inst->result_count = count > L1_MAX_RESULTS ? L1_MAX_RESULTS : count;
  for (i = 0; i < inst->result_count; i++) inst->results[i] = names[i];
}

static bool parse_operands_and_finish(Parser *p, InstList *into, L1InstKind kind,
                                      const char **names, uint32_t name_count) {
  L1Operand ops[L1P_MAX_PARAMS];
  uint32_t count = 0;
  const L1Type *ty = NULL;
  L1MemOrder order = ORDER_RELAXED;
  bool is_volatile = false;
  const char *symbol = NULL;
  L1Inst *inst;

  skip(p);
  if (kind == INST_CALL) {
    char callee[128];
    if (!ident(p, callee, sizeof(callee))) return false;
    symbol = lainir_builder_string(p->builder, callee);
  } else if (kind == INST_DATA_ADDR || kind == INST_PROC_ADDR) {
    char name[128];
    if (!symbol_name(p, name, sizeof(name))) return false;
    symbol = lainir_builder_string(p->builder, name);
    ops[0] = lainir_int(0);
    count = 0;
  }

  /* 其他算子：可选 [TYPE, attrs] 然后 (operands) */
  skip(p);
  if (kind != INST_CALL && kind != INST_DATA_ADDR && kind != INST_PROC_ADDR) {
    if (match(p, '[')) {
      if (!parse_type(p, &ty)) return false;
      skip(p);
      while (match(p, ',')) {
        skip(p);
        if (at_word(p, "volatile")) {
          p->pos += 8;
          p->column += 8;
          is_volatile = true;
        } else if (at_word(p, "acquire")) {
          p->pos += 7;
          p->column += 7;
          order = ORDER_ACQUIRE;
        } else if (at_word(p, "release")) {
          p->pos += 7;
          p->column += 7;
          order = ORDER_RELEASE;
        } else if (at_word(p, "acq_rel")) {
          p->pos += 7;
          p->column += 7;
          order = ORDER_ACQ_REL;
        } else if (at_word(p, "seq_cst")) {
          p->pos += 7;
          p->column += 7;
          order = ORDER_SEQ_CST;
        } else if (at_word(p, "relaxed")) {
          p->pos += 7;
          p->column += 7;
          order = ORDER_RELAXED;
        } else {
          return fail(p, 3001, "unknown instruction attribute");
        }
        skip(p);
      }
      if (!expect(p, ']', "to end the type argument")) return false;
      skip(p);
    }
    if (!parse_operand_list_into(p, into, ops, L1P_MAX_PARAMS, &count,
                                 ty ? ty : NULL))
      return false;
  } else if (kind == INST_CALL) {
    if (!parse_operand_list_into(p, into, ops, L1P_MAX_PARAMS, &count, NULL))
      return false;
  }

  switch (kind) {
  case INST_STORE:
    inst = (L1Inst *)lainir_inst_mem(p->builder, kind, NULL, ty, order,
                                     is_volatile, ops, count);
    break;
  case INST_LOAD:
  case INST_ALLOCA:
  case INST_INT2PTR:
  case INST_PTR2INT:
    inst = (L1Inst *)lainir_inst_mem(p->builder, kind,
                                     name_count ? names[0] : NULL, ty, order,
                                     is_volatile, ops, count);
    break;
  case INST_CALL:
    inst = (L1Inst *)lainir_inst_call(p->builder,
                                      name_count ? names[0] : NULL, symbol, ops,
                                      count);
    break;
  case INST_DATA_ADDR:
  case INST_PROC_ADDR:
    inst = (L1Inst *)lainir_inst_symbol(p->builder, kind,
                                        name_count ? names[0] : NULL, symbol);
    break;
  default:
    inst = (L1Inst *)lainir_inst(p->builder, kind,
                                 name_count ? names[0] : NULL, ty, ops, count);
    break;
  }
  if (!inst) return fail(p, 3002, "cannot build the instruction");
  set_results(inst, names, name_count);
  if (is_volatile) inst->is_volatile = true;
  inst->order = order;
  inst->line = p->line;
  inst->column = p->column;
  return list_push(p, into, inst);
}

static bool parse_instruction_into(Parser *p, InstList *into, bool *terminated,
                                   bool require_result) {
  char name_storage[L1P_MAX_RESULTS][128];
  const char *names[L1P_MAX_RESULTS];
  uint32_t name_count = 0;
  char opname[64];
  L1InstKind kind;
  uint32_t n = 0;

  if (terminated) *terminated = false;
  skip(p);
  if (!parse_result_prefix(p, name_storage, names, L1P_MAX_RESULTS,
                           &name_count))
    return false;
  if (require_result && name_count == 0) {
    /* 糖展平出来的指令得有个名字，否则它的值没人能引用。 */
    snprintf(name_storage[0], sizeof(name_storage[0]), "_tmp%u", p->temp_seq++);
    names[0] = lainir_builder_string(p->builder, name_storage[0]);
    name_count = 1;
  }
  skip(p);
  if (!match(p, '#')) return fail(p, 3001, "expected an instruction");
  while (n + 1 < sizeof(opname)) {
    char c = cur(p);
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) break;
    opname[n++] = c;
    advance(p);
  }
  opname[n] = '\0';
  if (!n) return fail(p, 3002, "expected an opcode after `#`");
  if (!lainir_opcode_kind(opname, n, &kind))
    return fail(p, 3002, "unknown opcode `#%s`", opname);

  /* 向量算子的车道后缀（vadd.i32x4）还没实现——引擎和后端也都没有。 */
  if (cur(p) == '.')
    return fail(p, 3010, "vector lane suffixes are not supported yet");

  switch (kind) {
  case INST_IF: {
    L1Operand cond;
    const L1Type *results[L1P_MAX_RESULTS];
    uint32_t result_count = 0;
    const L1Region *then_body;
    const L1Region *else_body = NULL;
    L1Inst *inst;
    skip(p);
    memset(&cond, 0, sizeof(cond));
    if (!parse_operand(p, &cond, into, NULL)) return false;
    if (!parse_region_results(p, results, L1P_MAX_RESULTS, &result_count))
      return false;
    then_body = parse_block(p, NULL, 0, results, result_count);
    if (!then_body) return false;
    skip(p);
    if (at_word(p, "else")) {
      p->pos += 4;
      p->column += 4;
      else_body = parse_block(p, NULL, 0, results, result_count);
      if (!else_body) return false;
    }
    inst = (L1Inst *)lainir_inst_if(p->builder,
                                    name_count ? names[0] : NULL, cond, then_body,
                                    else_body);
    if (!inst) return fail(p, 3002, "cannot build #if");
    set_results(inst, names, name_count);
    return list_push(p, into, inst);
  }

  case INST_LOOP: {
    L1RegionParam params[L1P_MAX_PARAMS];
    uint32_t param_count = 0;
    const L1Type *results[L1P_MAX_RESULTS];
    uint32_t result_count = 0;
    const L1Region *body;
    L1Inst *inst;
    char label[128];
    skip(p);
    if (!ident(p, label, sizeof(label))) return false;
    if (!parse_loop_params(p, params, L1P_MAX_PARAMS, &param_count))
      return false;
    if (!parse_region_results(p, results, L1P_MAX_RESULTS, &result_count))
      return false;
    body = parse_block(p, params, param_count, results, result_count);
    if (!body) return false;
    inst = (L1Inst *)lainir_inst_loop(p->builder,
                                      name_count ? names[0] : NULL,
                                      lainir_builder_string(p->builder, label),
                                      body);
    if (!inst) return fail(p, 3002, "cannot build #loop");
    set_results(inst, names, name_count);
    return list_push(p, into, inst);
  }

  case INST_SWITCH:
    return fail(p, 3010, "#switch is not supported yet");

  case INST_BREAK:
  case INST_CONTINUE: {
    char label[128];
    L1Operand ops[L1P_MAX_PARAMS];
    uint32_t count = 0;
    L1Inst *inst;
    skip(p);
    if (!ident(p, label, sizeof(label))) return false;
    skip(p);
    if (cur(p) == '(') {
      if (!parse_operand_list_into(p, into, ops, L1P_MAX_PARAMS, &count, NULL))
        return false;
    }
    inst = (L1Inst *)lainir_inst_jump(p->builder, kind,
                                      lainir_builder_string(p->builder, label),
                                      ops, count);
    if (!inst) return fail(p, 3002, "cannot build the jump");
    if (terminated) *terminated = true;
    return list_push(p, into, inst);
  }

  case INST_YIELD:
  case INST_RETURN: {
    L1Operand ops[L1P_MAX_PARAMS];
    uint32_t count = 0;
    L1Inst *inst;
    bool saved = p->inline_only;
    p->inline_only = true;
    skip_inline(p);
    while (cur(p) && cur(p) != '\n' && cur(p) != '}') {
      if (count >= L1P_MAX_PARAMS) {
        p->inline_only = saved;
        return fail(p, 3006, "too many operands");
      }
      if (!parse_operand(p, &ops[count], into, NULL)) {
        p->inline_only = saved;
        return false;
      }
      count++;
      skip_inline(p);
      if (match(p, ',')) {
        skip_inline(p);
        continue;
      }
      break;
    }
    p->inline_only = saved;
    inst = (L1Inst *)lainir_inst(p->builder, kind, NULL, NULL, ops, count);
    if (!inst) return fail(p, 3002, "cannot build the terminator");
    if (terminated) *terminated = true;
    return list_push(p, into, inst);
  }

  default:
    return parse_operands_and_finish(p, into, kind, names, name_count);
  }
}

/* --- 顶层 ----------------------------------------------------------------- */

static bool parse_proc(Parser *p, L1Subroutine *out) {
  char name[128];
  L1Param params[L1P_MAX_PARAMS];
  uint32_t param_count = 0;
  const L1Type *results[L1P_MAX_RESULTS];
  uint32_t result_count = 0;
  bool is_extern = false;
  const char *link_name = NULL;
  const L1Region *body = NULL;

  if (!ident(p, name, sizeof(name))) return false;
  skip(p);
  if (!expect(p, '(', "to start the parameter list")) return false;
  skip(p);
  if (!match(p, ')')) {
    for (;;) {
      char pname[128];
      const L1Type *ty = NULL;
      if (param_count >= L1P_MAX_PARAMS)
        return fail(p, 3011, "too many parameters");
      if (!value_name(p, pname, sizeof(pname))) return false;
      skip(p);
      if (!expect(p, ':', "after the parameter name")) return false;
      skip(p);
      if (!parse_type(p, &ty)) return false;
      params[param_count].name = lainir_builder_string(p->builder, pname);
      params[param_count].ty = ty;
      param_count++;
      skip(p);
      if (match(p, ',')) {
        skip(p);
        continue;
      }
      break;
    }
    if (!expect(p, ')', "to end the parameter list")) return false;
  }
  skip(p);
  if (cur(p) == '-' && at(p, 1) == '>') {
    uint32_t i;
    advance(p);
    advance(p);
    skip(p);
    if (cur(p) == '(') {
      if (!parse_region_results(p, results, L1P_MAX_RESULTS, &result_count))
        return false;
    } else {
      if (!parse_type(p, &results[0])) return false;
      result_count = 1;
    }
    for (i = 0; i < result_count; i++) {
      if (!results[i]) return fail(p, 3003, "bad result type");
    }
  }

  skip(p);
  if (match(p, '#')) {
    char word[16];
    uint32_t n = 0;
    while (n + 1 < sizeof(word)) {
      char c = cur(p);
      if (!((c >= 'a' && c <= 'z') || c == '_')) break;
      word[n++] = c;
      advance(p);
    }
    word[n] = '\0';
    if (strcmp(word, "extern") != 0)
      return fail(p, 3002, "expected `#extern` or a body");
    is_extern = true;
    skip(p);
    if (match(p, '"')) {
      char symbol[128];
      uint32_t k = 0;
      while (cur(p) && cur(p) != '"' && k + 1 < sizeof(symbol)) {
        symbol[k++] = cur(p);
        advance(p);
      }
      symbol[k] = '\0';
      if (!expect(p, '"', "to end the extern symbol")) return false;
      link_name = lainir_builder_string(p->builder, symbol);
    }
  } else {
    body = parse_block(p, NULL, 0, NULL, 0);
    if (!body) return false;
  }

  if (is_extern) {
    *out = *lainir_subroutine_extern(p->builder,
                                     lainir_builder_string(p->builder, name),
                                     link_name ? link_name
                                               : lainir_builder_string(p->builder, name),
                                     params, param_count, results, result_count);
  } else {
    *out = *lainir_subroutine(p->builder,
                              lainir_builder_string(p->builder, name), params,
                              param_count, results, result_count, body);
  }
  return true;
}

static bool parse_data(Parser *p, L1Data *out) {
  char symbol[128];
  bool writable = false;
  /* 字节也按需增长：单个数组成员数同样只该来自模块本身。 */
  uint8_t *bytes = NULL;
  uint32_t size = 0;
  uint32_t byte_cap = 0;

  if (!symbol_name(p, symbol, sizeof(symbol))) return false;
  skip(p);
  if (at_word(p, "rw")) {
    p->pos += 2;
    p->column += 2;
    writable = true;
  } else if (at_word(p, "ro")) {
    p->pos += 2;
    p->column += 2;
  } else {
    return fail(p, 3001, "expected `ro` or `rw`");
  }
  skip(p);
  if (!expect(p, '{', "to start the data bytes")) return false;
  for (;;) {
    uint64_t byte = 0;
    skip(p);
    if (match(p, '}')) break;
    if (!integer(p, &byte)) return false;
    if (!grow_table(p->builder, (void **)&bytes, &byte_cap, size, 1u)) {
      return fail(p, 3012, "out of memory for the data object");
    }
    bytes[size++] = (uint8_t)byte;
  }
  *out = *lainir_data(p->builder, lainir_builder_string(p->builder, symbol),
                      bytes, size, writable);
  return true;
}

const L1Module *lainir_parse(L1Builder *builder, const char *text,
                             L1Diagnostic *diag) {
  Parser p;
  /* 过程表和数据表按需增长：**上限只该来自模块本身**。
   * 这两张表曾经是 data[64] / subs[64]，一个 852 过程的模块直接编不了。
   * 旧块留在 arena 里不回收——这是宿主侧工具，而且 lainir_module 随后会
   * 拷成精确大小的一份。 */
  L1Data *data = NULL;
  L1Subroutine *subs = NULL;
  uint32_t data_count = 0;
  uint32_t data_cap = 0;
  uint32_t sub_count = 0;
  uint32_t sub_cap = 0;

  if (!builder || !text) return NULL;
  memset(&p, 0, sizeof(p));
  p.builder = builder;
  p.src = text;
  p.line = 1;
  p.column = 1;
  p.diag = diag;
  if (diag) {
    diag->code = 0;
    diag->message[0] = '\0';
  }

  for (;;) {
    skip(&p);
    if (p.failed) return NULL;
    if (!cur(&p)) break;
    if (at_word(&p, "data")) {
      p.pos += 4;
      p.column += 4;
      skip(&p);
      if (!grow_table(builder, (void **)&data, &data_cap, data_count,
                      sizeof(L1Data))) {
        fail(&p, 3013, "out of memory for data objects");
        return NULL;
      }
      if (!parse_data(&p, &data[data_count])) return NULL;
      data_count++;
      continue;
    }
    if (at_word(&p, "#proc")) {
      p.pos += 5;
      p.column += 5;
      skip(&p);
      if (!grow_table(builder, (void **)&subs, &sub_cap, sub_count,
                      sizeof(L1Subroutine))) {
        fail(&p, 3014, "out of memory for subroutines");
        return NULL;
      }
      if (!parse_proc(&p, &subs[sub_count])) return NULL;
      sub_count++;
      continue;
    }
    fail(&p, 3001, "expected `#proc` or `data`");
    return NULL;
  }

  return lainir_module(builder, "module", data, data_count, subs, sub_count);
}
