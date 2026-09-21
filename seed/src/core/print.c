/* lainir/print.h 的实现。 */
#include "lainir/print.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lainir/opname.h"

typedef struct {
  const L1TextSink *sink;
  uint32_t indent;
  L1Diagnostic *diag;
  bool failed;
} Printer;

static void out(Printer *p, const char *text) {
  if (!p->sink || !p->sink->write) return;
  p->sink->write(p->sink->user, text, (uint32_t)strlen(text));
}

static void outf(Printer *p, const char *fmt, ...) {
  char buffer[512];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);
  out(p, buffer);
}

static void ind(Printer *p) {
  uint32_t i;
  for (i = 0; i < p->indent; i++) out(p, "  ");
}

static void print_type(Printer *p, const L1Type *ty) {
  if (!ty) {
    out(p, "#?");
    return;
  }
  switch (ty->kind) {
  case TY_BITS: outf(p, "#bits<%u>", ty->width); break;
  case TY_FLOATS: outf(p, "#f<%u>", ty->width); break;
  case TY_VEC: outf(p, "#vec<%u>", ty->width); break;
  case TY_ADDR: out(p, "#addr"); break;
  default: out(p, "#?"); break;
  }
}

static void print_operand(Printer *p, const L1Operand *op, const L1Type *ty) {
  if (op->kind == OPERAND_VALUE) {
    outf(p, "%%%s", op->name ? op->name : "?");
    return;
  }
  if (op->kind == OPERAND_FLOAT) {
    uint32_t width = ty ? ty->width : 64;
    if (width == 32) {
      uint32_t raw = (uint32_t)op->bits;
      float f;
      memcpy(&f, &raw, sizeof(f));
      outf(p, "%.9g", (double)f);
    } else {
      double d;
      memcpy(&d, &op->bits, sizeof(d));
      outf(p, "%.17g", d);
    }
    return;
  }
  outf(p, "%llu", (unsigned long long)op->bits);
}

/* 区域声明的结果：` -> (T, T)`；没有就不写。 */
static void print_region_results(Printer *p, const L1Region *region) {
  uint32_t i;
  if (!region || region->result_count == 0) return;
  out(p, " -> (");
  for (i = 0; i < region->result_count; i++) {
    if (i) out(p, ", ");
    print_type(p, region->results[i]);
  }
  out(p, ")");
}

/* 过程签名的结果：` -> T`（单个不括号）或 ` -> (T, T)`。 */
static void print_sub_results(Printer *p, const L1Subroutine *sub) {
  uint32_t i;
  if (sub->result_count == 0) return;
  out(p, " -> ");
  if (sub->result_count == 1) {
    print_type(p, sub->results[0]);
    return;
  }
  out(p, "(");
  for (i = 0; i < sub->result_count; i++) {
    if (i) out(p, ", ");
    print_type(p, sub->results[i]);
  }
  out(p, ")");
}

static void print_params(Printer *p, const L1Param *params, uint32_t count) {
  uint32_t i;
  for (i = 0; i < count; i++) {
    if (i) out(p, ", ");
    outf(p, "%%%s: ", params[i].name ? params[i].name : "?");
    print_type(p, params[i].ty);
  }
}

static void print_loop_params(Printer *p, const L1Region *region) {
  uint32_t i;
  for (i = 0; i < region->param_count; i++) {
    const L1RegionParam *param = &region->params[i];
    if (i) out(p, ", ");
    outf(p, "%%%s: ", param->param.name ? param->param.name : "?");
    print_type(p, param->param.ty);
    out(p, " = ");
    print_operand(p, &param->init, param->param.ty);
  }
}

static void print_inst(Printer *p, const L1Region *region, const L1Inst *inst);
static void print_region(Printer *p, const L1Region *region);

static void print_result_prefix(Printer *p, const L1Inst *inst) {
  uint32_t i;
  if (inst->result_count == 0) return;
  for (i = 0; i < inst->result_count; i++) {
    if (i) out(p, ", ");
    outf(p, "%%%s", inst->results[i] ? inst->results[i] : "?");
  }
  out(p, " = ");
}

static void print_opcode(Printer *p, const L1Inst *inst) {
  const char *name = lainir_opcode_name(inst->kind);
  outf(p, "#%s", name ? name : "?");
  if (inst->lane_bits) outf(p, ".i%ux%u", inst->lane_bits,
                            inst->ty ? inst->ty->width / inst->lane_bits : 0u);
}

static void print_type_arg(Printer *p, const L1Inst *inst) {
  if (!inst->has_ty) return;
  out(p, "[");
  print_type(p, inst->ty);
  if (inst->is_volatile) out(p, ", volatile");
  if (inst->order != ORDER_RELAXED) {
    const char *order = "relaxed";
    switch (inst->order) {
    case ORDER_ACQUIRE: order = "acquire"; break;
    case ORDER_RELEASE: order = "release"; break;
    case ORDER_ACQ_REL: order = "acq_rel"; break;
    case ORDER_SEQ_CST: order = "seq_cst"; break;
    default: break;
    }
    outf(p, ", %s", order);
  }
  out(p, "]");
}

static void print_operand_list(Printer *p, const L1Inst *inst,
                               uint32_t first) {
  uint32_t i;
  out(p, "(");
  for (i = first; i < inst->operand_count; i++) {
    if (i > first) out(p, ", ");
    print_operand(p, &inst->operands[i],
                  inst->has_ty && !(inst->kind == INST_STORE && i == 1)
                      ? inst->ty
                      : NULL);
  }
  out(p, ")");
}

static void print_inst(Printer *p, const L1Region *region, const L1Inst *inst) {
  (void)region;
  ind(p);
  print_result_prefix(p, inst);

  switch (inst->kind) {
  case INST_IF:
    print_opcode(p, inst);
    if (inst->operand_count > 0) {
      out(p, " ");
      print_operand(p, &inst->operands[0], NULL);
    }
    print_region_results(p, inst->body);
    out(p, " {\n");
    p->indent++;
    print_region(p, inst->body);
    p->indent--;
    ind(p);
    if (inst->else_body) {
      out(p, "} else {\n");
      p->indent++;
      print_region(p, inst->else_body);
      p->indent--;
      ind(p);
    }
    out(p, "}\n");
    return;

  case INST_LOOP:
    print_opcode(p, inst);
    outf(p, " %s(", inst->label ? inst->label : "?");
    print_loop_params(p, inst->body);
    out(p, ")");
    print_region_results(p, inst->body);
    out(p, " {\n");
    p->indent++;
    print_region(p, inst->body);
    p->indent--;
    ind(p);
    out(p, "}\n");
    return;

  case INST_SWITCH: {
    uint32_t i;
    print_opcode(p, inst);
    /* 选择子的类型实参必须打出来：解析器要求它，往返靠它成立。 */
    print_type_arg(p, inst);
    if (inst->operand_count > 0) {
      out(p, " ");
      print_operand(p, &inst->operands[0], NULL);
    }
    print_region_results(p, inst->default_case ? inst->default_case
                                               : (inst->case_count > 0
                                                      ? inst->cases[0].body
                                                      : NULL));
    out(p, " {\n");
    for (i = 0; i < inst->case_count; i++) {
      p->indent++;
      ind(p);
      outf(p, "case %llu {\n", (unsigned long long)inst->cases[i].value);
      p->indent++;
      print_region(p, inst->cases[i].body);
      p->indent--;
      ind(p);
      out(p, "}\n");
      p->indent--;
    }
    if (inst->default_case) {
      p->indent++;
      ind(p);
      out(p, "default {\n");
      p->indent++;
      print_region(p, inst->default_case);
      p->indent--;
      ind(p);
      out(p, "}\n");
      p->indent--;
    }
    ind(p);
    out(p, "}\n");
    return;
  }

  case INST_BREAK:
  case INST_CONTINUE:
    print_opcode(p, inst);
    outf(p, " %s", inst->label ? inst->label : "?");
    if (inst->operand_count > 0) print_operand_list(p, inst, 0);
    out(p, "\n");
    return;

  case INST_YIELD:
  case INST_RETURN: {
    uint32_t i;
    print_opcode(p, inst);
    /* 终止子的操作数不带括号：`#return %r`、`#yield %a, %b`。 */
    for (i = 0; i < inst->operand_count; i++) {
      out(p, i ? ", " : " ");
      print_operand(p, &inst->operands[i], NULL);
    }
    out(p, "\n");
    return;
  }

  default:
    /* `#eval callee(args)` 与 `#call callee(args)` 是同一种指令的两种拼写：
     * 带编译期标记的直接调用打印成 `#eval`，其余种类仍用行尾标记。 */
    if (inst->kind == INST_CALL && inst->is_eval)
      outf(p, "#%s", LAINIR_OPCODE_EVAL);
    else
      print_opcode(p, inst);
    print_type_arg(p, inst);
    if (inst->kind == INST_DATA_ADDR || inst->kind == INST_PROC_ADDR)
      outf(p, " %s", inst->symbol ? inst->symbol : "?");
    else if (inst->kind == INST_CALL || inst->kind == INST_CALL_INDIRECT) {
      if (inst->kind == INST_CALL)
        outf(p, " %s", inst->symbol ? inst->symbol : "?");
      print_operand_list(p, inst, 0);
    } else {
      print_operand_list(p, inst, 0);
    }
    if (inst->is_eval && inst->kind != INST_CALL) out(p, " #eval");
    out(p, "\n");
    return;
  }
}

static void print_region(Printer *p, const L1Region *region) {
  uint32_t i;
  if (!region) return;
  for (i = 0; i < region->inst_count; i++)
    print_inst(p, region, &region->insts[i]);
}

static void print_subroutine(Printer *p, const L1Subroutine *sub) {
  out(p, "#proc ");
  outf(p, "%s(", sub->name ? sub->name : "?");
  print_params(p, sub->params, sub->param_count);
  out(p, ")");
  print_sub_results(p, sub);
  if (sub->flags & SUBROUTINE_EXTERN) {
    outf(p, " #extern \"%s\"\n",
         sub->link_name ? sub->link_name : (sub->name ? sub->name : "?"));
    return;
  }
  out(p, " {\n");
  p->indent++;
  print_region(p, sub->body);
  p->indent--;
  out(p, "}\n");
}

static void print_data(Printer *p, const L1Data *data) {
  uint32_t i;
  outf(p, "data %s %s {", data->symbol ? data->symbol : "?",
       data->is_writable ? "rw" : "ro");
  for (i = 0; i < data->size; i++) outf(p, " %u", data->bytes ? data->bytes[i] : 0);
  out(p, " }\n");
}

int lainir_print(const L1Module *module, const L1TextSink *sink,
                 L1Diagnostic *diag) {
  Printer p;
  uint32_t i;
  if (!module || !sink) return 1;
  memset(&p, 0, sizeof(p));
  p.sink = sink;
  p.diag = diag;
  if (diag) {
    diag->code = 0;
    diag->message[0] = '\0';
  }
  for (i = 0; i < module->data_count; i++) {
    print_data(&p, &module->data[i]);
  }
  if (module->data_count && module->subroutine_count) out(&p, "\n");
  for (i = 0; i < module->subroutine_count; i++) {
    if (i) out(&p, "\n");
    print_subroutine(&p, &module->subroutines[i]);
  }
  return p.failed ? 1 : 0;
}

typedef struct {
  char *data;
  uint32_t size;
  uint32_t cap;
} Buffer;

static void buffer_write(void *user, const char *bytes, uint32_t size) {
  Buffer *buffer = (Buffer *)user;
  if (buffer->size + size + 1 > buffer->cap) {
    uint32_t cap = buffer->cap ? buffer->cap : 256;
    while (cap < buffer->size + size + 1) cap *= 2;
    buffer->data = (char *)realloc(buffer->data, cap);
    buffer->cap = cap;
  }
  memcpy(buffer->data + buffer->size, bytes, size);
  buffer->size += size;
  buffer->data[buffer->size] = '\0';
}

char *lainir_print_to_string(const L1Module *module) {
  L1TextSink sink;
  Buffer buffer;
  memset(&buffer, 0, sizeof(buffer));
  sink.write = buffer_write;
  sink.user = &buffer;
  if (lainir_print(module, &sink, NULL) != 0) {
    free(buffer.data);
    return NULL;
  }
  if (!buffer.data) {
    buffer.data = (char *)calloc(1, 1);
  }
  return buffer.data;
}
