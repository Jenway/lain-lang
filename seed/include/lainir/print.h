/* 模块 -> canonical 文本。
 *
 * canonical 的定义就是这里的输出：平坦（一条指令一行）、类型实参总是写出来、
 * 整数用十进制、每个模块项之间一个空行。**固定点**要求
 * `print(parse(text)) == text` 对 canonical 文本成立，所以解析器必须恰好接受
 * 这个形状，打印器必须是确定性的。
 */
#ifndef LAINIR_PRINT_H
#define LAINIR_PRINT_H

#include <stdint.h>

#include "lainir/core.h"

typedef struct {
  void (*write)(void *user, const char *bytes, uint32_t size);
  void *user;
} L1TextSink;

/* 把模块打成 canonical 文本，写进 sink。成功返回 0。 */
int lainir_print(const L1Module *module, const L1TextSink *sink,
                 L1Diagnostic *diag);

/* 便利：打到一块 malloc 出来的 NUL 结尾内存里；调用方 free。 */
char *lainir_print_to_string(const L1Module *module);

#endif /* LAINIR_PRINT_H */
