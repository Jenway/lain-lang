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
  L1_VALUE_ADDR = 2, /* **裸**宿主地址：宿主能力的结果、#data_addr、TCB 栈基址。
                      * 没有身份，只按 VSpace 判权限（今天的行为）。 */
  L1_VALUE_REF = 3,  /* **受检引用**：{能力句柄, 对象内偏移}。每次访问都要过内存
                      * 能力模型（对象身份 / 范围 / 权限 / 代数），再判 VSpace。
                      * 只能由 #alloca、base 是引用的 #lea、#int2ptr、以及
                      * #load[#addr] 读到的记录产生——程序内部洗不出裸地址。 */
} L1ValueKind;

typedef struct {
  L1ValueKind kind;
  uint32_t bit_width; /* BITS 用；ADDR / REF 无意义（0） */
  union {
    uint64_t bits;
    void *addr;
    /* 受检引用的载荷，与 `LainVmMemRef` **布局一致**（VM 侧用 _Static_assert 钉住）。
     * 这里不 include `lainvm/memcap.h`：lainir 是契约层，不依赖 VM 的实现。 */
    struct {
      uint32_t slot;
      uint32_t generation;
      uint64_t offset;
    } ref;
  } as;
} L1Value;

#endif /* LAINIR_VALUE_H */
