/* canonical 文本 -> 模块。
 *
 * 只接受 canonical 形状（printer 的输出），不假装宽容：
 *   - 类型实参总是写出来；
 *   - 一条指令一行，平坦；
 *   - 操作数写字面量，不写嵌套。
 *
 * 唯一的例外是**输入糖**：操作数位置上出现 `#op(...)` 会被展平成一条
 * 新指令（名字 `_tmp<N>`）。
 * 糖生成的临时名以 `_tmp` 开头，是保留前缀。
 *
 * 这里只做**拓扑**：拼出模块。名字、类型、区域这些规矩由验证器查。
 */
#ifndef LAINIR_PARSE_H
#define LAINIR_PARSE_H

#include "lain/ir/build.h"
#include "lain/ir/codes.h"
#include "lain/ir/core.h"

/* 解析拒绝码；段边界见 lain/ir/codes.h。 */
enum {
  LAINPARSE_ERR_SYNTAX = 3001,       /* 记法与书写层面的期望落空 */
  LAINPARSE_ERR_BUILD = 3002,        /* 构造器拒绝、未知算子或分配失败 */
  LAINPARSE_ERR_TYPE = 3003,         /* 物理类型不认识或对不上 */
  LAINPARSE_ERR_LITERAL = 3004,      /* 字面量写坏或超长 */
  LAINPARSE_ERR_NESTED = 3005,       /* 嵌套指令不在这里，或它不止产出一个值 */
  LAINPARSE_ERR_OPERANDS = 3006,
  LAINPARSE_ERR_REGION_RESULTS = 3007,
  LAINPARSE_ERR_RESULTS = 3008,
  LAINPARSE_ERR_LOOP_PARAMS = 3009,
  LAINPARSE_ERR_VECTOR_LANES = 3010, /* lane 后缀尚未支持 */
  LAINPARSE_ERR_PARAMS = 3011,
  LAINPARSE_ERR_DATA_ALLOC = 3012,
  LAINPARSE_ERR_DATA_OBJECTS_ALLOC = 3013,
  LAINPARSE_ERR_SUBROUTINES_ALLOC = 3014,
  LAINPARSE_ERR_CASES_ALLOC = 3015,
  LAINPARSE_ERR_SWITCH_DEFAULT = 3016,
};

_Static_assert(LAINPARSE_ERR_SYNTAX > LAINIR_CODES_TEXT_BASE &&
                   LAINPARSE_ERR_SWITCH_DEFAULT < LAINIR_CODES_TEXT_LIMIT,
               "parse codes must stay inside the text segment");

/* 解析失败返回 NULL，诊断写进 diag（可为 NULL）。 */
const L1Module *lainir_parse(L1Builder *builder, const char *text,
                             L1Diagnostic *diag);

#endif /* LAINIR_PARSE_H */
