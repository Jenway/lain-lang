/* canonical 文本 -> 模块。
 *
 * 只接受 canonical 形状（printer 的输出），不假装宽容：
 *   - 类型实参总是写出来；
 *   - 一条指令一行，平坦；
 *   - 操作数写字面量，不写嵌套。
 *
 * 唯一的例外是**输入糖**：操作数位置上出现 `#op(...)` 会被展平成一条
 * 新指令（名字 `_tmp<N>`），这就是文档 §6 说的「嵌套只是文本糖」。
 * 糖生成的临时名以 `_tmp` 开头，是保留前缀。
 *
 * 这里只做**拓扑**：拼出模块。名字、类型、区域这些规矩由验证器查。
 */
#ifndef LAINIR_PARSE_H
#define LAINIR_PARSE_H

#include "lainir/build.h"
#include "lainir/core.h"

/* 解析失败返回 NULL，诊断写进 diag（可为 NULL）。 */
const L1Module *lainir_parse(L1Builder *builder, const char *text,
                             L1Diagnostic *diag);

#endif /* LAINIR_PARSE_H */
