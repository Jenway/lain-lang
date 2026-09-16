/* 后端的目标描述。
 *
 * 后端和 engine 最大的差别之一：engine 跑在哪个机器上，是**编译 engine 的
 * 那个 C 编译器**决定的；后端的目标必须先告诉它。
 * 目标不是通用的：一个后端 = 一个目标。
 */
#ifndef LAINBACKEND_TARGET_H
#define LAINBACKEND_TARGET_H

#include <stdbool.h>
#include <stdint.h>

/* extern 调用怎么落地。
 *
 * 这决定「谁能违反能力规矩」以及「在哪里被发现」：
 * 后端一写，权威边界就被写死了，所以它必须在接口里，不能藏在实现里。 */
typedef enum {
  /* 发出一个外部符号引用：权威在链接器 / 加载器。
   * 「能力显式注入、不得隐式捕获未声明的环境输入」这条从此不归 VM 保证。 */
  LAINBC_CAP_SYMBOL = 0,
  /* 发出一条陷入：权威留在内核 / VM。 */
  LAINBC_CAP_TRAP = 1,
} LainBackendCapability;

typedef struct {
  const char *name;

  uint32_t address_bits;

  /* 原生支持的整数宽度。表里没有的宽度必须合法化：
   * 窄了掩码，宽了拆成多条带进位。 */
  const uint32_t *int_widths;
  uint32_t int_width_count;

  /* 原生支持的浮点格式（位数）。 */
  const uint32_t *float_formats;
  uint32_t float_format_count;

  uint32_t max_alignment;
  bool unaligned_ok; /* 假 = 非对齐访问要拆开，或者编译期拒绝 */
  bool little_endian;

  LainBackendCapability capability;
} LainTarget;

#endif /* LAINBACKEND_TARGET_H */
