# lain0 实现地图

语言标准：C++20。

## 文件分工

| 文件 | 内容 |
| --- | --- |
| `src/Source.cpp` | `SourceMgr`：文件 id、行首偏移表、`position()`、`lineText()`；CRLF 的 `\r` 不算进行文本 |
| `src/Diag.cpp` | `DiagSink`：按 `文件:行:列: error: 消息 (码)` 输出；无 Span 时用 `<unknown>` |
| `src/Lexer.cpp` | 词法与关键字表；`<= >= == != << >> ->` 最长匹配 |
| `src/Parser.cpp` | Pratt 表达式与声明、语句解析 |
| `src/Sema.cpp` | 作用域、类型表、struct/enum 布局、算子选择、import 成员登记 |
| `src/LowerExpr.cpp` | 表达式降级：宽度取自 `lainir_inst_result_type()`，地址算术走 `#lea`，内建 `load/store/lea/ptr_of/addr_of/len` |
| `src/LowerStmt.cpp` | 值 `#if`、`#loop`、`for`、`break/continue` 的降级与终结判定 |
| `src/LowerDecl.cpp` | 顶层 func/extern/静态数据/聚合构造的降级 |
| `src/Driver.cpp` | `loadFile → lex → parseModule → analyze → Lowerer.run → lainir_verify → lainir_print_to_string`；verify 不过不产出文本 |
| `tools/main.cpp` | CLI：`lain0 <file.lain> [-o out.l1]`、`lain0 --unit <清单>` |

## 必须遵守的约束

1. **不能 `#include` seed 头文件。** 一律 `#include "lain0/Seed.hpp"`（它内部做 `extern "C"` 包裹）。
2. **不拼含 `#` 的 IR 文本。** 一切 IR 经 Seed.hpp 的构造 API 产生。
3. 诊断码用 `include/lain0/Diag.hpp` 里 `enum class D`，不自造数字。
4. `CompileResult::ok` 为 true 要求：无诊断错误、verify 通过、文本非空。

## 单文件语法检查

```bash
clang++ -std=c++20 -fsyntax-only -I seed/lain0/include -I seed/kernel/include \
  seed/lain0/src/<文件>.cpp
```
