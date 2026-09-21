/* LAINVM 引擎：CU + ALU。
 *
 * 引擎无状态，所有可变状态在 TCB 里。它不分配、不调度、不管阻塞、
 * 不吞 trap；每条指令算一步。
 *
 * 位置约定：CU 在分派**之前不推进** position，算子在执行期间读到的
 * frame->position 就是当前这条指令。分派之后，如果算子没换帧也没动
 * 位置，CU 才推进一格。这样操作数读取和结果写回都不用额外的上下文。
 */
#include "lainvm/engine.h"

#include <limits.h>
#include <math.h>
#include <string.h>

#include "lainvm/image.h"

#define LAINVM_MAX_OPERANDS L1_MAX_OPERANDS

/* --- 基本换算 ------------------------------------------------------------- */

static LainVmFrame *top(LainVmTcb *tcb) {
  return &tcb->frames[tcb->frame_count - 1];
}

static const LainVmFrame *ctop(const LainVmTcb *tcb) {
  return tcb->frame_count > 0 ? &tcb->frames[tcb->frame_count - 1] : NULL;
}

static uint64_t width_mask(uint32_t width) {
  return width >= 64 ? ~(uint64_t)0 : (((uint64_t)1 << width) - 1);
}

static uint64_t sign_fill(uint64_t bits, uint32_t width) {
  if (width == 0 || width >= 64) return bits;
  return (bits & width_mask(width)) |
         (((bits >> (width - 1)) & 1) ? ~width_mask(width) : 0);
}

static int64_t as_signed(uint64_t bits, uint32_t width) {
  return (int64_t)sign_fill(bits, width);
}

static uintptr_t as_addr(L1Value value) {
  return value.kind == L1_VALUE_ADDR ? (uintptr_t)value.as.addr
                                     : (uintptr_t)value.as.bits;
}

static uint64_t type_size(const L1Type *ty) {
  if (!ty) return 1;
  if (ty->kind == TY_ADDR) return sizeof(void *);
  return ty->width >= 8 ? ty->width / 8u : 1u;
}

/* 一个物理类型占多少位。#addr 的 width 无意义，按指针宽度算。 */
static uint32_t type_width(const L1Type *ty) {
  if (!ty) return 64;
  if (ty->kind == TY_ADDR) return 64;
  return ty->width ? ty->width : 64;
}

static L1Value value_bits(uint64_t bits, uint32_t width) {
  L1Value value;
  memset(&value, 0, sizeof(value));
  value.kind = L1_VALUE_BITS;
  value.bit_width = width;
  value.as.bits = bits & width_mask(width);
  return value;
}

static L1Value value_addr(uintptr_t addr) {
  L1Value value;
  memset(&value, 0, sizeof(value));
  value.kind = L1_VALUE_ADDR;
  value.as.addr = (void *)addr;
  return value;
}

static LainVmSliceResult trap_now(LainVmTcb *tcb, LainVmTrapKind kind,
                                  int32_t status, const L1Inst *inst) {
  const LainVmFrame *frame = ctop(tcb);
  tcb->trap.kind = kind;
  tcb->trap.status = status;
  tcb->trap.region = frame ? frame->region : 0;
  tcb->trap.position = frame ? frame->position : 0;
  tcb->trap.line = inst ? inst->line : 0;
  tcb->trap.column = inst ? inst->column : 0;
  tcb->trap.active = true;
  tcb->state = LAINVM_DEAD;
  tcb->slice_result = LAINVM_SLICE_TRAPPED;
  return LAINVM_SLICE_TRAPPED;
}

/* --- 操作数与结果 --------------------------------------------------------- */

static L1Value read_ref(const LainVmTcb *tcb, LainVmOperandRef ref,
                        uint64_t literal_bits, const L1Type *ty) {
  L1Value value;
  memset(&value, 0, sizeof(value));
  if (ref.is_literal) {
    value.kind = L1_VALUE_BITS;
    value.bit_width = ty ? ty->width : 64u;
    value.as.bits = literal_bits & width_mask(value.bit_width);
    return value;
  }
  {
    const LainVmFrame *src =
        &tcb->frames[tcb->frame_count - 1 - ref.frame_distance];
    return tcb->slots[src->slot_base + ref.slot];
  }
}

L1Value lainvm_operand_read(const LainVmTcb *tcb, const L1Inst *inst,
                            uint32_t index) {
  const LainVmFrame *frame = ctop(tcb);
  LainVmOperandRef ref = lainvm_image_operand_ref(
      tcb->image, frame->region, frame->position, index);
  if (ref.is_literal) {
    const L1Operand *op = &inst->operands[index];
    return read_ref(tcb, ref, op->bits, inst->has_ty ? inst->ty : NULL);
  }
  return read_ref(tcb, ref, 0, NULL);
}

void lainvm_result_write(LainVmTcb *tcb, const L1Inst *inst, uint32_t index,
                         L1Value value) {
  LainVmFrame *frame = top(tcb);
  uint32_t slot = lainvm_image_result_slot(tcb->image, frame->region,
                                           frame->position, index);
  (void)inst;
  tcb->slots[frame->slot_base + slot] = value;
}

/* --- 区域栈 --------------------------------------------------------------- */

static LainVmSliceResult push_region(LainVmTcb *tcb, uint32_t region_id,
                                     uint32_t parent_position,
                                     const char *label, bool is_call) {
  const LainVmImageRegion *rec = lainvm_image_region(tcb->image, region_id);
  LainVmFrame *parent;
  LainVmFrame *frame;
  if (!rec) return trap_now(tcb, LAINVM_TRAP_STATE, 1010, NULL);
  if (tcb->frame_count >= tcb->frame_cap)
    return trap_now(tcb, LAINVM_TRAP_STATE, 1011, NULL);
  parent = top(tcb);
  frame = &tcb->frames[tcb->frame_count];
  frame->region = region_id;
  frame->position = 0;
  frame->slot_base = parent->slot_base + parent->slot_count;
  frame->slot_count = rec->slot_count;
  frame->parent_position = parent_position;
  frame->label = label;
  frame->is_call_frame = is_call;
  frame->stack_mark = tcb->stack_used;
  if (frame->slot_base + frame->slot_count > tcb->slot_cap)
    return trap_now(tcb, LAINVM_TRAP_STATE, 1012, NULL);
  memset(&tcb->slots[frame->slot_base], 0, sizeof(L1Value) * frame->slot_count);
  tcb->frame_count++;
  return LAINVM_SLICE_RUNNABLE;
}

/* 离开当前区域：值写进创造这一帧那条指令的结果槽，回退栈水位，
 * 推进父位置（父位置一直停在创造这一帧的那条指令上）。 */
static LainVmSliceResult leave_region(LainVmTcb *tcb, const L1Value *values,
                                      uint32_t count) {
  LainVmFrame *frame = top(tcb);
  LainVmFrame *parent;
  uint32_t i;
  if (tcb->frame_count < 2) {
    tcb->has_result = false;
    tcb->state = LAINVM_DEAD;
    tcb->slice_result = LAINVM_SLICE_DONE;
    return LAINVM_SLICE_DONE;
  }
  parent = &tcb->frames[tcb->frame_count - 2];
  for (i = 0; i < count; i++) {
    uint32_t slot = lainvm_image_result_slot(tcb->image, parent->region,
                                             frame->parent_position, i);
    if (slot == LAINVM_IMAGE_NO_INDEX)
      return trap_now(tcb, LAINVM_TRAP_STATE, 1013, NULL);
    tcb->slots[parent->slot_base + slot] = values[i];
  }
  tcb->stack_used = frame->stack_mark;
  tcb->frame_count--;
  parent->position++;
  return LAINVM_SLICE_RUNNABLE;
}

static int32_t find_loop_frame(const LainVmTcb *tcb, const char *label) {
  uint32_t i = tcb->frame_count;
  if (!label) return -1;
  while (i > 0) {
    i--;
    if (tcb->frames[i].label && strcmp(tcb->frames[i].label, label) == 0)
      return (int32_t)i;
  }
  return -1;
}

/* 读当前指令的第 index 个操作数，字面量按给定类型定宽。 */
static L1Value read_typed(LainVmTcb *tcb, const L1Inst *inst, uint32_t index,
                          const L1Type *ty) {
  const LainVmFrame *frame = ctop(tcb);
  LainVmOperandRef ref = lainvm_image_operand_ref(
      tcb->image, frame->region, frame->position, index);
  uint64_t bits = inst->operands ? inst->operands[index].bits : 0;
  return read_ref(tcb, ref, bits, ty);
}

/* --- ALU：整数 ------------------------------------------------------------ */

static LainVmSliceResult op_int_bin(LainVmTcb *tcb, const L1Inst *inst) {
  uint32_t width = inst->ty ? inst->ty->width : 64u;
  uint64_t a = lainvm_operand_read(tcb, inst, 0).as.bits;
  uint64_t b = lainvm_operand_read(tcb, inst, 1).as.bits;
  uint64_t r = 0;

  switch (inst->kind) {
  case INST_ADD: r = a + b; break;
  case INST_SUB: r = a - b; break;
  case INST_MUL: r = a * b; break;
  case INST_AND: r = a & b; break;
  case INST_OR: r = a | b; break;
  case INST_XOR: r = a ^ b; break;
  case INST_SHL:
  case INST_LSHR:
  case INST_ASHR:
    if (b >= width) return trap_now(tcb, LAINVM_TRAP_EXECUTION, 1002, inst);
    if (inst->kind == INST_SHL)
      r = a << b;
    else if (inst->kind == INST_LSHR)
      r = (a & width_mask(width)) >> b;
    else
      r = (uint64_t)(as_signed(a, width) >> b);
    break;
  case INST_UDIV:
  case INST_UREM:
    if (b == 0) return trap_now(tcb, LAINVM_TRAP_EXECUTION, 1001, inst);
    r = inst->kind == INST_UDIV ? a / b : a % b;
    break;
  case INST_SDIV:
  case INST_SREM: {
    int64_t x = as_signed(a, width);
    int64_t y = as_signed(b, width);
    if (y == 0) return trap_now(tcb, LAINVM_TRAP_EXECUTION, 1001, inst);
    if (y == -1 && x == INT64_MIN)
      return trap_now(tcb, LAINVM_TRAP_EXECUTION, 1003, inst);
    r = (uint64_t)(inst->kind == INST_SDIV ? x / y : x % y);
    break;
  }
  default:
    return trap_now(tcb, LAINVM_TRAP_STATE, 1014, inst);
  }
  lainvm_result_write(tcb, inst, 0, value_bits(r, width));
  return LAINVM_SLICE_RUNNABLE;
}

static LainVmSliceResult op_int_cmp(LainVmTcb *tcb, const L1Inst *inst) {
  uint32_t width = inst->ty ? inst->ty->width : 64u;
  uint64_t a = lainvm_operand_read(tcb, inst, 0).as.bits & width_mask(width);
  uint64_t b = lainvm_operand_read(tcb, inst, 1).as.bits & width_mask(width);
  int64_t sa = as_signed(a, width);
  int64_t sb = as_signed(b, width);
  bool r = false;

  switch (inst->kind) {
  case INST_EQ: r = a == b; break;
  case INST_NE: r = a != b; break;
  case INST_SLT: r = sa < sb; break;
  case INST_SLE: r = sa <= sb; break;
  case INST_SGT: r = sa > sb; break;
  case INST_SGE: r = sa >= sb; break;
  case INST_ULT: r = a < b; break;
  case INST_ULE: r = a <= b; break;
  case INST_UGT: r = a > b; break;
  case INST_UGE: r = a >= b; break;
  default: return trap_now(tcb, LAINVM_TRAP_STATE, 1015, inst);
  }
  lainvm_result_write(tcb, inst, 0, value_bits(r ? 1u : 0u, 1));
  return LAINVM_SLICE_RUNNABLE;
}

static LainVmSliceResult op_int_conv(LainVmTcb *tcb, const L1Inst *inst) {
  L1Value src = lainvm_operand_read(tcb, inst, 0);
  uint32_t target = inst->ty ? inst->ty->width : 64u;
  uint32_t source = src.bit_width ? src.bit_width : target;
  uint64_t r;

  switch (inst->kind) {
  case INST_ZEXT: r = src.as.bits & width_mask(source); break;
  case INST_SEXT: r = sign_fill(src.as.bits, source); break;
  case INST_TRUNC:
  case INST_BITCAST: r = src.as.bits; break;
  default: return trap_now(tcb, LAINVM_TRAP_STATE, 1016, inst);
  }
  lainvm_result_write(tcb, inst, 0, value_bits(r, target));
  return LAINVM_SLICE_RUNNABLE;
}

/* --- ALU：浮点 ------------------------------------------------------------ */

static double f64_of(uint64_t bits) {
  double d;
  memcpy(&d, &bits, sizeof(d));
  return d;
}
static uint64_t f64_bits(double d) {
  uint64_t bits;
  memcpy(&bits, &d, sizeof(bits));
  return bits;
}
static float f32_of(uint64_t bits) {
  uint32_t raw = (uint32_t)bits;
  float f;
  memcpy(&f, &raw, sizeof(f));
  return f;
}
static uint64_t f32_bits(float f) {
  uint32_t raw;
  memcpy(&raw, &f, sizeof(raw));
  return (uint64_t)raw;
}

static LainVmSliceResult op_float_bin(LainVmTcb *tcb, const L1Inst *inst) {
  uint32_t width = inst->ty ? inst->ty->width : 64u;
  uint64_t a = lainvm_operand_read(tcb, inst, 0).as.bits;
  uint64_t b = lainvm_operand_read(tcb, inst, 1).as.bits;

  if (width == 32) {
    float x = f32_of(a);
    float y = f32_of(b);
    float r;
    switch (inst->kind) {
    case INST_FADD: r = x + y; break;
    case INST_FSUB: r = x - y; break;
    case INST_FMUL: r = x * y; break;
    case INST_FDIV: r = x / y; break;
    default: return trap_now(tcb, LAINVM_TRAP_STATE, 1017, inst);
    }
    lainvm_result_write(tcb, inst, 0, value_bits(f32_bits(r), 32));
    return LAINVM_SLICE_RUNNABLE;
  }
  if (width == 64) {
    double x = f64_of(a);
    double y = f64_of(b);
    double r;
    switch (inst->kind) {
    case INST_FADD: r = x + y; break;
    case INST_FSUB: r = x - y; break;
    case INST_FMUL: r = x * y; break;
    case INST_FDIV: r = x / y; break;
    default: return trap_now(tcb, LAINVM_TRAP_STATE, 1017, inst);
    }
    lainvm_result_write(tcb, inst, 0, value_bits(f64_bits(r), 64));
    return LAINVM_SLICE_RUNNABLE;
  }
  return trap_now(tcb, LAINVM_TRAP_STATE, 1018, inst);
}

static LainVmSliceResult op_float_cmp(LainVmTcb *tcb, const L1Inst *inst) {
  uint32_t width = inst->ty ? inst->ty->width : 64u;
  double x = width == 32
                 ? (double)f32_of(lainvm_operand_read(tcb, inst, 0).as.bits)
                 : f64_of(lainvm_operand_read(tcb, inst, 0).as.bits);
  double y = width == 32
                 ? (double)f32_of(lainvm_operand_read(tcb, inst, 1).as.bits)
                 : f64_of(lainvm_operand_read(tcb, inst, 1).as.bits);
  bool unordered = isnan(x) || isnan(y);
  bool rel = false;
  bool want_unordered = false;

  switch (inst->kind) {
  case INST_FOEQ: rel = x == y; break;
  case INST_FONE: rel = x != y; break;
  case INST_FOLT: rel = x < y; break;
  case INST_FOLE: rel = x <= y; break;
  case INST_FOGT: rel = x > y; break;
  case INST_FOGE: rel = x >= y; break;
  case INST_FUEQ: rel = x == y; want_unordered = true; break;
  case INST_FUNE: rel = x != y; want_unordered = true; break;
  case INST_FULT: rel = x < y; want_unordered = true; break;
  case INST_FULE: rel = x <= y; want_unordered = true; break;
  case INST_FUGT: rel = x > y; want_unordered = true; break;
  case INST_FUGE: rel = x >= y; want_unordered = true; break;
  default: return trap_now(tcb, LAINVM_TRAP_STATE, 1019, inst);
  }
  if (want_unordered) rel = rel || unordered;
  else rel = rel && !unordered;
  lainvm_result_write(tcb, inst, 0, value_bits(rel ? 1u : 0u, 1));
  return LAINVM_SLICE_RUNNABLE;
}

static LainVmSliceResult op_float_conv(LainVmTcb *tcb, const L1Inst *inst) {
  L1Value src = lainvm_operand_read(tcb, inst, 0);
  uint32_t target = inst->ty ? inst->ty->width : 64u;
  uint32_t source = src.bit_width ? src.bit_width : 64u;
  double x = source == 32 ? (double)f32_of(src.as.bits) : f64_of(src.as.bits);
  L1Value out;

  switch (inst->kind) {
  case INST_FPEXT:
  case INST_FPTRUNC:
    out = target == 32 ? value_bits(f32_bits((float)x), 32)
                       : value_bits(f64_bits(x), 64);
    break;
  case INST_FPTOSI:
    out = value_bits((uint64_t)(int64_t)x, target);
    break;
  case INST_FPTOUI:
    out = value_bits((uint64_t)x, target);
    break;
  case INST_SITOFP:
    x = (double)as_signed(src.as.bits, source);
    out = target == 32 ? value_bits(f32_bits((float)x), 32)
                       : value_bits(f64_bits(x), 64);
    break;
  case INST_UITOFP:
    x = (double)(src.as.bits & width_mask(source));
    out = target == 32 ? value_bits(f32_bits((float)x), 32)
                       : value_bits(f64_bits(x), 64);
    break;
  default:
    return trap_now(tcb, LAINVM_TRAP_STATE, 1020, inst);
  }
  lainvm_result_write(tcb, inst, 0, out);
  return LAINVM_SLICE_RUNNABLE;
}

/* --- 地址与内存 ----------------------------------------------------------- */

static LainVmSliceResult op_lea(LainVmTcb *tcb, const L1Inst *inst) {
  uintptr_t base = as_addr(lainvm_operand_read(tcb, inst, 0));
  uintptr_t idx = (uintptr_t)lainvm_operand_read(tcb, inst, 1).as.bits;
  uint64_t scale = lainvm_operand_read(tcb, inst, 2).as.bits;
  uint64_t offset = lainvm_operand_read(tcb, inst, 3).as.bits;
  lainvm_result_write(tcb, inst, 0,
                      value_addr(base + idx * (uintptr_t)scale + (uintptr_t)offset));
  return LAINVM_SLICE_RUNNABLE;
}

static LainVmSliceResult op_int2ptr(LainVmTcb *tcb, const L1Inst *inst) {
  lainvm_result_write(tcb, inst, 0,
                      value_addr((uintptr_t)lainvm_operand_read(tcb, inst, 0).as.bits));
  return LAINVM_SLICE_RUNNABLE;
}

static LainVmSliceResult op_ptr2int(LainVmTcb *tcb, const L1Inst *inst) {
  uint32_t width = inst->ty ? inst->ty->width : 64u;
  lainvm_result_write(tcb, inst, 0,
                      value_bits((uint64_t)as_addr(lainvm_operand_read(tcb, inst, 0)),
                                 width));
  return LAINVM_SLICE_RUNNABLE;
}

static LainVmSliceResult op_load(LainVmTcb *tcb, const L1Inst *inst) {
  uintptr_t addr = as_addr(lainvm_operand_read(tcb, inst, 0));
  uint64_t size = type_size(inst->ty);
  uint64_t raw = 0;
  if (!lainvm_space_check(tcb->vspace, addr, size, LAINVM_MEM_READ))
    return trap_now(tcb, LAINVM_TRAP_EXECUTION, 1004, inst);
  memcpy(&raw, (const void *)addr, (size_t)size);
  if (inst->ty && inst->ty->kind == TY_ADDR)
    lainvm_result_write(tcb, inst, 0, value_addr((uintptr_t)raw));
  else
    lainvm_result_write(tcb, inst, 0,
                        value_bits(raw, inst->ty ? inst->ty->width : 64u));
  return LAINVM_SLICE_RUNNABLE;
}

static LainVmSliceResult op_store(LainVmTcb *tcb, const L1Inst *inst) {
  L1Value value = lainvm_operand_read(tcb, inst, 0);
  uintptr_t addr = as_addr(lainvm_operand_read(tcb, inst, 1));
  uint64_t size = type_size(inst->ty);
  uint64_t raw = value.kind == L1_VALUE_ADDR ? (uint64_t)(uintptr_t)value.as.addr
                                            : value.as.bits;
  if (!lainvm_space_check(tcb->vspace, addr, size, LAINVM_MEM_WRITE))
    return trap_now(tcb, LAINVM_TRAP_EXECUTION, 1005, inst);
  memcpy((void *)addr, &raw, (size_t)size);
  return LAINVM_SLICE_RUNNABLE;
}

static LainVmSliceResult op_alloca(LainVmTcb *tcb, const L1Inst *inst) {
  /* `count ? count : 1`：验证器已经要求 count > 0（码 2024 `#alloca count must
   * be positive`），所以这条分支在正常路径上够不着——留着只是不让引擎依赖
   * "上游一定拦住了"。 */
  uint64_t count = lainvm_operand_read(tcb, inst, 0).as.bits;
  uint64_t element = type_size(inst->ty);
  uint64_t total;
  uint64_t align = 16;
  uint64_t used;
  const LainVmRegion *stack;

  if (lainvm_space_handle_none(tcb->stack))
    return trap_now(tcb, LAINVM_TRAP_EXECUTION, 1006, inst);
  stack = lainvm_space_slot(tcb->vspace, tcb->stack);
  if (!stack) return trap_now(tcb, LAINVM_TRAP_EXECUTION, 1006, inst);
  /* 尺寸算术**先查回绕**，再判容量。`8 × 2^61` 曾经回绕成 0 字节，于是
   * 得到一个"合法"的分配（实测 R08）。失败不改变水位。 */
  if (element != 0 && count > 0xFFFFFFFFFFFFFFFFull / element)
    return trap_now(tcb, LAINVM_TRAP_EXECUTION, 1035, inst);
  total = element * (count ? count : 1);
  if (tcb->stack_used > 0xFFFFFFFFFFFFFFFFull - (align - 1))
    return trap_now(tcb, LAINVM_TRAP_EXECUTION, 1035, inst);
  used = (tcb->stack_used + (align - 1)) & ~(align - 1);
  if (total > 0xFFFFFFFFFFFFFFFFull - used)
    return trap_now(tcb, LAINVM_TRAP_EXECUTION, 1035, inst);
  if (used + total > stack->size)
    return trap_now(tcb, LAINVM_TRAP_EXECUTION, 1007, inst);
  tcb->stack_used = used + total;
  lainvm_result_write(tcb, inst, 0,
                      value_addr(stack->base + (uintptr_t)used));
  return LAINVM_SLICE_RUNNABLE;
}

static LainVmSliceResult op_data_addr(LainVmTcb *tcb, const L1Inst *inst) {
  const LainVmFrame *frame = ctop(tcb);
  LainVmImageInstMeta meta =
      lainvm_image_inst_meta(tcb->image, frame->region, frame->position);
  uintptr_t addr = lainvm_image_symbol_addr(tcb->image, meta.symbol_index);
  if (meta.symbol_index == LAINVM_IMAGE_NO_INDEX || addr == 0)
    return trap_now(tcb, LAINVM_TRAP_STATE, 1021, inst);
  lainvm_result_write(tcb, inst, 0, value_addr(addr));
  return LAINVM_SLICE_RUNNABLE;
}

/* --- 调用与结构 ----------------------------------------------------------- */

/* 调一次已经解析好的宿主能力。args 已经是 L1Value，这里只做装配与还原。 */
static LainVmSliceResult invoke_host(LainVmTcb *tcb, const L1Inst *inst,
                                     uint32_t sub_index,
                                     const L1Subroutine *sub,
                                     const L1Value *args, uint32_t arg_count,
                                     L1Value *result_out) {
  uint64_t raw[LAINVM_MAX_OPERANDS];
  uint64_t result = 0;
  uint32_t status;
  uint32_t i;
  const LainVmCapEntry *entry;
  LainVmHostFn fn;
  const L1Type *rt;

  if (arg_count != sub->param_count)
    return trap_now(tcb, LAINVM_TRAP_STATE, 1102, inst);
  if (arg_count > LAINVM_MAX_OPERANDS)
    return trap_now(tcb, LAINVM_TRAP_STATE, 1103, inst);
  if (!tcb->resolved || sub_index >= tcb->resolved_count ||
      !tcb->resolved[sub_index])
    return trap_now(tcb, LAINVM_TRAP_CAPABILITY, 1110, inst);
  entry = tcb->resolved[sub_index];
  if (lainvm_cap_kind(entry) != LAINVM_CAP_FUNCTION)
    return trap_now(tcb, LAINVM_TRAP_CAPABILITY, 1111, inst);
  fn = lainvm_cap_fn(entry);
  if (!fn) return trap_now(tcb, LAINVM_TRAP_CAPABILITY, 1112, inst);

  for (i = 0; i < arg_count; i++)
    raw[i] = args[i].kind == L1_VALUE_ADDR ? (uint64_t)(uintptr_t)args[i].as.addr
                                           : args[i].as.bits;

  status = fn(raw, arg_count, result_out ? &result : NULL);
  if (status != 0)
    return trap_now(tcb, LAINVM_TRAP_CAPABILITY, (int32_t)status, inst);

  if (result_out) {
    memset(result_out, 0, sizeof(*result_out));
    rt = sub->result_count > 0 ? sub->results[0] : NULL;
    if (rt && rt->kind == TY_ADDR) {
      result_out->kind = L1_VALUE_ADDR;
      result_out->as.addr = (void *)(uintptr_t)result;
    } else {
      result_out->kind = L1_VALUE_BITS;
      result_out->bit_width = type_width(rt);
      result_out->as.bits = result & width_mask(type_width(rt));
    }
  }
  return LAINVM_SLICE_RUNNABLE;
}

LainVmSliceResult lainvm_vm_call_host(LainVmTcb *tcb, uint32_t sub_index,
                                      const L1Value *args, uint32_t arg_count,
                                      L1Value *result_out) {
  const LainVmImageSub *sub;
  if (!tcb || sub_index >= tcb->image->sub_count)
    return trap_now(tcb, LAINVM_TRAP_CAPABILITY, 1100, NULL);
  sub = &tcb->image->subs[sub_index];
  return invoke_host(tcb, NULL, sub_index, sub->sub, args, arg_count,
                     result_out);
}

/* 宿主调用指令：没有体区域，先按调用点的参数类型读操作数，再交给 invoke_host。 */
static LainVmSliceResult enter_extern(LainVmTcb *tcb, const L1Inst *inst,
                                      uint32_t sub_index,
                                      const L1Subroutine *sub,
                                      uint32_t arg_base, uint32_t arg_count) {
  L1Value args[LAINVM_MAX_OPERANDS];
  L1Value result;
  LainVmSliceResult r;
  uint32_t i;
  if (arg_count > LAINVM_MAX_OPERANDS)
    return trap_now(tcb, LAINVM_TRAP_STATE, 1103, inst);
  for (i = 0; i < arg_count; i++) {
    const L1Type *ty = sub->params ? sub->params[i].ty : NULL;
    args[i] = read_typed(tcb, inst, arg_base + i, ty);
  }
  memset(&result, 0, sizeof(result));
  r = invoke_host(tcb, inst, sub_index, sub, args, arg_count,
                  inst->result_count > 0 ? &result : NULL);
  if (r != LAINVM_SLICE_RUNNABLE || inst->result_count == 0) return r;
  lainvm_result_write(tcb, inst, 0, result);
  return LAINVM_SLICE_RUNNABLE;
}

/* 压帧进入一个子过程，并把实参绑到它的参数槽。
 * 直接调用和间接调用共用这一段；extern 走能力空间。 */
static LainVmSliceResult enter_sub(LainVmTcb *tcb, const L1Inst *inst,
                                   uint32_t sub_index, uint32_t arg_base,
                                   uint32_t arg_count,
                                   uint32_t parent_position) {
  const LainVmImageSub *sub;
  L1Value args[LAINVM_MAX_OPERANDS];
  LainVmFrame *callee;
  LainVmSliceResult r;
  uint32_t i;

  if (sub_index >= tcb->image->sub_count)
    return trap_now(tcb, LAINVM_TRAP_CAPABILITY, 1100, inst);
  sub = &tcb->image->subs[sub_index];
  if (sub->sub->flags & SUBROUTINE_EXTERN)
    return enter_extern(tcb, inst, sub_index, sub->sub, arg_base, arg_count);
  if (sub->body_region == LAINVM_IMAGE_NO_REGION)
    return trap_now(tcb, LAINVM_TRAP_CAPABILITY, 1101, inst);
  if (arg_count != sub->sub->param_count)
    return trap_now(tcb, LAINVM_TRAP_STATE, 1102, inst);
  if (arg_count > LAINVM_MAX_OPERANDS)
    return trap_now(tcb, LAINVM_TRAP_STATE, 1103, inst);

  for (i = 0; i < arg_count; i++) {
    const L1Type *ty = sub->sub->params ? sub->sub->params[i].ty : NULL;
    args[i] = read_typed(tcb, inst, arg_base + i, ty);
  }
  r = push_region(tcb, sub->body_region, parent_position, NULL, true);
  if (r != LAINVM_SLICE_RUNNABLE) return r;
  callee = top(tcb);
  for (i = 0; i < arg_count; i++)
    tcb->slots[callee->slot_base + i] = args[i];
  return LAINVM_SLICE_RUNNABLE;
}

static LainVmSliceResult op_call(LainVmTcb *tcb, const L1Inst *inst) {
  const LainVmFrame *frame = ctop(tcb);
  LainVmImageInstMeta meta =
      lainvm_image_inst_meta(tcb->image, frame->region, frame->position);
  return enter_sub(tcb, inst, meta.sub_index, 0, inst->operand_count,
                   frame->position);
}

/* 间接调用：操作数 0 是目标地址，其余是实参。
 * 地址没有类型——能不能调用由它所在区段的 CALL 权限说话（相当于执行位）。 */
static LainVmSliceResult op_call_indirect(LainVmTcb *tcb, const L1Inst *inst) {
  const LainVmFrame *frame = ctop(tcb);
  uintptr_t target;
  uint32_t sub_index;

  if (inst->operand_count < 1)
    return trap_now(tcb, LAINVM_TRAP_STATE, 1104, inst);
  target = as_addr(lainvm_operand_read(tcb, inst, 0));
  if (!lainvm_space_check(tcb->vspace, target, sizeof(void *), LAINVM_MEM_CALL))
    return trap_now(tcb, LAINVM_TRAP_CAPABILITY, 1105, inst);
  sub_index = lainvm_image_sub_at(tcb->image, target);
  if (sub_index == LAINVM_IMAGE_NO_INDEX)
    return trap_now(tcb, LAINVM_TRAP_CAPABILITY, 1106, inst);
  return enter_sub(tcb, inst, sub_index, 1, inst->operand_count - 1,
                   frame->position);
}

/* 取子过程的地址。和 #data_addr 同形，产出的是普通 #addr。 */
static LainVmSliceResult op_proc_addr(LainVmTcb *tcb, const L1Inst *inst) {
  const LainVmFrame *frame = ctop(tcb);
  LainVmImageInstMeta meta =
      lainvm_image_inst_meta(tcb->image, frame->region, frame->position);
  uintptr_t addr;
  if (meta.sub_index == LAINVM_IMAGE_NO_INDEX ||
      meta.sub_index >= tcb->image->sub_count)
    return trap_now(tcb, LAINVM_TRAP_STATE, 1040, inst);
  addr = lainvm_image_sub_addr(tcb->image, meta.sub_index);
  if (addr == 0) return trap_now(tcb, LAINVM_TRAP_STATE, 1041, inst);
  lainvm_result_write(tcb, inst, 0, value_addr(addr));
  return LAINVM_SLICE_RUNNABLE;
}

static LainVmSliceResult op_if(LainVmTcb *tcb, const L1Inst *inst) {
  const LainVmFrame *frame = ctop(tcb);
  LainVmImageInstMeta meta =
      lainvm_image_inst_meta(tcb->image, frame->region, frame->position);
  bool truth = (lainvm_operand_read(tcb, inst, 0).as.bits & 1u) != 0;
  uint32_t target = truth ? meta.then_region : meta.else_region;

  if (target == LAINVM_IMAGE_NO_REGION) return LAINVM_SLICE_RUNNABLE;
  return push_region(tcb, target, frame->position, NULL, false);
}

static LainVmSliceResult op_switch(LainVmTcb *tcb, const L1Inst *inst) {
  const LainVmFrame *frame = ctop(tcb);
  LainVmImageInstMeta meta =
      lainvm_image_inst_meta(tcb->image, frame->region, frame->position);
  L1Value selector = lainvm_operand_read(tcb, inst, 0);
  uint64_t raw = selector.kind == L1_VALUE_ADDR
                     ? (uint64_t)(uintptr_t)selector.as.addr
                     : selector.as.bits;
  uint32_t width = selector.bit_width ? selector.bit_width : 64u;
  uint64_t mask;
  uint32_t target = meta.default_region;
  uint32_t i;

  /* 选择子的宽度：有类型实参就按它，否则按值自己的宽度。
   * 两边都掩码，写宽了的常量不会意外匹配窄的选择子。 */
  if (inst->has_ty && inst->ty)
    width = inst->ty->kind == TY_ADDR ? 64u : inst->ty->width;
  mask = width_mask(width);

  if (inst->case_count > 0) {
    if (meta.case_base == LAINVM_IMAGE_NO_INDEX)
      return trap_now(tcb, LAINVM_TRAP_STATE, 1042, inst);
    for (i = 0; i < inst->case_count; i++) {
      if ((inst->cases[i].value & mask) == (raw & mask)) {
        target = tcb->image->case_regions[meta.case_base + i];
        break;
      }
    }
  }
  /* 验证器要求 default 一定在，所以这里只可能是没验证过的模块。 */
  if (target == LAINVM_IMAGE_NO_REGION)
    return trap_now(tcb, LAINVM_TRAP_STATE, 1043, inst);
  return push_region(tcb, target, frame->position, NULL, false);
}

static LainVmSliceResult op_loop(LainVmTcb *tcb, const L1Inst *inst) {
  const LainVmFrame *frame = ctop(tcb);
  LainVmImageInstMeta meta =
      lainvm_image_inst_meta(tcb->image, frame->region, frame->position);
  const LainVmImageRegion *rec =
      lainvm_image_region(tcb->image, meta.loop_region);
  L1Value inits[LAINVM_MAX_OPERANDS];
  uint32_t i;
  LainVmSliceResult r;

  if (!rec) return trap_now(tcb, LAINVM_TRAP_STATE, 1022, inst);
  if (rec->region->param_count > LAINVM_MAX_OPERANDS)
    return trap_now(tcb, LAINVM_TRAP_STATE, 1023, inst);

  /* 初值在父帧求值，字面量按参数声明的类型定宽。 */
  for (i = 0; i < rec->region->param_count; i++) {
    LainVmOperandRef ref =
        lainvm_image_param_init_ref(tcb->image, meta.loop_region, i);
    uint64_t bits = rec->region->params[i].init.bits;
    inits[i] = read_ref(tcb, ref, bits, rec->region->params[i].param.ty);
  }
  r = push_region(tcb, meta.loop_region, frame->position, inst->label, false);
  if (r != LAINVM_SLICE_RUNNABLE) return r;
  {
    LainVmFrame *loop = top(tcb);
    for (i = 0; i < rec->region->param_count; i++)
      tcb->slots[loop->slot_base + i] = inits[i];
  }
  return LAINVM_SLICE_RUNNABLE;
}

static LainVmSliceResult op_yield(LainVmTcb *tcb, const L1Inst *inst) {
  const LainVmFrame *frame = ctop(tcb);
  const LainVmImageRegion *rec =
      lainvm_image_region(tcb->image, frame->region);
  L1Value values[LAINVM_MAX_OPERANDS];
  uint32_t i;

  if (inst->operand_count > LAINVM_MAX_OPERANDS)
    return trap_now(tcb, LAINVM_TRAP_STATE, 1024, inst);
  if (inst->operand_count != rec->region->result_count)
    return trap_now(tcb, LAINVM_TRAP_STATE, 1025, inst);
  for (i = 0; i < inst->operand_count; i++)
    values[i] = read_typed(tcb, inst, i, rec->region->results[i]);
  return leave_region(tcb, values, inst->operand_count);
}

static LainVmSliceResult op_break(LainVmTcb *tcb, const L1Inst *inst) {
  int32_t index = find_loop_frame(tcb, inst->label);
  L1Value values[LAINVM_MAX_OPERANDS];
  LainVmFrame *loop;
  LainVmFrame *parent;
  const LainVmImageRegion *rec;
  uint32_t i;

  if (index <= 0) return trap_now(tcb, LAINVM_TRAP_STATE, 1026, inst);
  if (inst->operand_count > LAINVM_MAX_OPERANDS)
    return trap_now(tcb, LAINVM_TRAP_STATE, 1027, inst);
  loop = &tcb->frames[index];
  rec = lainvm_image_region(tcb->image, loop->region);
  if (inst->operand_count != rec->region->result_count)
    return trap_now(tcb, LAINVM_TRAP_STATE, 1028, inst);
  for (i = 0; i < inst->operand_count; i++)
    values[i] = read_typed(tcb, inst, i, rec->region->results[i]);

  parent = &tcb->frames[index - 1];
  for (i = 0; i < inst->operand_count; i++) {
    uint32_t slot = lainvm_image_result_slot(tcb->image, parent->region,
                                             loop->parent_position, i);
    if (slot == LAINVM_IMAGE_NO_INDEX)
      return trap_now(tcb, LAINVM_TRAP_STATE, 1029, inst);
    tcb->slots[parent->slot_base + slot] = values[i];
  }
  tcb->stack_used = loop->stack_mark;
  tcb->frame_count = (uint32_t)index;
  parent->position++;
  return LAINVM_SLICE_RUNNABLE;
}

static LainVmSliceResult op_continue(LainVmTcb *tcb, const L1Inst *inst) {
  int32_t index = find_loop_frame(tcb, inst->label);
  L1Value values[LAINVM_MAX_OPERANDS];
  LainVmFrame *loop;
  const LainVmImageRegion *rec;
  uint32_t i;

  if (index < 0) return trap_now(tcb, LAINVM_TRAP_STATE, 1030, inst);
  loop = &tcb->frames[index];
  rec = lainvm_image_region(tcb->image, loop->region);
  if (inst->operand_count != rec->region->param_count)
    return trap_now(tcb, LAINVM_TRAP_STATE, 1031, inst);
  if (inst->operand_count > LAINVM_MAX_OPERANDS)
    return trap_now(tcb, LAINVM_TRAP_STATE, 1032, inst);
  for (i = 0; i < inst->operand_count; i++)
    values[i] = read_typed(tcb, inst, i, rec->region->params[i].param.ty);

  for (i = 0; i < inst->operand_count; i++)
    tcb->slots[loop->slot_base + i] = values[i];
  tcb->frame_count = (uint32_t)index + 1;
  tcb->stack_used = loop->stack_mark;
  loop->position = 0;
  return LAINVM_SLICE_RUNNABLE;
}

static LainVmSliceResult op_return(LainVmTcb *tcb, const L1Inst *inst) {
  L1Value value;
  LainVmFrame *callee;
  uint32_t i;

  memset(&value, 0, sizeof(value));
  if (inst->operand_count > 0) {
    const LainVmFrame *frame = ctop(tcb);
    const LainVmImageRegion *rec =
        lainvm_image_region(tcb->image, frame->region);
    const L1Type *ty = NULL;
    if (rec->sub_index < tcb->image->sub_count &&
        tcb->image->subs[rec->sub_index].sub->result_count > 0)
      ty = tcb->image->subs[rec->sub_index].sub->results[0];
    value = read_typed(tcb, inst, 0, ty);
  }

  /* 弹到最近的调用帧（区域帧都夹在调用帧之上）。 */
  while (tcb->frame_count > 0 && !top(tcb)->is_call_frame) tcb->frame_count--;
  if (tcb->frame_count == 0) {
    tcb->has_result = false;
    tcb->state = LAINVM_DEAD;
    tcb->slice_result = LAINVM_SLICE_DONE;
    return LAINVM_SLICE_DONE;
  }
  callee = top(tcb);
  tcb->stack_used = callee->stack_mark;
  tcb->frame_count--;
  if (tcb->frame_count == 0) {
    tcb->result = value;
    tcb->has_result = inst->operand_count > 0;
    tcb->state = LAINVM_DEAD;
    tcb->slice_result = LAINVM_SLICE_DONE;
    return LAINVM_SLICE_DONE;
  }
  {
    LainVmFrame *caller = top(tcb);
    if (inst->operand_count > 0) {
      uint32_t slot = lainvm_image_result_slot(tcb->image, caller->region,
                                               caller->position, 0);
      if (slot == LAINVM_IMAGE_NO_INDEX)
        return trap_now(tcb, LAINVM_TRAP_STATE, 1033, inst);
      tcb->slots[caller->slot_base + slot] = value;
    }
    caller->position++;
  }
  (void)i;
  return LAINVM_SLICE_RUNNABLE;
}

static LainVmSliceResult op_unimplemented(LainVmTcb *tcb, const L1Inst *inst) {
  return trap_now(tcb, LAINVM_TRAP_STATE, 1099, inst);
}

/* --- 分派表 --------------------------------------------------------------- */

static const LainVmOp k_ops[INST_COUNT] = {
    [INST_ADD] = op_int_bin,
    [INST_SUB] = op_int_bin,
    [INST_MUL] = op_int_bin,
    [INST_SDIV] = op_int_bin,
    [INST_UDIV] = op_int_bin,
    [INST_SREM] = op_int_bin,
    [INST_UREM] = op_int_bin,
    [INST_AND] = op_int_bin,
    [INST_OR] = op_int_bin,
    [INST_XOR] = op_int_bin,
    [INST_SHL] = op_int_bin,
    [INST_LSHR] = op_int_bin,
    [INST_ASHR] = op_int_bin,
    [INST_FADD] = op_float_bin,
    [INST_FSUB] = op_float_bin,
    [INST_FMUL] = op_float_bin,
    [INST_FDIV] = op_float_bin,
    [INST_EQ] = op_int_cmp,
    [INST_NE] = op_int_cmp,
    [INST_SLT] = op_int_cmp,
    [INST_SLE] = op_int_cmp,
    [INST_SGT] = op_int_cmp,
    [INST_SGE] = op_int_cmp,
    [INST_ULT] = op_int_cmp,
    [INST_ULE] = op_int_cmp,
    [INST_UGT] = op_int_cmp,
    [INST_UGE] = op_int_cmp,
    [INST_FOEQ] = op_float_cmp,
    [INST_FONE] = op_float_cmp,
    [INST_FOLT] = op_float_cmp,
    [INST_FOLE] = op_float_cmp,
    [INST_FOGT] = op_float_cmp,
    [INST_FOGE] = op_float_cmp,
    [INST_FUEQ] = op_float_cmp,
    [INST_FUNE] = op_float_cmp,
    [INST_FULT] = op_float_cmp,
    [INST_FULE] = op_float_cmp,
    [INST_FUGT] = op_float_cmp,
    [INST_FUGE] = op_float_cmp,
    [INST_ZEXT] = op_int_conv,
    [INST_SEXT] = op_int_conv,
    [INST_TRUNC] = op_int_conv,
    [INST_BITCAST] = op_int_conv,
    [INST_FPEXT] = op_float_conv,
    [INST_FPTRUNC] = op_float_conv,
    [INST_FPTOSI] = op_float_conv,
    [INST_FPTOUI] = op_float_conv,
    [INST_SITOFP] = op_float_conv,
    [INST_UITOFP] = op_float_conv,
    [INST_INT2PTR] = op_int2ptr,
    [INST_PTR2INT] = op_ptr2int,
    [INST_LEA] = op_lea,
    [INST_LOAD] = op_load,
    [INST_STORE] = op_store,
    [INST_ALLOCA] = op_alloca,
    [INST_DATA_ADDR] = op_data_addr,
    [INST_PROC_ADDR] = op_proc_addr,
    [INST_CALL] = op_call,
    [INST_CALL_INDIRECT] = op_call_indirect,
    [INST_IF] = op_if,
    [INST_LOOP] = op_loop,
    [INST_SWITCH] = op_switch,
    [INST_YIELD] = op_yield,
    [INST_BREAK] = op_break,
    [INST_CONTINUE] = op_continue,
    [INST_RETURN] = op_return,
};

const LainVmOp *lainvm_op_table(void) { return k_ops; }

/* lainvm/engine.h 里逐条声明的算子入口；这里都指向共用的实现。 */
#define LAINVM_OP_ALIAS(name)                                   \
  LainVmSliceResult lainvm_op_##name(LainVmTcb *tcb,            \
                                     const L1Inst *inst) {      \
    return op_int_bin(tcb, inst);                               \
  }

LainVmSliceResult lainvm_op_add(LainVmTcb *t, const L1Inst *i) { return op_int_bin(t, i); }
LainVmSliceResult lainvm_op_sub(LainVmTcb *t, const L1Inst *i) { return op_int_bin(t, i); }
LainVmSliceResult lainvm_op_mul(LainVmTcb *t, const L1Inst *i) { return op_int_bin(t, i); }
LainVmSliceResult lainvm_op_sdiv(LainVmTcb *t, const L1Inst *i) { return op_int_bin(t, i); }
LainVmSliceResult lainvm_op_udiv(LainVmTcb *t, const L1Inst *i) { return op_int_bin(t, i); }
LainVmSliceResult lainvm_op_srem(LainVmTcb *t, const L1Inst *i) { return op_int_bin(t, i); }
LainVmSliceResult lainvm_op_urem(LainVmTcb *t, const L1Inst *i) { return op_int_bin(t, i); }
LainVmSliceResult lainvm_op_and(LainVmTcb *t, const L1Inst *i) { return op_int_bin(t, i); }
LainVmSliceResult lainvm_op_or(LainVmTcb *t, const L1Inst *i) { return op_int_bin(t, i); }
LainVmSliceResult lainvm_op_xor(LainVmTcb *t, const L1Inst *i) { return op_int_bin(t, i); }
LainVmSliceResult lainvm_op_shl(LainVmTcb *t, const L1Inst *i) { return op_int_bin(t, i); }
LainVmSliceResult lainvm_op_lshr(LainVmTcb *t, const L1Inst *i) { return op_int_bin(t, i); }
LainVmSliceResult lainvm_op_ashr(LainVmTcb *t, const L1Inst *i) { return op_int_bin(t, i); }

LainVmSliceResult lainvm_op_fadd(LainVmTcb *t, const L1Inst *i) { return op_float_bin(t, i); }
LainVmSliceResult lainvm_op_fsub(LainVmTcb *t, const L1Inst *i) { return op_float_bin(t, i); }
LainVmSliceResult lainvm_op_fmul(LainVmTcb *t, const L1Inst *i) { return op_float_bin(t, i); }
LainVmSliceResult lainvm_op_fdiv(LainVmTcb *t, const L1Inst *i) { return op_float_bin(t, i); }

LainVmSliceResult lainvm_op_eq(LainVmTcb *t, const L1Inst *i) { return op_int_cmp(t, i); }
LainVmSliceResult lainvm_op_ne(LainVmTcb *t, const L1Inst *i) { return op_int_cmp(t, i); }
LainVmSliceResult lainvm_op_slt(LainVmTcb *t, const L1Inst *i) { return op_int_cmp(t, i); }
LainVmSliceResult lainvm_op_sle(LainVmTcb *t, const L1Inst *i) { return op_int_cmp(t, i); }
LainVmSliceResult lainvm_op_sgt(LainVmTcb *t, const L1Inst *i) { return op_int_cmp(t, i); }
LainVmSliceResult lainvm_op_sge(LainVmTcb *t, const L1Inst *i) { return op_int_cmp(t, i); }
LainVmSliceResult lainvm_op_ult(LainVmTcb *t, const L1Inst *i) { return op_int_cmp(t, i); }
LainVmSliceResult lainvm_op_ule(LainVmTcb *t, const L1Inst *i) { return op_int_cmp(t, i); }
LainVmSliceResult lainvm_op_ugt(LainVmTcb *t, const L1Inst *i) { return op_int_cmp(t, i); }
LainVmSliceResult lainvm_op_uge(LainVmTcb *t, const L1Inst *i) { return op_int_cmp(t, i); }

LainVmSliceResult lainvm_op_foeq(LainVmTcb *t, const L1Inst *i) { return op_float_cmp(t, i); }
LainVmSliceResult lainvm_op_fone(LainVmTcb *t, const L1Inst *i) { return op_float_cmp(t, i); }
LainVmSliceResult lainvm_op_folt(LainVmTcb *t, const L1Inst *i) { return op_float_cmp(t, i); }
LainVmSliceResult lainvm_op_fole(LainVmTcb *t, const L1Inst *i) { return op_float_cmp(t, i); }
LainVmSliceResult lainvm_op_fogt(LainVmTcb *t, const L1Inst *i) { return op_float_cmp(t, i); }
LainVmSliceResult lainvm_op_foge(LainVmTcb *t, const L1Inst *i) { return op_float_cmp(t, i); }
LainVmSliceResult lainvm_op_fueq(LainVmTcb *t, const L1Inst *i) { return op_float_cmp(t, i); }
LainVmSliceResult lainvm_op_fune(LainVmTcb *t, const L1Inst *i) { return op_float_cmp(t, i); }
LainVmSliceResult lainvm_op_fult(LainVmTcb *t, const L1Inst *i) { return op_float_cmp(t, i); }
LainVmSliceResult lainvm_op_fule(LainVmTcb *t, const L1Inst *i) { return op_float_cmp(t, i); }
LainVmSliceResult lainvm_op_fugt(LainVmTcb *t, const L1Inst *i) { return op_float_cmp(t, i); }
LainVmSliceResult lainvm_op_fuge(LainVmTcb *t, const L1Inst *i) { return op_float_cmp(t, i); }

LainVmSliceResult lainvm_op_zext(LainVmTcb *t, const L1Inst *i) { return op_int_conv(t, i); }
LainVmSliceResult lainvm_op_sext(LainVmTcb *t, const L1Inst *i) { return op_int_conv(t, i); }
LainVmSliceResult lainvm_op_trunc(LainVmTcb *t, const L1Inst *i) { return op_int_conv(t, i); }
LainVmSliceResult lainvm_op_bitcast(LainVmTcb *t, const L1Inst *i) { return op_int_conv(t, i); }

LainVmSliceResult lainvm_op_fpext(LainVmTcb *t, const L1Inst *i) { return op_float_conv(t, i); }
LainVmSliceResult lainvm_op_fptrunc(LainVmTcb *t, const L1Inst *i) { return op_float_conv(t, i); }
LainVmSliceResult lainvm_op_fptosi(LainVmTcb *t, const L1Inst *i) { return op_float_conv(t, i); }
LainVmSliceResult lainvm_op_fptoui(LainVmTcb *t, const L1Inst *i) { return op_float_conv(t, i); }
LainVmSliceResult lainvm_op_sitofp(LainVmTcb *t, const L1Inst *i) { return op_float_conv(t, i); }
LainVmSliceResult lainvm_op_uitofp(LainVmTcb *t, const L1Inst *i) { return op_float_conv(t, i); }

LainVmSliceResult lainvm_op_int2ptr(LainVmTcb *t, const L1Inst *i) { return op_int2ptr(t, i); }
LainVmSliceResult lainvm_op_ptr2int(LainVmTcb *t, const L1Inst *i) { return op_ptr2int(t, i); }
LainVmSliceResult lainvm_op_lea(LainVmTcb *t, const L1Inst *i) { return op_lea(t, i); }
LainVmSliceResult lainvm_op_load(LainVmTcb *t, const L1Inst *i) { return op_load(t, i); }
LainVmSliceResult lainvm_op_store(LainVmTcb *t, const L1Inst *i) { return op_store(t, i); }
LainVmSliceResult lainvm_op_alloca(LainVmTcb *t, const L1Inst *i) { return op_alloca(t, i); }
LainVmSliceResult lainvm_op_data_addr(LainVmTcb *t, const L1Inst *i) { return op_data_addr(t, i); }
LainVmSliceResult lainvm_op_proc_addr(LainVmTcb *t, const L1Inst *i) { return op_proc_addr(t, i); }
LainVmSliceResult lainvm_op_call(LainVmTcb *t, const L1Inst *i) { return op_call(t, i); }
LainVmSliceResult lainvm_op_call_indirect(LainVmTcb *t, const L1Inst *i) { return op_call_indirect(t, i); }
LainVmSliceResult lainvm_op_if(LainVmTcb *t, const L1Inst *i) { return op_if(t, i); }
LainVmSliceResult lainvm_op_loop(LainVmTcb *t, const L1Inst *i) { return op_loop(t, i); }
LainVmSliceResult lainvm_op_switch(LainVmTcb *t, const L1Inst *i) { return op_switch(t, i); }
LainVmSliceResult lainvm_op_yield(LainVmTcb *t, const L1Inst *i) { return op_yield(t, i); }
LainVmSliceResult lainvm_op_break(LainVmTcb *t, const L1Inst *i) { return op_break(t, i); }
LainVmSliceResult lainvm_op_continue(LainVmTcb *t, const L1Inst *i) { return op_continue(t, i); }
LainVmSliceResult lainvm_op_return(LainVmTcb *t, const L1Inst *i) { return op_return(t, i); }

#define LAINVM_OP_STUB(name)                                    \
  LainVmSliceResult lainvm_op_##name(LainVmTcb *tcb,            \
                                     const L1Inst *inst) {      \
    return op_unimplemented(tcb, inst);                         \
  }

LAINVM_OP_STUB(vadd)
LAINVM_OP_STUB(vsub)
LAINVM_OP_STUB(vmul)
LAINVM_OP_STUB(vdiv)
LAINVM_OP_STUB(vand)
LAINVM_OP_STUB(vor)
LAINVM_OP_STUB(vxor)
LAINVM_OP_STUB(vshl)
LAINVM_OP_STUB(vshuffle)
LAINVM_OP_STUB(vbroadcast)
LAINVM_OP_STUB(vextract)
LAINVM_OP_STUB(vinsert)
LAINVM_OP_STUB(vcmpeq)
LAINVM_OP_STUB(vcmpne)
LAINVM_OP_STUB(vcmplt)
LAINVM_OP_STUB(vcmpgt)
LAINVM_OP_STUB(xchg)
LAINVM_OP_STUB(cmpxchg)
LAINVM_OP_STUB(rmw_add)
LAINVM_OP_STUB(rmw_sub)
LAINVM_OP_STUB(rmw_and)
LAINVM_OP_STUB(rmw_or)
LAINVM_OP_STUB(rmw_xor)

/* --- CU ------------------------------------------------------------------- */

LainVmSliceResult lainvm_engine_step(LainVmTcb *tcb) {
  LainVmFrame *frame;
  const LainVmImageRegion *rec;
  const L1Inst *inst;
  LainVmSliceResult result;
  uint32_t saved_count;
  uint32_t saved_region;
  uint32_t saved_position;

  if (!tcb) return LAINVM_SLICE_TRAPPED;
  if (tcb->state != LAINVM_RUNNING) return tcb->slice_result;

  frame = top(tcb);
  rec = lainvm_image_region(tcb->image, frame->region);
  if (!rec) return trap_now(tcb, LAINVM_TRAP_STATE, 1008, NULL);

  /* 区域末尾落下 = 顺序离开这个区域；产出值的区域落下是错的。 */
  if (frame->position >= rec->inst_count) {
    if (rec->region->result_count > 0)
      return trap_now(tcb, LAINVM_TRAP_STATE, 1009, NULL);
    return leave_region(tcb, NULL, 0);
  }

  inst = &rec->region->insts[frame->position];
  if ((uint32_t)inst->kind >= (uint32_t)INST_COUNT)
    return trap_now(tcb, LAINVM_TRAP_STATE, 1034, inst);

  saved_count = tcb->frame_count;
  saved_region = frame->region;
  saved_position = frame->position;

  result = k_ops[inst->kind](tcb, inst);
  tcb->steps++;
  if (tcb->fuel > 0) tcb->fuel--;
  if (result != LAINVM_SLICE_RUNNABLE) {
    tcb->slice_result = result;
    return result;
  }

  frame = top(tcb);
  if (tcb->frame_count == saved_count && frame->region == saved_region &&
      frame->position == saved_position)
    frame->position++;
  return LAINVM_SLICE_RUNNABLE;
}

LainVmSliceResult lainvm_engine_run(LainVmTcb *tcb, uint64_t fuel) {
  LainVmSliceResult result = LAINVM_SLICE_RUNNABLE;
  if (!tcb) return LAINVM_SLICE_TRAPPED;
  tcb->fuel = fuel;
  while (result == LAINVM_SLICE_RUNNABLE && tcb->fuel > 0)
    result = lainvm_engine_step(tcb);
  return result;
}
