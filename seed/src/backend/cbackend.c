/* C 后端：把 LAINIR artifact 编译成 C。
 *
 * 这一版刻意**不做寄存器分配**：每个值在函数里占一个槽，用 `l1v r[N]` 表示，
 * 布局和 VM 的值槽一模一样。于是这个后端的活只剩三件——**合法化、控制流、ABI**，
 * 而 clang 会替我们做分配。
 *
 * 不变量：**每个槽里存的值都已经按它自己的宽度掩码过**。所以操作数只要按
 * 运算宽度掩码（窄值再掩一次是空操作），只有 #sext 需要知道源宽度。
 *
 * 域外行为**不发检查**。除零、移位越界、越权地址在这里是 C 的 UB——这是
 * emit.h 里那条待定项（检查由谁消去）尚未解决的表现，所以差分测试只喂
 * 「在定义域内」的程序。
 */
#include "lainbackend/emit.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lainir/infer.h"

typedef struct {
  const L1Region *region;
  const L1Region *parent;
  uint32_t base;
} RegionBase;

typedef struct LoopCtx {
  const char *label;
  uint32_t id;
  const L1Region *region;
  const L1Region *parent;
  uint32_t parent_pos;
  const struct LoopCtx *outer;
} LoopCtx;

struct LainBackend {
  const LainTarget *target;
  const LainBackendSink *sink;
  L1Diagnostic *diag;
  const L1Module *module;
  const L1Subroutine *sub;
  LainIrTypes types;
  /* 两张表都按模块自己的计数在 emit 开始时分配，之后不变。
   * 这里曾经是 regions[128] / widths[512]：槽数是**整个模块**的扁平计数，
   * 512 对一个真编译器远远不够，而越界时 set_width 悄悄丢掉、slot_width
   * 悄悄按 64 位算——那是静默错编，不是拒绝。 */
  RegionBase *regions;
  uint32_t region_count;
  uint32_t region_cap;
  uint8_t *widths;
  uint32_t slot_cap;
  uint32_t next_slot;
  /* #eval 检查用的工作表；容量取区域总数这个安全上界。 */
  const L1Region **worklist;
  uint32_t worklist_cap;
  uint32_t label_seq;
  uint32_t indent;
  bool failed;
};

/* --- 输出 ----------------------------------------------------------------- */

static void emit_text(LainBackend *be, const char *text) {
  if (!be->sink || !be->sink->write) return;
  be->sink->write(be->sink->user, text, (uint32_t)strlen(text));
}

static void emitf(LainBackend *be, const char *fmt, ...) {
  char buffer[512];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);
  emit_text(be, buffer);
}

static void emit_indent(LainBackend *be) {
  uint32_t i;
  for (i = 0; i < be->indent; i++) emit_text(be, "  ");
}

static void fail(LainBackend *be, int code, const char *message) {
  if (be->failed) return;
  be->failed = true;
  if (!be->diag) return;
  be->diag->code = code;
  be->diag->line = 0;
  be->diag->column = 0;
  snprintf(be->diag->message, sizeof(be->diag->message), "%s", message);
}

static void mangle(char *out, size_t cap, const char *name, const char *prefix) {
  size_t at = 0;
  size_t i;
  for (i = 0; prefix && prefix[i] && at + 1 < cap; i++) out[at++] = prefix[i];
  for (i = 0; name && name[i] && at + 1 < cap; i++) {
    char c = name[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_')
      out[at++] = c;
    else
      out[at++] = '_';
  }
  out[at] = '\0';
}

/* --- 槽布局与宽度 --------------------------------------------------------- */

static uint32_t region_base(const LainBackend *be, const L1Region *region) {
  uint32_t i;
  for (i = 0; i < be->region_count; i++) {
    if (be->regions[i].region == region) return be->regions[i].base;
  }
  return 0;
}

static const L1Region *region_parent(const LainBackend *be,
                                     const L1Region *region) {
  uint32_t i;
  for (i = 0; i < be->region_count; i++) {
    if (be->regions[i].region == region) return be->regions[i].parent;
  }
  return NULL;
}

static void set_width(LainBackend *be, uint32_t slot, uint32_t width) {
  if (slot >= be->slot_cap) {
    /* 容量是按模块数出来的，越界就是数漏了——不能静默按 64 位算。 */
    fail(be, 9226, "cbackend: slot layout does not match the module");
    return;
  }
  be->widths[slot] = (uint8_t)(width ? width : 64);
}

static uint32_t slot_width(const LainBackend *be, uint32_t slot) {
  if (slot >= be->slot_cap) return 64;
  return be->widths[slot] ? be->widths[slot] : 64;
}

/* 量尺寸：区域总数、扁平槽总数、区域嵌套深度。
 * 遍历顺序必须和 assign_regions 一致（loop 不看 else_body），
 * 否则数出来的槽数和实际用的对不上。 */
static void measure_regions(LainBackend *be, const L1Region *region,
                            uint32_t depth) {
  uint32_t pos;
  if (!region) return;
  be->region_cap++;
  be->next_slot += region->param_count;
  for (pos = 0; pos < region->inst_count; pos++) {
    const L1Inst *inst = &region->insts[pos];
    uint32_t k;
    be->next_slot += inst->result_count;
    if (inst->kind != INST_LOOP)
      measure_regions(be, inst->else_body, depth + 1);
    measure_regions(be, inst->body, depth + 1);
    measure_regions(be, inst->default_case, depth + 1);
    /* #switch 的分支体也要算：漏了它们，槽位就会和别的区域重叠。 */
    for (k = 0; k < inst->case_count; k++)
      measure_regions(be, inst->cases[k].body, depth + 1);
  }
}

/* 按模块量一次尺寸并分配。emit 一开始就调用。
 *
 * 槽号是**每个过程内部**从 0 重排的（emit_subroutine 会重置 next_slot），
 * 所以取各过程的最大值，不是全模块求和。 */
static bool size_backend(LainBackend *be, const L1Module *module) {
  uint32_t i;
  uint32_t max_regions = 0;
  uint32_t max_slots = 0;

  for (i = 0; i < module->subroutine_count; i++) {
    const L1Subroutine *sub = &module->subroutines[i];
    if ((sub->flags & SUBROUTINE_EXTERN) || !sub->body) continue;
    be->region_cap = 0;
    be->next_slot = sub->param_count;
    measure_regions(be, sub->body, 1);
    if (be->region_cap > max_regions) max_regions = be->region_cap;
    if (be->next_slot > max_slots) max_slots = be->next_slot;
  }

  be->region_cap = max_regions;
  be->slot_cap = max_slots;
  be->regions = (RegionBase *)calloc(max_regions ? max_regions : 1u,
                                     sizeof(RegionBase));
  be->widths = (uint8_t *)calloc(max_slots ? max_slots : 1u, 1u);
  be->worklist = (const L1Region **)calloc(max_regions ? max_regions : 1u,
                                           sizeof(const L1Region *));
  be->worklist_cap = max_regions;
  if (!be->regions || !be->widths || !be->worklist) {
    fail(be, 9227, "cbackend: cannot allocate the layout tables");
    return false;
  }
  be->region_count = 0;
  be->next_slot = 0;
  return true;
}
/* 一个物理类型占多少位。#addr 的 width 无意义，按指针宽度算。 */
static uint32_t width_of_type(const L1Type *ty);

static const L1Subroutine *find_sub(const L1Module *module, const char *name) {
  uint32_t i;
  for (i = 0; i < module->subroutine_count; i++) {
    const L1Subroutine *sub = &module->subroutines[i];
    if (sub->name && name && strcmp(sub->name, name) == 0) return sub;
  }
  return NULL;
}

/* 一条指令产出的值有多宽。
 * 规则本身在 lainir/infer.c 里，只写一遍——验证器和后端必须用同一份。 */
static uint32_t inst_result_width(LainBackend *be, const L1Inst *inst) {
  const L1Type *ty = lainir_inst_result_type(&be->types, inst, 0);
  return ty ? width_of_type(ty) : 64;
}

static void assign_regions(LainBackend *be, const L1Region *region,
                           const L1Region *parent) {
  uint32_t pos;
  uint32_t i;
  if (!region) return;
  if (be->region_count >= be->region_cap) {
    char buffer[192];
    snprintf(buffer, sizeof(buffer),
             "cbackend: region layout does not match the module "
             "(%u regions, capacity %u)",
             be->region_count, be->region_cap);
    fail(be, 9226, buffer);
    return;
  }
  be->regions[be->region_count].region = region;
  be->regions[be->region_count].parent = parent;
  be->regions[be->region_count].base = be->next_slot;
  be->region_count++;
  for (i = 0; i < region->param_count; i++)
    set_width(be, be->next_slot + i, width_of_type(region->params[i].param.ty));
  be->next_slot += region->param_count;
  for (pos = 0; pos < region->inst_count; pos++) {
    const L1Inst *inst = &region->insts[pos];
    uint32_t k;
    for (k = 0; k < inst->result_count; k++)
      set_width(be, be->next_slot + k, inst_result_width(be, inst));
    be->next_slot += inst->result_count;
  }
  for (pos = 0; pos < region->inst_count; pos++) {
    const L1Inst *inst = &region->insts[pos];
    uint32_t k;
    if (inst->kind != INST_LOOP) assign_regions(be, inst->else_body, region);
    assign_regions(be, inst->body, region);
    assign_regions(be, inst->default_case, region);
    /* 和 measure_regions 必须逐项对上：#switch 的分支体也占槽。 */
    for (k = 0; k < inst->case_count; k++)
      assign_regions(be, inst->cases[k].body, region);
  }
}

static uint32_t slot_of(const LainBackend *be, const L1Region *region,
                        uint32_t position, uint32_t index) {
  uint32_t slot = region_base(be, region) + region->param_count;
  uint32_t q;
  for (q = 0; q < position; q++) slot += region->insts[q].result_count;
  return slot + index;
}

static bool resolve_slot(const LainBackend *be, const L1Region *region,
                         const char *name, uint32_t *slot_out) {
  const L1Region *r = region;
  while (r) {
    uint32_t base = region_base(be, r);
    uint32_t i;
    uint32_t slot = base + r->param_count;
    if (r == be->sub->body) {
      for (i = 0; i < be->sub->param_count; i++) {
        if (be->sub->params[i].name && name &&
            strcmp(be->sub->params[i].name, name) == 0) {
          *slot_out = i;
          return true;
        }
      }
    }
    for (i = 0; i < r->param_count; i++) {
      if (r->params[i].param.name && name &&
          strcmp(r->params[i].param.name, name) == 0) {
        *slot_out = base + i;
        return true;
      }
    }
    for (i = 0; i < r->inst_count; i++) {
      const L1Inst *inst = &r->insts[i];
      uint32_t k;
      for (k = 0; k < inst->result_count; k++) {
        if (inst->results[k] && name && strcmp(inst->results[k], name) == 0) {
          *slot_out = slot + k;
          return true;
        }
      }
      slot += inst->result_count;
    }
    r = region_parent(be, r);
  }
  return false;
}

/* --- 表达式 --------------------------------------------------------------- */

static const char *mem_type(uint32_t width) {
  if (width <= 8) return "uint8_t";
  if (width <= 16) return "uint16_t";
  if (width <= 32) return "uint32_t";
  return "uint64_t";
}

/* 一个物理类型占多少位。#addr 的 width 无意义，按指针宽度算。 */
static uint32_t width_of_type(const L1Type *ty) {
  if (!ty) return 64;
  if (ty->kind == TY_ADDR) return 64;
  return ty->width ? ty->width : 64;
}

static void operand_expr(LainBackend *be, const L1Region *region,
                         const L1Inst *inst, uint32_t index, char *out,
                         size_t cap) {
  const L1Operand *op = &inst->operands[index];
  if (op->kind == OPERAND_VALUE) {
    uint32_t slot = 0;
    if (!resolve_slot(be, region, op->name, &slot)) {
      fail(be, 9202, "cbackend: undefined value");
      snprintf(out, cap, "0");
      return;
    }
    snprintf(out, cap, "r[%u]", slot);
  } else {
    snprintf(out, cap, "(l1v)%llu", (unsigned long long)op->bits);
  }
}

static void operand_masked(LainBackend *be, const L1Region *region,
                           const L1Inst *inst, uint32_t index, uint32_t width,
                           char *out, size_t cap) {
  char raw[192];
  operand_expr(be, region, inst, index, raw, sizeof(raw));
  snprintf(out, cap, "(%s & l1_mask(%u))", raw, width);
}

/* 操作数自己的宽度：字面量看指令的类型实参，值看槽宽度表。 */
static uint32_t operand_width(LainBackend *be, const L1Region *region,
                              const L1Inst *inst, uint32_t index) {
  const L1Operand *op = &inst->operands[index];
  if (op->kind != OPERAND_VALUE)
    return inst->has_ty && inst->ty ? inst->ty->width : 64;
  {
    uint32_t slot = 0;
    if (!resolve_slot(be, region, op->name, &slot)) return 64;
    return slot_width(be, slot);
  }
}

/* --- 前导 ----------------------------------------------------------------- */

static void emit_prologue(LainBackend *be) {
  emit_text(be,
            "/* 由 LAINIR 的 C 后端生成。不要手改。 */\n"
            "#include <stdint.h>\n"
            "#include <string.h>\n"
            "#include <stdlib.h>\n"
            "#include <stdio.h>\n"
            "#include <limits.h>\n"
            "\n"
            "typedef uint64_t l1v;\n"
            "\n"
            "/* 域外行为的物化：VM 那边是记 trap 后拒绝，编译产物里没有那条通道，\n"
            " * 所以只能终止。谁把它消去（证明不会发生）是后端接口里那条待定项。 */\n"
            "void l1_abort(uint32_t status) {\n"
            "  fprintf(stderr, \"LAINIR trap %u\\n\", (unsigned)status);\n"
            "  abort();\n"
            "}\n"
            "\n"
            "/* 值的低 W 位有效；W >= 64 时 64 位都是真值。 */\n"
            "l1v l1_mask(unsigned w) {\n"
            "  return w >= 64 ? ~(l1v)0 : ((((l1v)1) << w) - 1);\n"
            "}\n"
            "int64_t l1_s64(l1v v, unsigned w) {\n"
            "  return (int64_t)(v << (64 - w)) >> (64 - w);\n"
            "}\n"
            "double l1_f64_of(l1v v) { double d; memcpy(&d, &v, 8); return d; }\n"
            "l1v l1_f64_bits(double d) { l1v v; memcpy(&v, &d, 8); return v; }\n"
            "float l1_f32_of(l1v v) { float f; uint32_t u = (uint32_t)v; memcpy(&f, &u, 4); return f; }\n"
            "l1v l1_f32_bits(float f) { uint32_t u; memcpy(&u, &f, 4); return (l1v)u; }\n"
            "\n"
            "/* 间接调用：地址换回子过程。 */\n"
            "l1v l1_indirect(l1v target, const l1v *a, uint32_t n);\n"
            "\n");
}

/* 外部符号名必须是一个合法的 C 标识符——因为解释器按同一个名字查能力表，
 * 后端按同一个名字发符号引用，**一个键两种兑现方式，名字不能有两份**。 */
static bool is_c_identifier(const char *s) {
  size_t i;
  if (!s || !s[0]) return false;
  if (!((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z') ||
        s[0] == '_'))
    return false;
  for (i = 1; s[i]; i++) {
    if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
          (s[i] >= '0' && s[i] <= '9') || s[i] == '_'))
      return false;
  }
  return true;
}

static const char *extern_symbol(const L1Subroutine *sub) {
  return sub->link_name ? sub->link_name : sub->name;
}

/* 每个 extern 子过程一条声明，并把符号报给 sink。
 * ABI 与 lainvm/caps.h 的 LainVmHostFn 逐字相同——宿主只写一个函数。 */
static void emit_externs(LainBackend *be) {
  uint32_t i;
  for (i = 0; i < be->module->subroutine_count; i++) {
    const L1Subroutine *sub = &be->module->subroutines[i];
    const char *symbol;
    if (!(sub->flags & SUBROUTINE_EXTERN)) continue;
    symbol = extern_symbol(sub);
    if (!is_c_identifier(symbol)) {
      fail(be, 9226,
           "cbackend: external symbol name is not a C identifier "
           "(the capability key and the link symbol must be one name)");
      return;
    }
    emitf(be,
          "/* 宿主能力：由嵌入者提供。能力名 = 外部符号名。 */\n"
          "extern uint32_t %s(const uint64_t *args, uint32_t arg_count, "
          "uint64_t *result);\n",
          symbol);
    if (be->sink && be->sink->symbol)
      be->sink->symbol(be->sink->user, symbol, true);
  }
  emit_text(be, "\n");
}

static void emit_data(LainBackend *be) {
  uint32_t i;
  for (i = 0; i < be->module->data_count; i++) {
    const L1Data *data = &be->module->data[i];
    char name[128];
    uint32_t k;
    mangle(name, sizeof(name), data->symbol, "l1_data_");
    emitf(be, "static %suint8_t %s[%u] = {", data->is_writable ? "" : "const ",
          name, data->size ? data->size : 1);
    for (k = 0; k < data->size; k++)
      emitf(be, "%s%u", k ? ", " : "", data->bytes ? data->bytes[k] : 0);
    emit_text(be, "};\n");
    if (be->sink && be->sink->symbol)
      be->sink->symbol(be->sink->user, data->symbol, false);
  }
  if (be->module->data_count) {
    /* 让外面拿得到数据对象的地址，和 VM 的符号表对应。 */
    emit_text(be, "l1v l1_data_addr(uint32_t index) {\n  switch (index) {\n");
    for (i = 0; i < be->module->data_count; i++) {
      char name[128];
      mangle(name, sizeof(name), be->module->data[i].symbol, "l1_data_");
      emitf(be, "    case %uu: return (l1v)(uintptr_t)%s;\n", i, name);
    }
    emitf(be, "    default: break;\n  }\n  abort();\n  return 0;\n}\n");
    emitf(be, "uint32_t l1_data_count(void) { return %uu; }\n\n",
          be->module->data_count);
  }
}

/* --- 指令 ----------------------------------------------------------------- */

static const LoopCtx *find_loop(const LoopCtx *loops, const char *label) {
  const LoopCtx *it = loops;
  if (!label) return NULL;
  while (it) {
    if (it->label && strcmp(it->label, label) == 0) return it;
    it = it->outer;
  }
  return NULL;
}

static void emit_region(LainBackend *be, const L1Region *region,
                        const L1Region *parent, uint32_t parent_pos,
                        const LoopCtx *loops);

/* 整数算术。
 *
 * 除零、移位越界、有符号除法的溢出，在 VM 里是「域外行为 -> 拒绝」；
 * 编译产物里没有那个通道，所以**物化成检查**：违反就 abort。
 * 这是「无 UB」在编译路径上要付的账。谁消去这些检查（证明它们不会发生）
 * 是 emit.h 里那条待定项。 */
static void emit_int_arith(LainBackend *be, const L1Region *region,
                           const L1Inst *inst, uint32_t width, const char *dest) {
  char a[192], b[192];
  const char *op;
  bool signed_op = false;
  operand_masked(be, region, inst, 0, width, a, sizeof(a));
  operand_masked(be, region, inst, 1, width, b, sizeof(b));
  switch (inst->kind) {
  case INST_ADD: op = "+"; break;
  case INST_SUB: op = "-"; break;
  case INST_MUL: op = "*"; break;
  case INST_AND: op = "&"; break;
  case INST_OR: op = "|"; break;
  case INST_XOR: op = "^"; break;
  case INST_ASHR:
    emitf(be, "{ l1v l1a = %s, l1b = %s;\n", a, b);
    emitf(be, "  if (l1b >= %uu) l1_abort(1002);\n", width);
    emitf(be, "  %s = (l1v)(l1_s64(l1a, %u) >> l1b) & l1_mask(%u); }\n", dest,
          width, width);
    return;
  case INST_SHL:
  case INST_LSHR:
    emitf(be, "{ l1v l1a = %s, l1b = %s;\n", a, b);
    emitf(be, "  if (l1b >= %uu) l1_abort(1002);\n", width);
    emitf(be, "  %s = (l1a %s l1b) & l1_mask(%u); }\n",
          dest, inst->kind == INST_SHL ? "<<" : ">>", width);
    return;
  case INST_SDIV:
  case INST_UDIV:
  case INST_SREM:
  case INST_UREM:
    signed_op = inst->kind == INST_SDIV || inst->kind == INST_SREM;
    op = (inst->kind == INST_SDIV || inst->kind == INST_UDIV) ? "/" : "%";
    emitf(be, "{ l1v l1a = %s, l1b = %s;\n", a, b);
    emitf(be, "  if (l1b == 0) l1_abort(1001);\n");
    if (signed_op) {
      emitf(be,
            "  if (l1_s64(l1a, %u) == INT64_MIN && l1_s64(l1b, %u) == -1)"
            " l1_abort(1003);\n",
            width, width);
      emitf(be,
            "  %s = (l1v)(l1_s64(l1a, %u) %s l1_s64(l1b, %u)) & l1_mask(%u); }\n",
            dest, width, op, width, width);
    } else {
      emitf(be, "  %s = (l1a %s l1b) & l1_mask(%u); }\n", dest, op, width);
    }
    return;
  default:
    fail(be, 9203, "cbackend: unknown integer operator");
    return;
  }
  if (signed_op)
    emitf(be, "%s = (l1v)(l1_s64(%s, %u) %s l1_s64(%s, %u)) & l1_mask(%u);\n",
          dest, a, width, op, b, width, width);
  else
    emitf(be, "%s = (%s %s %s) & l1_mask(%u);\n", dest, a, op, b, width);
}

static void emit_compare(LainBackend *be, const L1Region *region,
                         const L1Inst *inst, uint32_t width, const char *dest) {
  char a[192], b[192];
  const char *op;
  bool sign = false;
  operand_masked(be, region, inst, 0, width, a, sizeof(a));
  operand_masked(be, region, inst, 1, width, b, sizeof(b));
  switch (inst->kind) {
  case INST_EQ: op = "=="; break;
  case INST_NE: op = "!="; break;
  case INST_SLT: op = "<"; sign = true; break;
  case INST_SLE: op = "<="; sign = true; break;
  case INST_SGT: op = ">"; sign = true; break;
  case INST_SGE: op = ">="; sign = true; break;
  case INST_ULT: op = "<"; break;
  case INST_ULE: op = "<="; break;
  case INST_UGT: op = ">"; break;
  case INST_UGE: op = ">="; break;
  default:
    fail(be, 9204, "cbackend: unknown comparison");
    return;
  }
  if (sign)
    emitf(be, "%s = (l1_s64(%s, %u) %s l1_s64(%s, %u)) ? 1u : 0u;\n", dest, a,
          width, op, b, width);
  else
    emitf(be, "%s = (%s %s %s) ? 1u : 0u;\n", dest, a, op, b);
}

static void emit_float_binary(LainBackend *be, const L1Region *region,
                              const L1Inst *inst, uint32_t width,
                              const char *dest) {
  char a[192], b[192];
  const char *op;
  operand_masked(be, region, inst, 0, width, a, sizeof(a));
  operand_masked(be, region, inst, 1, width, b, sizeof(b));
  switch (inst->kind) {
  case INST_FADD: op = "+"; break;
  case INST_FSUB: op = "-"; break;
  case INST_FMUL: op = "*"; break;
  case INST_FDIV: op = "/"; break;
  default:
    fail(be, 9205, "cbackend: unknown float operator");
    return;
  }
  if (width == 32)
    emitf(be, "%s = l1_f32_bits(l1_f32_of(%s) %s l1_f32_of(%s));\n", dest, a, op,
          b);
  else if (width == 64)
    emitf(be, "%s = l1_f64_bits(l1_f64_of(%s) %s l1_f64_of(%s));\n", dest, a, op,
          b);
  else
    fail(be, 9206, "cbackend: unsupported float format");
}

static void emit_float_compare(LainBackend *be, const L1Region *region,
                               const L1Inst *inst, uint32_t width,
                               const char *dest) {
  char a[192], b[192];
  const char *op;
  const char *conv = width == 32 ? "l1_f32_of" : "l1_f64_of";
  bool unordered_wanted = false;
  operand_masked(be, region, inst, 0, width, a, sizeof(a));
  operand_masked(be, region, inst, 1, width, b, sizeof(b));
  switch (inst->kind) {
  case INST_FOEQ: op = "=="; break;
  case INST_FONE: op = "!="; break;
  case INST_FOLT: op = "<"; break;
  case INST_FOLE: op = "<="; break;
  case INST_FOGT: op = ">"; break;
  case INST_FOGE: op = ">="; break;
  case INST_FUEQ: op = "=="; unordered_wanted = true; break;
  case INST_FUNE: op = "!="; unordered_wanted = true; break;
  case INST_FULT: op = "<"; unordered_wanted = true; break;
  case INST_FULE: op = "<="; unordered_wanted = true; break;
  case INST_FUGT: op = ">"; unordered_wanted = true; break;
  case INST_FUGE: op = ">="; unordered_wanted = true; break;
  default:
    fail(be, 9207, "cbackend: unknown float comparison");
    return;
  }
  if (unordered_wanted)
    emitf(be,
          "{ double x = (double)%s(%s), y = (double)%s(%s);"
          " %s = ((x != x) || (y != y) || (x %s y)) ? 1u : 0u; }\n",
          conv, a, conv, b, dest, op);
  else
    emitf(be,
          "{ double x = (double)%s(%s), y = (double)%s(%s);"
          " %s = ((x == x) && (y == y) && (x %s y)) ? 1u : 0u; }\n",
          conv, a, conv, b, dest, op);
}

static void emit_loop(LainBackend *be, const L1Inst *inst, const L1Region *region,
                      uint32_t position, const LoopCtx *loops) {
  const L1Region *body = inst->body;
  LoopCtx ctx;
  char head[32], end[32];
  uint32_t i;

  if (!inst->label) {
    fail(be, 9208, "cbackend: loop without a label");
    return;
  }
  ctx.label = inst->label;
  ctx.id = be->label_seq++;
  ctx.region = body;
  ctx.parent = region;
  ctx.parent_pos = position;
  ctx.outer = loops;
  snprintf(head, sizeof(head), "L%u_head", ctx.id);
  snprintf(end, sizeof(end), "L%u_end", ctx.id);

  /* 入口参数初值：在父区域的作用域里求值 */
  for (i = 0; i < body->param_count; i++) {
    char value[192];
    uint32_t width = width_of_type(body->params[i].param.ty);
    if (body->params[i].init.kind == OPERAND_VALUE) {
      uint32_t slot = 0;
      if (!resolve_slot(be, region, body->params[i].init.name, &slot)) {
        fail(be, 9209, "cbackend: undefined loop initial value");
        return;
      }
      snprintf(value, sizeof(value), "r[%u]", slot);
    } else {
      snprintf(value, sizeof(value), "(l1v)%llu",
               (unsigned long long)body->params[i].init.bits);
    }
    emit_indent(be);
    emitf(be, "r[%u] = %s & l1_mask(%u);\n", region_base(be, body) + i, value,
          width);
  }

  emit_indent(be);
  emitf(be, "%s: ;\n", head);
  be->indent++;
  emit_region(be, body, region, position, &ctx);
  be->indent--;
  emit_indent(be);
  emitf(be, "goto %s;\n", end); /* 区域末尾落下 = 离开循环 */
  emit_indent(be);
  emitf(be, "%s: ;\n", end);
}

static void emit_inst(LainBackend *be, const L1Region *region, uint32_t position,
                      const L1Inst *inst, const LoopCtx *loops,
                      const L1Region *parent, uint32_t parent_pos) {
  char dest[64];
  uint32_t width = inst->has_ty && inst->ty ? inst->ty->width : 64u;
  uint32_t i;

  dest[0] = '\0';
  if (inst->result_count > 0)
    snprintf(dest, sizeof(dest), "r[%u]", slot_of(be, region, position, 0));

  switch (inst->kind) {
  case INST_ADD: case INST_SUB: case INST_MUL:
  case INST_AND: case INST_OR: case INST_XOR:
  case INST_SHL: case INST_LSHR: case INST_ASHR:
  case INST_SDIV: case INST_UDIV: case INST_SREM: case INST_UREM:
    if (width > 64) {
      fail(be, 9210, "cbackend: integer width above 64 needs legalization");
      return;
    }
    emit_int_arith(be, region, inst, width, dest);
    return;

  case INST_EQ: case INST_NE:
  case INST_SLT: case INST_SLE: case INST_SGT: case INST_SGE:
  case INST_ULT: case INST_ULE: case INST_UGT: case INST_UGE:
    emit_compare(be, region, inst, width, dest);
    return;

  case INST_FADD: case INST_FSUB: case INST_FMUL: case INST_FDIV:
    emit_float_binary(be, region, inst, width, dest);
    return;
  case INST_FOEQ: case INST_FONE: case INST_FOLT: case INST_FOLE:
  case INST_FOGT: case INST_FOGE:
  case INST_FUEQ: case INST_FUNE: case INST_FULT: case INST_FULE:
  case INST_FUGT: case INST_FUGE:
    emit_float_compare(be, region, inst, width, dest);
    return;

  case INST_ZEXT: case INST_TRUNC: case INST_BITCAST: {
    char a[192];
    operand_expr(be, region, inst, 0, a, sizeof(a));
    emitf(be, "%s = %s & l1_mask(%u);\n", dest, a, width);
    return;
  }
  case INST_SEXT: {
    char a[192];
    uint32_t source = operand_width(be, region, inst, 0);
    operand_expr(be, region, inst, 0, a, sizeof(a));
    emitf(be, "%s = (l1v)l1_s64(%s, %u) & l1_mask(%u);\n", dest, a, source, width);
    return;
  }
  case INST_FPEXT: case INST_FPTRUNC: case INST_FPTOSI: case INST_FPTOUI:
  case INST_SITOFP: case INST_UITOFP:
  case INST_INT2PTR: case INST_PTR2INT: {
    char a[192];
    uint32_t source = operand_width(be, region, inst, 0);
    operand_expr(be, region, inst, 0, a, sizeof(a));
    switch (inst->kind) {
    case INST_FPTOSI:
      emitf(be, "%s = (l1v)(int64_t)l1_f64_of(%s) & l1_mask(%u);\n", dest, a,
            width);
      break;
    case INST_FPTOUI:
      emitf(be, "%s = (l1v)l1_f64_of(%s) & l1_mask(%u);\n", dest, a, width);
      break;
    case INST_SITOFP:
      emitf(be, "%s = l1_f64_bits((double)l1_s64(%s, %u));\n", dest, a, source);
      break;
    case INST_UITOFP:
      emitf(be, "%s = l1_f64_bits((double)(%s & l1_mask(%u)));\n", dest, a,
            source);
      break;
    default:
      emitf(be, "%s = %s & l1_mask(%u);\n", dest, a, width);
      break;
    }
    return;
  }

  case INST_LEA: {
    char base[192], idx[192], scale[192], offset[192];
    operand_expr(be, region, inst, 0, base, sizeof(base));
    operand_expr(be, region, inst, 1, idx, sizeof(idx));
    operand_expr(be, region, inst, 2, scale, sizeof(scale));
    operand_expr(be, region, inst, 3, offset, sizeof(offset));
    emitf(be,
          "%s = (l1v)((uintptr_t)(%s) + (uintptr_t)(%s) * (uintptr_t)(%s) + "
          "(uintptr_t)(%s));\n",
          dest, base, idx, scale, offset);
    return;
  }

  case INST_LOAD: {
    char a[192];
    bool is_addr = inst->ty && inst->ty->kind == TY_ADDR;
    uint32_t bytes = is_addr ? 8u : (width >= 8 ? width / 8u : 1u);
    operand_expr(be, region, inst, 0, a, sizeof(a));
    if (bytes > 8) {
      fail(be, 9211, "cbackend: load wider than 8 bytes needs legalization");
      return;
    }
    emitf(be, "%s = (l1v)(*(const %s *)(uintptr_t)(%s)) & l1_mask(%u);\n", dest,
          is_addr ? "uintptr_t" : mem_type(width), a, is_addr ? 64u : width);
    return;
  }
  case INST_STORE: {
    char value[192], a[192];
    bool is_addr = inst->ty && inst->ty->kind == TY_ADDR;
    uint32_t bytes = is_addr ? 8u : (width >= 8 ? width / 8u : 1u);
    operand_expr(be, region, inst, 0, value, sizeof(value));
    operand_expr(be, region, inst, 1, a, sizeof(a));
    if (bytes > 8) {
      fail(be, 9212, "cbackend: store wider than 8 bytes needs legalization");
      return;
    }
    emitf(be, "*(%s *)(uintptr_t)(%s) = (%s)(%s & l1_mask(%u));\n",
          is_addr ? "uintptr_t" : mem_type(width),
          a, is_addr ? "uintptr_t" : mem_type(width), value, is_addr ? 64u : width);
    return;
  }
  case INST_ALLOCA:
    emitf(be, "%s = (l1v)(uintptr_t)m%u_%u;\n", dest, region_base(be, region),
          position);
    return;

  case INST_DATA_ADDR: {
    char name[128];
    uint32_t k;
    const char *symbol = NULL;
    for (k = 0; k < be->module->data_count; k++) {
      if (be->module->data[k].symbol == inst->symbol) {
        symbol = be->module->data[k].symbol;
        break;
      }
    }
    if (!symbol) {
      for (k = 0; k < be->module->data_count; k++) {
        const char *candidate = be->module->data[k].symbol;
        if (candidate && inst->symbol && strcmp(candidate, inst->symbol) == 0) {
          symbol = candidate;
          break;
        }
      }
    }
    if (!symbol) {
      fail(be, 9214, "cbackend: unknown data symbol");
      return;
    }
    mangle(name, sizeof(name), symbol, "l1_data_");
    emitf(be, "%s = (l1v)(uintptr_t)%s;\n", dest, name);
    return;
  }
  case INST_PROC_ADDR: {
    char name[128];
    const L1Subroutine *callee = find_sub(be->module, inst->symbol);
    if (!callee) {
      fail(be, 9215, "cbackend: unknown subroutine in #proc_addr");
      return;
    }
    mangle(name, sizeof(name), callee->name, "sub_");
    emitf(be, "%s = (l1v)(uintptr_t)&%s;\n", dest, name);
    return;
  }

  case INST_CALL: {
    const L1Subroutine *callee = find_sub(be->module, inst->symbol);
    char name[128];
    uint32_t k;
    if (!callee) {
      fail(be, 9216, "cbackend: unknown callee");
      return;
    }
    if (callee->flags & SUBROUTINE_EXTERN) {
      const char *symbol = extern_symbol(callee);
      uint32_t result_width =
          callee->result_count > 0 ? width_of_type(callee->results[0]) : 0;
      if (!is_c_identifier(symbol)) {
        fail(be, 9227, "cbackend: external symbol name is not a C identifier");
        return;
      }
      emit_text(be, "{\n");
      emit_indent(be);
      if (inst->operand_count == 0) {
        /* 空初始化列表是 C23；零参数调用给一个占位元素。 */
        emit_text(be, "const uint64_t a[1] = {0};\n");
      } else {
        emitf(be, "const uint64_t a[%u] = {", inst->operand_count);
        for (k = 0; k < inst->operand_count; k++) {
          char a[192];
          uint32_t param_width =
              (callee->params && k < callee->param_count)
                  ? width_of_type(callee->params[k].ty)
                  : 64;
          operand_masked(be, region, inst, k, param_width, a, sizeof(a));
          emitf(be, "%s%s", k ? ", " : "", a);
        }
        emit_text(be, "};\n");
      }
      emit_indent(be);
      emitf(be, "uint64_t res = 0;\n");
      emit_indent(be);
      /* 宿主拒绝时没有别的出路：编译产物里没有能力异常这条路（待定）。 */
      emitf(be, "if (%s(a, %uu, %s) != 0) abort();\n", symbol,
            inst->operand_count, dest[0] ? "&res" : "0");
      if (dest[0]) {
        emit_indent(be);
        emitf(be, "%s = res & l1_mask(%u);\n", dest, result_width);
      }
      emit_indent(be);
      emit_text(be, "}\n");
      return;
    }
    mangle(name, sizeof(name), callee->name, "sub_");
    if (dest[0]) emitf(be, "%s = ", dest);
    emitf(be, "%s(", name);
    for (k = 0; k < inst->operand_count; k++) {
      char a[192];
      uint32_t param_width =
          (callee->params && k < callee->param_count)
              ? width_of_type(callee->params[k].ty)
              : 64;
      operand_masked(be, region, inst, k, param_width, a, sizeof(a));
      emitf(be, "%s%s", k ? ", " : "", a);
    }
    emit_text(be, ");\n");
    return;
  }
  case INST_CALL_INDIRECT: {
    char target[192];
    operand_expr(be, region, inst, 0, target, sizeof(target));
    if (dest[0]) emitf(be, "%s = ", dest);
    emitf(be, "l1_indirect(%s, (const l1v[]){", target);
    for (i = 1; i < inst->operand_count; i++) {
      char a[192];
      operand_masked(be, region, inst, i, 64, a, sizeof(a));
      emitf(be, "%s%s", i > 1 ? ", " : "", a);
    }
    emitf(be, "}, %uu);\n", inst->operand_count > 0 ? inst->operand_count - 1u : 0u);
    return;
  }

  case INST_IF:
    {
      char cond[192];
      operand_masked(be, region, inst, 0, 1, cond, sizeof(cond));
      emitf(be, "if (%s) {\n", cond);
      be->indent++;
      emit_region(be, inst->body, region, position, loops);
      be->indent--;
      if (inst->else_body) {
        emit_indent(be);
        emit_text(be, "} else {\n");
        be->indent++;
        emit_region(be, inst->else_body, region, position, loops);
        be->indent--;
      }
      emit_indent(be);
      emit_text(be, "}\n");
      return;
    }
  case INST_LOOP:
    emit_loop(be, inst, region, position, loops);
    return;
  case INST_SWITCH: {
    char sel[192];
    uint32_t k;
    uint32_t width = (inst->has_ty && inst->ty) ? width_of_type(inst->ty) : 64u;
    uint64_t mask = width >= 64u ? ~(uint64_t)0
                                 : ((((uint64_t)1u) << width) - 1u);
    /* 选择子和 case 常量都掩到同一个宽度：和引擎逐字对齐，
     * 否则写宽了的常量在两边会有不同的匹配行为。 */
    operand_masked(be, region, inst, 0, width, sel, sizeof(sel));
    emitf(be, "switch (%s) {\n", sel);
    for (k = 0; k < inst->case_count; k++) {
      emit_indent(be);
      emitf(be, "case %llu: {\n",
            (unsigned long long)(inst->cases[k].value & mask));
      be->indent++;
      emit_region(be, inst->cases[k].body, region, position, loops);
      be->indent--;
      emit_indent(be);
      emit_text(be, "} break;\n");
    }
    emit_indent(be);
    emit_text(be, "default: {\n");
    be->indent++;
    emit_region(be, inst->default_case, region, position, loops);
    be->indent--;
    emit_indent(be);
    emit_text(be, "} break;\n");
    emit_indent(be);
    emit_text(be, "}\n");
    return;
  }

  case INST_YIELD:
    if (!parent) {
      fail(be, 9219, "cbackend: #yield outside a region");
      return;
    }
    for (i = 0; i < inst->operand_count; i++) {
      char value[192];
      uint32_t target_width =
          (region->result_count > i) ? width_of_type(region->results[i]) : 64;
      operand_masked(be, region, inst, i, target_width, value, sizeof(value));
      emitf(be, "r[%u] = %s;\n", slot_of(be, parent, parent_pos, i), value);
    }
    return;
  case INST_BREAK: {
    const LoopCtx *ctx = find_loop(loops, inst->label);
    if (!ctx) {
      fail(be, 9220, "cbackend: #break without a target loop");
      return;
    }
    for (i = 0; i < inst->operand_count; i++) {
      char value[192];
      uint32_t target_width =
          (ctx->region->result_count > i) ? width_of_type(ctx->region->results[i])
                                          : 64;
      operand_masked(be, region, inst, i, target_width, value, sizeof(value));
      emitf(be, "r[%u] = %s;\n", slot_of(be, ctx->parent, ctx->parent_pos, i),
            value);
    }
    emitf(be, "goto L%u_end;\n", ctx->id);
    return;
  }
  case INST_CONTINUE: {
    const LoopCtx *ctx = find_loop(loops, inst->label);
    if (!ctx) {
      fail(be, 9221, "cbackend: #continue without a target loop");
      return;
    }
    for (i = 0; i < inst->operand_count; i++) {
      char value[192];
      uint32_t target_width =
          (ctx->region->param_count > i)
              ? width_of_type(ctx->region->params[i].param.ty)
              : 64;
      operand_masked(be, region, inst, i, target_width, value, sizeof(value));
      emitf(be, "r[%u] = %s;\n", region_base(be, ctx->region) + i, value);
    }
    emitf(be, "goto L%u_head;\n", ctx->id);
    return;
  }
  case INST_RETURN: {
    char value[192];
    uint32_t target_width =
        (be->sub->result_count > 0) ? width_of_type(be->sub->results[0]) : 64;
    if (inst->operand_count == 0) {
      emit_text(be, "return 0;\n");
      return;
    }
    operand_masked(be, region, inst, 0, target_width, value, sizeof(value));
    emitf(be, "return %s;\n", value);
    return;
  }

  default:
    fail(be, 9222, "cbackend: this operator is not implemented by the C backend");
    return;
  }
}

static void emit_region(LainBackend *be, const L1Region *region,
                        const L1Region *parent, uint32_t parent_pos,
                        const LoopCtx *loops) {
  uint32_t pos;
  if (!region || be->failed) return;
  for (pos = 0; pos < region->inst_count; pos++) {
    if (be->failed) return;
    emit_indent(be);
    emit_inst(be, region, pos, &region->insts[pos], loops, parent, parent_pos);
  }
}

/* --- 函数 ----------------------------------------------------------------- */

static void emit_subroutine(LainBackend *be, const L1Subroutine *sub) {
  char name[128];
  uint32_t i;

  if (sub->flags & SUBROUTINE_EXTERN) return;
  if (sub->body && sub->body->result_count > 0) {
    fail(be, 9223, "cbackend: procedure body must not declare region results");
    return;
  }

  be->sub = sub;
  be->region_count = 0;
  be->next_slot = sub->param_count;
  memset(be->widths, 0, (size_t)be->slot_cap);
  for (i = 0; i < sub->param_count; i++)
    set_width(be, i, width_of_type(sub->params[i].ty));
  assign_regions(be, sub->body, NULL);
  if (be->failed) return;

  mangle(name, sizeof(name), sub->name, "sub_");
  emitf(be, "static l1v %s(", name);
  if (sub->param_count == 0) {
    emit_text(be, "void");
  } else {
    for (i = 0; i < sub->param_count; i++)
      emitf(be, "%sl1v p%u", i ? ", " : "", i);
  }
  emit_text(be, ") {\n");
  emitf(be, "  l1v r[%u] = {0};\n", be->next_slot ? be->next_slot : 1);
  for (i = 0; i < sub->param_count; i++)
    emitf(be, "  r[%u] = p%u & l1_mask(%u);\n", i, i, slot_width(be, i));

  {
    uint32_t r;
    for (r = 0; r < be->region_count; r++) {
      const L1Region *region = be->regions[r].region;
      uint32_t pos;
      for (pos = 0; pos < region->inst_count; pos++) {
        const L1Inst *inst = &region->insts[pos];
        uint32_t elem;
        uint64_t bytes;
        if (inst->kind != INST_ALLOCA) continue;
        if (!inst->has_ty || !inst->ty ||
            inst->operands[0].kind == OPERAND_VALUE) {
          fail(be, 9224, "cbackend: #alloca needs a constant count");
          return;
        }
        elem = inst->ty->width >= 8 ? inst->ty->width / 8u : 1u;
        bytes = (uint64_t)elem * (inst->operands[0].bits ? inst->operands[0].bits : 1);
        emitf(be, "  uint8_t m%u_%u[%llu];\n", be->regions[r].base, pos,
              (unsigned long long)(bytes ? bytes : 1));
      }
    }
  }

  be->indent = 1;
  emit_region(be, sub->body, NULL, 0, NULL);
  be->indent = 0;
  emit_text(be, "  return 0;\n}\n\n");
}

static void emit_indirect(LainBackend *be) {
  uint32_t i;
  emit_text(be, "l1v l1_indirect(l1v target, const l1v *a, uint32_t n) {\n");
  /* 模块里可能没有任何带体的子过程，那时这几个参数一个都用不到。
   * 产物必须能带 -Wunused-parameter 编译。 */
  emit_text(be, "  (void)target; (void)a; (void)n;\n");
  for (i = 0; i < be->module->subroutine_count; i++) {
    const L1Subroutine *sub = &be->module->subroutines[i];
    char name[128];
    uint32_t k;
    if (sub->flags & SUBROUTINE_EXTERN) continue;
    mangle(name, sizeof(name), sub->name, "sub_");
    emitf(be, "  if (target == (l1v)(uintptr_t)&%s) {\n", name);
    emitf(be, "    if (n != %uu) return 0;\n", sub->param_count);
    emitf(be, "    return %s(", name);
    for (k = 0; k < sub->param_count; k++) emitf(be, "%sa[%u]", k ? ", " : "", k);
    emit_text(be, ");\n  }\n");
  }
  emit_text(be, "  abort();\n  return 0;\n}\n\n");
}

static void emit_entry(LainBackend *be) {
  uint32_t i;
  emit_text(be,
            "/* 统一入口：按子过程序号调用。 */\n"
            "l1v l1_call(uint32_t sub_index, const l1v *args, uint32_t nargs) {\n"
            "  (void)sub_index; (void)args; (void)nargs;\n"
            "  switch (sub_index) {\n");
  for (i = 0; i < be->module->subroutine_count; i++) {
    const L1Subroutine *sub = &be->module->subroutines[i];
    char name[128];
    uint32_t k;
    if (sub->flags & SUBROUTINE_EXTERN) continue;
    mangle(name, sizeof(name), sub->name, "sub_");
    emitf(be, "    case %uu: {\n      if (nargs != %uu) abort();\n", i,
          sub->param_count);
    emitf(be, "      return %s(", name);
    for (k = 0; k < sub->param_count; k++) emitf(be, "%sargs[%u]", k ? ", " : "", k);
    emit_text(be, ");\n    }\n");
  }
  emit_text(be,
            "    default: break;\n  }\n  abort();\n  return 0;\n}\n\n");
  emitf(be,
        "uint32_t l1_sub_count(void) { return %uu; }\n\n",
        be->module->subroutine_count);
}

LainBackend *lainbackend_new(const LainTarget *target,
                             const LainBackendSink *sink, L1Diagnostic *diag) {
  LainBackend *be;
  if (!target || !sink) return NULL;
  be = (LainBackend *)calloc(1, sizeof(LainBackend));
  if (!be) return NULL;
  be->target = target;
  be->sink = sink;
  be->diag = diag;
  if (diag) {
    diag->code = 0;
    diag->message[0] = '\0';
  }
  return be;
}

void lainbackend_free(LainBackend *backend) {
  if (!backend) return;
  free(backend->regions);
  free(backend->widths);
  free(backend->worklist);
  free(backend);
}

/* 往工作表里压一个区域；容量是区域总数上界，压不下就是计数错了。 */
static bool push_work(LainBackend *be, uint32_t *depth,
                      const L1Region *region) {
  if (!region) return true;
  if (*depth >= be->worklist_cap) {
    fail(be, 9226, "cbackend: region layout does not match the module");
    return false;
  }
  be->worklist[(*depth)++] = region;
  return true;
}

/* #eval 必须已经被折叠消掉。
 *
 * 工作表容量取区域总数（区域是树，每个区域最多压一次），所以不存在
 * 「压不下就跳过」这条静默路径——这里曾经是 stack[128] + 越界就静默丢弃，
 * 于是深嵌套里的 #eval 会被漏检。 */
static bool check_no_eval(LainBackend *be) {
  uint32_t i;
  for (i = 0; i < be->module->subroutine_count; i++) {
    uint32_t depth = 0;
    if ((be->module->subroutines[i].flags & SUBROUTINE_EXTERN) ||
        !be->module->subroutines[i].body)
      continue;
    if (!push_work(be, &depth, be->module->subroutines[i].body)) return false;
    while (depth > 0) {
      const L1Region *region = be->worklist[--depth];
      uint32_t pos;
      for (pos = 0; pos < region->inst_count; pos++) {
        const L1Inst *inst = &region->insts[pos];
        uint32_t k;
        if (inst->is_eval) {
          fail(be, 9225, "cbackend: #eval must be folded away before codegen");
          return false;
        }
        if (!push_work(be, &depth, inst->body)) return false;
        if (!push_work(be, &depth, inst->else_body)) return false;
        if (!push_work(be, &depth, inst->default_case)) return false;
        /* 分支体也要查：不然深埋在 case 里的 #eval 会被漏掉。 */
        for (k = 0; k < inst->case_count; k++) {
          if (!push_work(be, &depth, inst->cases[k].body)) return false;
        }
      }
    }
  }
  return true;
}

int lainbackend_emit(LainBackend *be, const L1Module *module) {
  uint32_t i;
  if (!be || !module) return 1;
  be->module = module;
  be->failed = false;
  lainir_types_init(&be->types, module);
  /* 先按模块量尺寸：槽数和区域数都由模块自己决定。 */
  free(be->regions);
  free(be->widths);
  free(be->worklist);
  be->regions = NULL;
  be->widths = NULL;
  be->worklist = NULL;
  if (!size_backend(be, module)) return 1;
  if (!check_no_eval(be)) return 1;

  emit_prologue(be);
  emit_data(be);
  emit_externs(be);
  if (be->failed) return 1;
  for (i = 0; i < module->subroutine_count; i++) {
    emit_subroutine(be, &module->subroutines[i]);
    if (be->failed) return 1;
  }
  emit_indirect(be);
  emit_entry(be);
  return be->failed ? 1 : 0;
}
