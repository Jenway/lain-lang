# 自举路线

目标：LainVM 上的 Meta 降级 `bootstrap/` 自己的 28 个模块，产出能被 `lainir_parse` +
`lainir_verify` 接受的 LAINIR；最终让 Meta 编出的 Meta 再编一遍，产物与第一遍逐字节相同。

## 当前状态

- **lain0 编 bootstrap**：28 个模块 → **每模块一份** LAINIR，零诊断；两次运行整批指纹相同
  （`5298dc25…`，`python scripts/build_meta_unit.py --check-twice`）。
- **Meta 编样例**：38 个样例中 21 个通过。`jumps`、`loop`、`loop_if`、`loop_local`、
  `loop_value` 以 trap status 1004 结束；`modules_lib`、`modules_main`、`modules_twin`、
  `refs`、`strinline`、`symbol_addr`、`u8math` 返回非零 `host_status`；`cast`、`pointer`、
  `field_expr`、`locals`、`table` 的产物过不了 `lainir_verify`。
- **Meta 编 bootstrap 自己**：未通，基线 **0/28**。差集清单、每类缺口的触发条件、最小复现
  与推进顺序在 `docs/selfhost-gap.md`。

## 判据

每一步的验收都是 **`host_status=0` 且产物过 `lainir_parse` + `lainir_verify`**。只看
`host_status` 会把 trap 之后截断的产物当成通过（trap 时 `host_status` 仍是 0）。

## 两条编译路径

| 路径 | 谁编译谁 | 状态 |
| --- | --- | --- |
| lain0（C++）编 bootstrap | 28 个 Lain 模块 → **每模块一份** LAINIR | **已通**：零诊断、两次跑整批指纹相同、在 VM 上 `host_status=0` |
| Meta（Lain）编源码 | 样例 → LAINIR | 21/38 绿，17 红 |
| **Meta 编 bootstrap 自己** | 28 个单元 → LAINIR | **未通**，基线 **0/28** |

## 剩余缺口

自举要求 Meta 实现它自己源码用到的那部分语言。缺口分布在导入登记、跨模块成员、单模块
构造与循环体四类：

- **循环体的 load fault trap**：循环体里 `let` 初值含算子、操作数要靠名字查类型时，
  `std_types.meta_bytes_equal` 越界读。`loop` 族 5 个样例与 19 个 bootstrap 单元同一成因，
  是唯一一条以 bug 形态出现的缺口。
- **驱动区段上限**：`LAINVM_SPACE_MAX_REGIONS=64`，每份源码占 3 个区段，一次最多登记
  约 20 份；整份 bootstrap 的闭包有 28 个模块（62 条登记）。
- **导入登记语义**：Meta 的 import 匹配是「spec 规范化后与登记名逐字节相等」，bootstrap
  源码写的是相对导入方目录的 `import("types")`。登记名换成源码里的 spec 才能继续往下走。
- bootstrap 源码没有用到 `struct` / `enum` / `ref` / `slice` / `for` / 赋值语句 /
  本单元 `t[i]` / 函数体字符串字面量，这几类缺口不阻塞自举。

## 复现

```bash
cmake --build build
python scripts/build_meta_unit.py
bash scripts/regress.sh
bash scripts/regress.sh --round-trip
./build/lain-meta.exe --units-dir build/lain0/units/ \
    examples/arith.lain bootstrap/lain/examples/arith.lain
```

前两条回归要求 `PASS=38 FAIL=0`；arith 要求 `host_status=0`、`region grants=2`、
`output bytes=74`。Meta 编各样例的结果可以按 `build/check_meta.sh` 的方式重跑并比对
`build/baseline_out`。
