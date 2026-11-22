/* 跨边界的值。
 *
 * VM 递给外面、外面递给 VM 的东西。只有两种：整数位串和地址。
 *
 * 没有 unit：没有结果就是「这个位置没有值」，不是一种类型。
 * 没有 string：字符串就是指向数据对象的 #addr（LAINIR 没有复合类型）。
 * 没有 func：函数指针也是 #addr，间接调用不做签名检查。
 */
#ifndef LAINIR_VALUE_H
#define LAINIR_VALUE_H

#include <stdint.h>

typedef enum {
  L1_VALUE_NONE = 0, /* 位置为空：不是一种类型，是没有值 */
  L1_VALUE_BITS = 1, /* 整数位串；bit_width 说明低多少位是真值 */
  L1_VALUE_ADDR = 2, /* 地址 */
  L1_VALUE_VEC = 3,  /* 向量寄存器：bit_width 位，按 as.vec 低位起顺序存放 */
} L1ValueKind;

typedef struct {
  L1ValueKind kind;
  uint32_t bit_width; /* BITS 用；ADDR 无意义（0） */
  union {
    uint64_t bits;
    void *addr;
    /* VEC 用：bit_width 最多 512，所以 8 个 64 位字就够；高位未用部分为 0。 */
    uint64_t vec[8];
  } as;
} L1Value;

#endif /* LAINIR_VALUE_H */
