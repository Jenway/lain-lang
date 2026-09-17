/* LAINIR 验证器。
 *
 * VM 和后端都假定输入已验证；这一步就是那个假定的兑现处。
 * 规则来自 seed/docs/LAINIR.md §11。诊断码 0 = 成功，非 0 = 稳定码。
 *
 * 位置：verify -> fold -> backend。这里查 IR 自己的规矩（名字、区域、终结子、
 * 元数、类型类别、宽度可搬运），不查语义（那是引擎执行时的事）。
 *
 * 一条刻意的保守：规则「产出值的区域每条路径都要交值」只做到**结构判定**
 * ——区域末尾必须是交值的终结子（`#if` 要求两个分支都满足，`#loop` 体里必须
 * 有一个针对它的 `#break`）。可达性不分析，所以可能漏掉「某条路径不交值」的
 * 病态程序；引擎碰到落下会 trap，兜住它。
 */
#ifndef LAINIR_VERIFY_H
#define LAINIR_VERIFY_H

#include "lainir/core.h"

/* 验证整个模块。成功返回 0；失败返回非 0 并把原因写进 diag（可为 NULL）。 */
int lainir_verify(const L1Module *module, L1Diagnostic *diag);

/* 规则编号 -> 诊断码，方便测试与文档对齐。 */
enum {
  L1V_OK = 0,
  L1V_DUPLICATE_BINDING = 2001,
  L1V_UNDEFINED_VALUE = 2002,
  L1V_AFTER_TERMINATOR = 2003,
  L1V_NO_TYPE_FOR_LITERAL = 2004,
  L1V_BAD_OPERAND_TYPE = 2005,
  L1V_MISSING_YIELD = 2006,
  L1V_BAD_JUMP_ARITY = 2007,
  L1V_MISSING_TYPE = 2008,
  L1V_BAD_CONDITION = 2009,
  L1V_DUPLICATE_CASE = 2010,
  L1V_BAD_CALL_ARITY = 2011,
  L1V_EVAL_ARG_NOT_CONST = 2012,
  L1V_BAD_MEMORY_WIDTH = 2013,
  L1V_ALLOCA_NOT_CONST = 2014,
  L1V_UNKNOWN_CALLEE = 2015,
  L1V_UNKNOWN_PROC = 2016,
  L1V_UNKNOWN_SYMBOL = 2017,
  L1V_UNKNOWN_LOOP = 2018,
  L1V_BAD_RETURN = 2019,
  L1V_BAD_RESULT_COUNT = 2020,
  L1V_BODY_DECLARES_RESULTS = 2021,
  L1V_DUPLICATE_SUBROUTINE = 2022,
  L1V_DUPLICATE_DATA = 2023,
  L1V_BAD_ALLOCA_COUNT = 2024,
  /* 2025 曾经是「#switch 还不支持」。#switch 现在已经支持了，这个码退休，
   * 号码不再复用。 */
  L1V_SWITCH_NO_DEFAULT = 2029,
  /* 宽度上限：一个值就是机器的一个字。>64 位需要多字表示，VM 和 C 后端
   * 都要跟着改；在那之前，规范里就不允许。 */
  L1V_WIDTH_TOO_WIDE = 2026,
  L1V_ZERO_WIDTH = 2027,
  /* 验证器自己的资源耗尽（绑定表扩容失败）。它说的不是「模块非法」，
   * 而是「这次验证没做完」——不要和后一类混。 */
  L1V_OUT_OF_MEMORY = 2028,
};

#endif /* LAINIR_VERIFY_H */
