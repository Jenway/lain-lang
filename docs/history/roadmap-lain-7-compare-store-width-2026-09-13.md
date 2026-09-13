# 编码 7：有符号比较与无类型 `#store` 位宽修复快照

日期：2026-09-13。只归档两个后端静默错码缺陷，完整 backend/native 阶段尚未完成。

## 问题与修改

两个缺陷都出自同一处：本后端把 `#bits<32>`/`#bits<64>` 映射为 `uint32_t`/`uint64_t`，
而需要「按操作数自身宽度作有符号解释」的操作直接使用了 C 的裸运算符。

### 1. `#slt`/`#sle`/`#sgt`/`#sge` 在 32/64 位上按无符号比较

生成 C 为 `#define L1_lt(a,b) ((a)<(b))`，比较的是操作数各自的 C 类型。对
`#bits<32>` 即 `uint32_t`，因此 `#slt(-1, 1)` 比较 4294967295 < 1 得假；seed 后端把
同一类型放在 `int32_t` 里，得有符号的真。8/16 位不受影响（`int8_t`/`int16_t` 本身有符号）。

修改：把 `#sdiv` 已引入的按宽度有符号重解释提取为共用宏 `L1_signed`，四个有符号比较
改为 `(L1_signed(a) < L1_signed(b))` 形式。无符号族**不**走该宏：它正是靠先拓宽到
`uint64_t` 才成为无符号比较。

### 2. 无类型 `#store` 的位宽靠文本猜成 64 位

`#store` 可以省略物理类型，此时 L1 verifier 从值表达式的类型推断（`verifier.c` 的
`INST_STORE`，推断不出则报 2017），seed emitter 存的是推断结果。本后端只读文本、
没有类型信息，于是默认 64 位，仅在值文本以 `%byte` 或 `#trunc[#bits<8>]` 开头时收窄。
因此 `#bits<32>` 的绑定被写成 8 字节，覆盖其后的字段；`%byte` 又是前缀匹配，`%bytes`
这类名字也会被误判。

修改：新增 `L1_store_auto(dest,val)`，用 `_Generic` 按**值的 C 类型**选择
`L1_store8/16/32/64`。绑定保留其声明类型、调用保留返回类型、`#load`/`#trunc` 保留注解，
`#addr` 的 `uintptr_t` 在 LLP64/LP64 上都与 `uint64_t` 同类型，因此所有值形态都能从
生成的表达式读出位宽。八种定宽类型两两不同，两种数据模型下都没有重复关联；其他类型
（例如被整型提升成 `int` 的窄算术）落到 `default`，其 arm 定义为零参函数，被选中时
**编译期**报错而不是静默选错宽度。

两种形态仍按文本判定，因为它们的 C 拼写丢失了宽度：整数字面量在 L1 里是 `#bits<64>`
而 C 类型是 `int`；`#trunc[#bits<8>]` 的包装会在写出前被剥掉，其宽度注解到不了选择处。
旧的 `%byte` 前缀启发式被删除（`_Generic` 已覆盖该情形且更准确）。

顺带加入 `append_text`：按 NUL 扫描确定长度，取代手数字节数。此前手数的两处新串
各差 1，生成 C 里出现 NUL 字节（编译器 `-Wnull-character` 警告）——这正是该辅助函数
要消除的失败模式。

## 已执行验收

- `python scripts/check_backend_differential.py`：从当前源码独立重建 backend 后
  10 个差分用例全部通过，含新增两条并带独立预期值：
  `differential_signed_compare.l1` → 1（无符号读法会得 10）、
  `differential_bare_store.l1` → 4（单一猜测位宽会得 1）。
- 负对照（不写入 `scripts/`，只在 `build/` 内构造）：把当前源码中 `L1_lt`/`L1_gt` 两行
  还原为裸 C 运算符后重建，比较用例返回 10；保留原有无类型 store 猜测逻辑的 backend
  返回 1。两条新用例都能暴露被修的错码，不是只断言「不崩」。
- `python scripts/check_backend_c_shape.py`：全部形状与拒绝用例通过。
- `python scripts/check_native_backend_canonical_diff.py`、`check_native_backend_migration.py`：
  通过。
- `python scripts/check_lainc_lainir_api_baseline.py`：42 道全量入口通过。

## 未完成：真实产物整体 lower 仍被另一处缺陷挡住

本次想用「把真实编译器产物整体 lower，再编译生成的 C」作为无类型 store 覆盖面证据，
未能完成。原因**不是**本次改动：`build/bootstrap/lainc.l1` 含 12 处真实 `#eval`（按
§13.1 必须在进入 backend 前求值并消失），而 `build/bootstrap/compiler_core.l1` 虽无
`#eval`，仍被拒绝。二分定位到最小复现：

```lain
#proc multi_line_header(
  #addr %a, #bits<32> %b, #bits<64> %c
) -> #addr { #return %a }        // 拒绝，status 1
```

形参表跨物理行即被拒绝；同样的形参写在一行则通过。声明收集（无原型阶段）已经按括号
配平跨行处理，**过程定义**仍是逐行分派，因此两个真实产物都无法整体 lower
（`compiler_core.l1` 的第 23 个过程 `tool_diag_new` 是首个多行形参表）。
修改前的 backend 在同一输入上同样失败，属既有缺陷，本次未修，留给后续切片。

## 证明范围

没有据此宣称：浮点运算、`#bitcast`/`#proc_addr` 的 C lower、`#call_indirect` 的一致
lowering、`#alloca` 生命周期与零初始化、除零/溢出 Trap、native 编译器矩阵已完成。
`#call_indirect`、未知 `#load`/`#store` 宽度与未知语句仍是「留 C 注释、退出 0」的静默
通道，未在本次修改（见路线图 §13.2）。

`L1_signed` 与 `L1_store_auto` 的正确性依赖操作数的 C 类型恰好是八种定宽类型之一：
窄算术被整型提升为 `int` 时不再静默选错，而是编译失败，但该类表达式尚无正确 lower。
`L1_store_auto` 的宽度来自 C 类型，因此只对该backend 自己发出的值表达式成立。

