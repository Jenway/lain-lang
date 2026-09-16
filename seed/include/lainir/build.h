/* LAINIR 构造接口。
 *
 * core.h 是纯数据，一个函数都没有；这层给测试和宿主一个手搭模块的办法。
 * 所有对象都从 builder 的 arena 里分配，在 lainir_builder_free 之前一直有效。
 *
 * 这是宿主侧工具，不是 LAINIR 的一部分：内核不认识它。
 */
#ifndef LAINIR_BUILD_H
#define LAINIR_BUILD_H

#include <stdbool.h>
#include <stdint.h>

#include "lainir/core.h"

typedef struct L1Builder L1Builder;

L1Builder *lainir_builder_new(void);
void lainir_builder_free(L1Builder *builder);

/* 把一段文本拷进 builder 的 arena。模块引用传进去的字符串，所以解析器
 * 必须用它来持有自己读到的名字。 */
const char *lainir_builder_string(L1Builder *builder, const char *text);

/* --- 类型 --- */
const L1Type *lainir_type(L1Builder *builder, L1TypeKind kind, uint32_t width);

/* --- 操作数 --- */
L1Operand lainir_ref(const char *name);     /* 引用一个值 */
L1Operand lainir_int(uint64_t bits);        /* 整数字面量 */
L1Operand lainir_float_bits(uint64_t bits); /* 浮点字面量（位模式） */

/* --- 区域 --- */
L1RegionParam lainir_param(const char *name, const L1Type *ty, L1Operand init);

const L1Region *lainir_region(L1Builder *builder, const L1RegionParam *params,
                              uint32_t param_count,
                              const L1Type *const *results,
                              uint32_t result_count,
                              const L1Inst *const *insts, uint32_t inst_count);

/* --- 指令 ---
 * result 为 NULL 表示不产出值；ty 为 NULL 表示没有类型实参。 */
const L1Inst *lainir_inst(L1Builder *builder, L1InstKind kind, const char *result,
                          const L1Type *ty, const L1Operand *operands,
                          uint32_t operand_count);

const L1Inst *lainir_inst_if(L1Builder *builder, const char *result,
                             L1Operand cond, const L1Region *then_body,
                             const L1Region *else_body);

const L1Inst *lainir_inst_loop(L1Builder *builder, const char *result,
                               const char *label, const L1Region *body);

const L1Inst *lainir_inst_jump(L1Builder *builder, L1InstKind kind,
                               const char *label, const L1Operand *operands,
                               uint32_t operand_count);

const L1Inst *lainir_inst_call(L1Builder *builder, const char *result,
                               const char *symbol, const L1Operand *operands,
                               uint32_t operand_count);

const L1Inst *lainir_inst_mem(L1Builder *builder, L1InstKind kind,
                              const char *result, const L1Type *ty,
                              L1MemOrder order, bool is_volatile,
                              const L1Operand *operands, uint32_t operand_count);

const L1Inst *lainir_inst_symbol(L1Builder *builder, L1InstKind kind,
                                 const char *result, const char *symbol);

/* 复制一条指令：换操作数和子区域，其余字段照抄（类型、内存序、符号、
 * 源位置、switch 的 cases）。body / else_body 传 NULL 表示沿用原来的
 * ——「去掉一个子区域」不是合法变换，所以不需要区分。
 *
 * 寿命规则（整个 builder 都适用）：建出来的对象**引用**传进去的字符串和
 * 类型指针，不做深拷贝。所以改写出来的模块引用输入模块的名字——
 * **输入必须活得比输出长**。 */
const L1Inst *lainir_inst_rewrite(L1Builder *builder, const L1Inst *inst,
                                  const L1Operand *operands,
                                  uint32_t operand_count, const L1Region *body,
                                  const L1Region *else_body);

/* --- 过程 --- */
L1Param lainir_proc_param(const char *name, const L1Type *ty);

const L1Subroutine *lainir_subroutine(L1Builder *builder, const char *name,
                                      const L1Param *params, uint32_t param_count,
                                      const L1Type *const *results,
                                      uint32_t result_count,
                                      const L1Region *body);

/* 没有体的子过程：实现在 LAINIR 之外。
 * link_name 既是**能力名**（解释器按它查能力空间）也是**链接符号名**
 * （后端按它发外部符号引用）——一个键，两种兑现方式。 */
const L1Subroutine *lainir_subroutine_extern(
    L1Builder *builder, const char *name, const char *link_name,
    const L1Param *params, uint32_t param_count, const L1Type *const *results,
    uint32_t result_count);

/* --- 模块 --- */
const L1Data *lainir_data(L1Builder *builder, const char *symbol,
                          const uint8_t *bytes, uint32_t size, bool is_writable);

const L1Module *lainir_module(L1Builder *builder, const char *name,
                              const L1Data *data, uint32_t data_count,
                              const L1Subroutine *subs, uint32_t sub_count);

#endif /* LAINIR_BUILD_H */
