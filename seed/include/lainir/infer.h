/* 一个值是什么物理类型。
 *
 * 这条规则**只写一遍**。验证器、后端（以及任何按宽度干活的消费者）都要它；
 * 写两遍就会漂，而它们必须一致——同一个值的宽度在两边不同，就是静默的错误，
 * 差分测试只在跑到时抓得住。
 *
 * 没有全局状态：合成类型（`#bits<1>`、`#addr`）由调用方持有。
 */
#ifndef LAINIR_INFER_H
#define LAINIR_INFER_H

#include "lainir/core.h"

typedef struct {
  const L1Module *module;
  L1Type bits1; /* 比较的结果 */
  L1Type addr;  /* 地址类算子的结果 */
} LainIrTypes;

void lainir_types_init(LainIrTypes *types, const L1Module *module);

const L1Subroutine *lainir_find_subroutine(const L1Module *module,
                                           const char *name);
const L1Data *lainir_find_data(const L1Module *module, const char *symbol);

/* 指令的第 index 个结果是什么类型；没有就返回 NULL。
 *
 *   - 比较             -> #bits<1>
 *   - #if / #loop      -> 它拥有的区域声明的结果
 *   - #call            -> 被调方声明的结果
 *   - 地址类算子        -> #addr
 *   - 其余             -> 指令的类型实参
 */
const L1Type *lainir_inst_result_type(const LainIrTypes *types,
                                      const L1Inst *inst, uint32_t index);

/* 一个物理类型占多少位。#addr 的 width 无意义，按指针宽度算。 */
uint32_t lainir_type_width(const L1Type *type);

#endif /* LAINIR_INFER_H */
