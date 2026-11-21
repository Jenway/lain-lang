/* LAINIR -> C99 后端。
 *
 * 将已验证的 L1Module 翻译为独立、零 UB 的标准 C99 源码：
 *   - 整数包裹运算全部使用无符号类型 + 位宽掩码；
 *   - 除零、有符号溢出（INT_MIN / -1）、移位 >= 位宽显式 trap；
 *   - #load / #store 与浮点位转换通过 memcpy 实现，无严格别名或未对齐 UB；
 *   - #alloca 绑定到过程激活帧，#if / #loop / #switch 结构化下降。
 */
#ifndef LAINIR_BACKEND_C99_H
#define LAINIR_BACKEND_C99_H

#include <stdbool.h>
#include <stdio.h>

#include "lain/ir/core.h"

typedef struct {
  /* 非 NULL 时生成 `int main(void)` 包装调用该无参入口过程并打印/返回结果。 */
  const char *entry;
  /* 若为真，在 main 中把入口返回值以十进制打印到 stdout 并返回 0（否则直接退出码）。 */
  bool print_result;
} LainC99Options;

/* 将已验证的 module 发射为 C99 源码写入 out。
 * 成功返回 0；失败返回非 0 并填写 diag（可为 NULL）。 */
int lainir_emit_c99(const L1Module *module, FILE *out,
                    const LainC99Options *opts, L1Diagnostic *diag);

#endif /* LAINIR_BACKEND_C99_H */
