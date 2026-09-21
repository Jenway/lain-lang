/* 指令名 <-> 算子。解析器、打印器（以及将来的文档生成）共用一份。 */
#ifndef LAINIR_OPNAME_H
#define LAINIR_OPNAME_H

#include <stdbool.h>
#include <stdint.h>

#include "lainir/core.h"

/* 算子的文本拼写；未知返回 NULL。 */
const char *lainir_opcode_name(L1InstKind kind);

/* 按拼写找算子。len 是名字长度（不做 NUL 假设）。 */
bool lainir_opcode_kind(const char *name, uint32_t len, L1InstKind *kind_out);

/* 编译期执行调用的拼写。`#eval f(...)` 与 `#call f(...)` 是**同一个指令种类**
 * （INST_CALL），区别只在 `is_eval` 标记；解析器与打印器共用这一份拼写，
 * 不做前缀匹配（`#evall` 仍然是未知算子）。 */
extern const char *const LAINIR_OPCODE_EVAL;

#endif /* LAINIR_OPNAME_H */
