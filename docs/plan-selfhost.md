# 自举路线

目标：LainVM 上的 Meta 降级 `bootstrap/` 自己的 28 个模块，产出能被 `lainir_parse` +
`lainir_verify` 接受的 LAINIR；最终让 Meta 编出的 Meta 再编一遍，产物与第一遍逐字节相同。

## 当前状态

- **lain0 编 bootstrap**：28 个模块 → `bootstrap.l1` 524574 字节，`verify subroutines=709
  data=167`，零诊断；两次运行的产物哈希相同。
- **Meta 编样例**：38 个样例中 26 个通过。`jumps`、`loop`、`loop_if`、`loop_local`、
  `loop_value` 以 trap status 1004 结束；`modules_lib`、`modules_main`、`modules_twin`、
  `refs`、`strinline`、`symbol_addr`、`u8math` 返回非零 `host_status`。
- **Meta 编 bootstrap 自己**：尚未打通。差集清单、每类缺口的触发条件、最小复现与推进
  顺序在 `docs/selfhost-gap.md`。

## 判据

每一步的验收都是 **`host_status=0` 且产物过 `lainir_parse` + `lainir_verify`**。只看
`host_status` 会把 trap 之后截断的产物当成通过（trap 时 `host_status` 仍是 0）。

改动 Meta 本体后必须复跑：

```bash
bash scripts/regress.sh
bash scripts/regress.sh --round-trip
python scripts/build_meta_unit.py
./build/lain-meta.exe --unit build/lain0/bootstrap_unit.txt \
    arith.lain bootstrap/lain/examples/arith.lain
```

前两条要求 `PASS=38 FAIL=0`；arith 要求 `host_status=0`、`region grants=2`、
`output bytes=74`。

## 两条编译路径

| 路径 | 谁编译谁 | 现状 |
| --- | --- | --- |
| lain0（C++）编 bootstrap | 28 个 Lain 模块 → LAINIR | 已通 |
| Meta（Lain）编样例 | 样例 → LAINIR | 26/38 |
| Meta 编 bootstrap 自己 | 28 个单元 → LAINIR | 未通 |

自举要求 Meta 实现它自己源码用到的那部分语言。缺口分布在导入登记、跨模块成员、单模块
构造与循环体四类，杠杆最高的是循环体的 load fault trap（`loop` 族 5 个样例与 19 个
bootstrap 单元同一成因）。
