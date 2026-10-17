# lain0 — C++ 前端

> lain0 = 用 C++ 写的 Lain 编译器，直接把 Lain 源码编译为 LAINIR。
> 它**不在 LainVM 里运行**，也不使用 CSpace / apply / region_grant。
> `bootstrap/` 是 Lain 写的 Meta 库，由 lain0 编译成 LAINIR 后交给 LainVM 执行。

## 编译管线

seed 已经有一整套可用的 IR 构造 + 验证 + 打印 API（`seed/kernel/include/lain/ir/`、
`seed/kernel/include/lain/text/`），全部是 PUBLIC 的：

```text
Lain 源码 → lexer/parser → AST → 语义层 → L1Builder 在内存搭 IR
         → lainir_verify() → 通过才 lainir_print_to_string()
```

带来的四条硬性质：

1. **不可能产出非法 IR** —— 不过 verify 就到不了打印。
2. **print/parse 已闭环** —— `text/print.h` 与 `text/parse.h` 定义了 canonical 固定点
   `print(parse(text)) == text`，走 printer 的输出天然进得了闸门。
3. **诊断有位置** —— `L1Diagnostic` 带 line/column，且 `L1Inst` 本身就带 line/column 字段。
4. **宽度推导不用重写** —— `ir/infer.h` 是 docs/01 规定的「宽度唯一来源」，
   直接 `lainir_inst_result_type()`，不存在「期望宽度传播」那一类 bug。

## 目录

```text
seed/lain0/
  include/lain0/
    Seed.hpp      extern "C" 边界（务必经它 include seed）
    Source.hpp    源文件管理、Pos/Span
    Diag.hpp      诊断 sink、诊断码
    Lexer.hpp     token
    Ast.hpp       AST 节点 + arena
    Parser.hpp    Pratt 解析
    Sema.hpp      作用域、类型表、布局
    Lower.hpp     AST → L1Builder
    Driver.hpp    编排 CLI
  src/*.cpp
  tools/main.cpp CLI：lain0 <file.lain> [-o out.l1] | lain0 --unit <清单> | lain0 --entry <file.lain> [--split <目录>]
  CMakeLists.txt
```

## 关键约束

- **不能直接 `#include` seed 头文件**，一律 `#include "lain0/Seed.hpp"`。
- **源码里不出现 `#` 形态**。这是 docs/04《判据》第一条。
- 所有名字串用 `lainir_builder_string()` 驻留，不能直接传 stack 上的临时字符串。
- 诊断码用 lain0 自己的号段，不许占用 `codes.h` 里已登记的 1xxx/2xxx/3xxx/9xxx。

## 垂直切片优先（push 的准线）

**Slice A（第一次 push 的准线）** —— 能端到端编译 `bootstrap/lain/examples/` 的
`arith.lain`、`flow.lain`、`inc/bump/lit` 一类最小样例，产物过 `lainir_parse + lainir_verify`：

- lexer / parser：func、param、return、let（标注可省）、if-语句、`+ - * / %`、`< > <= >= == !=`、整数字面量
- sema：标量表（i32/u32/i64/u64/usize/bool），宽度推导走 `infer.h`
- lower：表达式、调用、return、let、if
- driver + CLI

**Slice B（之后）** —— 补齐 `docs/04-bootstrap-dialect.md`《一、已能用》的全部行：
带值 if、loop/for + 标签/参数/break/continue、struct/enum（声明体、字段访问、变体投影、
函数体内构造降级到 `#alloca`）、`load/store/lea`、`ptr_of/addr_of`、`as`、负数字面量、
十六进制、顶层静态数据（字节串 / 定长表 → `#data`）、`extern`、`import` 成员访问。

## 任务拆分

| ID | 拥有文件 | 内容 | 依赖 |
|---|---|---|---|
| T1 | 全部 header + CMake | 契约（已完成者 RM 本人） | — |
| T2 | `src/Lexer.cpp` | 词法：标识符、整数、十六进制 `0x`、标点含 `<< >> ->`、注释；UTF-8 无关 | T1 |
| T3 | `src/Parser.cpp` | Pratt 表达式 + 语句/声明解析 | T1 |
| T4 | `src/Sema.cpp` | 作用域、类型表、struct/enum 布局、import 解析 | T1 |
| T5 | `src/LowerExpr.cpp` | 表达式降级（含算子表、宽度、`p+i*K` → `#lea`、`as`、构造器） | T1,T4 接口 |
| T6 | `src/LowerStmt.cpp` | 语句与控制流降级（if / loop / for / jump / return） | T1,T4 接口 |
| T7 | `src/LowerDecl.cpp` | 顶层：func / extern / struct / enum / import / 静态数据 | T1,T4 接口 |
| T8 | `src/Driver.cpp` + `tools/main.cpp` | 编排、verify、print、CLI | T1 |
| T9 | `tests/` | 回归 runner + 样例比对 | T8 |

T5/T6/T7 可以并行开发（接口由 T1 的 header 固定），集成在之后统一做。

## 模块命名空间、`--unit` 与 `--entry`/`--split`（已实现）

```text
lain0 <file.lain> [-o out.l1] [--dump-tokens] [--dump-ast] [--round-trip]
lain0 --unit <清单> [-o out.l1] [--round-trip]
lain0 --entry <file.lain> [--split <目录>] [-o out.l1] [--round-trip]
```

- **模块键** = 被解析到的文件相对**源根**的路径去 `.lain`、`/` 换 `_`（`bootstrap/std/lex.lain`
  → `std_lex`，`bootstrap/meta.lain` → `meta`）。源根：`--unit` 时是清单文件所在目录，
  `--entry` 时是入口文件所在目录（`--split` 下每个模块的键也按同一源根算）；
  单文件入口时是入口文件所在目录；规范化后跑出源根、或键里含 `__`，都报 312。
- **符号命名**：每个模块的顶层声明发 `<键>__<名字>` —— `func` 是 `<键>__f`，静态数据是
  `data <键>__x_storage` 加访问器 `#proc <键>__x()`，类型表键是 `<键>__<类型名>`。
  带 **`@root`** 标注的少数声明保持平坦（宿主按平坦字符串找入口；`bootstrap/meta.lain`
  的三个 `lain_std_abi_version` / `lain_std_initialize` / `lain_std_lower` 各自标着 `@root`），
  宿主 `extern` 的 `link_name` 同样保持平坦。两个模块各一个同名 `@root`、或 `@root` 平坦名与某个
  `<键>__<名字>` 相同 → 编译期报错。
- **跨模块访问**一律写 `M.名字`：`M.f(x)` 是调用、`M.NAME` 是访问器调用、`M.table[i]` 给元素地址、
  `p: M.Point` 的字段访问可用；`symbol_addr("X_storage")` 只解析**本模块**的静态数据，跨模块写
  别的模块的数据名报 301（消息里给出 `M.<名字>` 的写法）。
- **模块集合与产出（默认路径，无清单）**：`--entry <入口> --split <目录>` 从入口顺 import
  求闭包，**每个模块各出一份** `<键>.l1` 写进目录；顺序不影响任何一份的文本。装载侧
  （`seed/meta_driver.c`）用 `--units-dir <目录>` 装载目录里所有 `.l1`，按文件名排序
  （= 模块键排序）合并。模块集合由入口顺 import 求闭包得到，仓库里没有模块清单
  （`docs/proposal-modules-v2.md` §六）。`--entry` 与 `--unit` 互斥，`--split` 只与
  `--entry` 同用（且这时不能给 `-o`，拆分产出写进目录）。
- `--unit <清单>` 保留为兼容路径：每行一个 LAINIR 路径（相对**清单所在目录**）、`#` 起注释；
  清单顺序即降级顺序。`--unit` 与位置参数、与 `--entry` 都互斥（都给或都不给 → 退出码 2）；
  清单打不开 → 退出码 2 并打印 `lain0: 打不开清单 <路径>`。成员位置表与实测拒绝码见
  `docs/04-bootstrap-dialect.md` 的《模块：成员位置与符号命名》，设计取舍见
  `docs/proposal-modules-v2.md`，决策见 `docs/decisions.md` 的 D-004。

## 验收

1. `build/` 出 `lain0.exe`。
2. 对 `bootstrap/lain/examples/*.lain`（38 个）：产出文本能 `lainir_parse` + `lainir_verify` 通过
   （当前 **38/38**，`bash scripts/regress.sh` 与 `--round-trip` 同）。
