# 自举路线

目标：Meta 在 LainVM 上降级 `bootstrap/` 自己的 28 个模块，产物能被 `lainir_parse` +
`lainir_verify` 接受；最终让 Meta 编出的 Meta 再编一遍，产物与第一遍逐字节相同。

## 当前状态

以下数字是 2026-10-25 的实测。

- **lain0 编 bootstrap**：28 个模块 → 每模块一份 LAINIR，零诊断；两次运行整批指纹相同
  （`29ec2c2db1218cb1152b0d9ffd0fb04ca29e8537d57f6fca135aa74aaca26aed`，
  `python scripts/build_meta_unit.py --check-twice`）。合并装载
  `unit files=28 bytes=593524`、`verify subroutines=774 data=177`。
- **Meta 编样例**：38 个样例全部 `host_status=0` 且无 trap。产物单独过
  `lainir_parse` + `lainir_verify` 的有 35 个；`bytes` 与 `modules_main` 各有缺口（见下），
  `modules_twin` 与 `modules_twin_a` / `modules_twin_b` 的产物合并后通过。
- **Meta 编 bootstrap 自己**：单元闸门 `scripts/selfhost_gate.ps1` 当前通过
  `std/emit`（闭包 1）、`std/regions`（2）、`std/registry`（3）；`std/types`（2）产物校验
  失败，`std/lex`（3）降级失败；其余 23 个单元的 import 闭包都是全部 28 份源码，单次跑
  约 6 分 44 秒耗尽燃料、没有产物。

## 判据

每一步的验收都是 **`host_status=0` 且产物过 `lainir_parse` + `lainir_verify`**。只看
`host_status` 会把 trap 之后截断的产物当成通过（trap 时 `host_status` 仍是 0）。

## 两条编译路径

| 路径 | 谁编谁 | 状态 |
| --- | --- | --- |
| lain0（C++）编 bootstrap | 28 个模块 → 每模块一份 LAINIR | 已通：零诊断、两次指纹一致、合并装载 verify 通过 |
| Meta（Lain）编样例 | 样例 → LAINIR | 38/38 `host_status=0`；35 个产物过 verify |
| **Meta 编 bootstrap 自己** | 28 个单元 → LAINIR | 未通：3 个叶子单元通过，2 个叶子单元有缺口，23 个单元被燃料上限挡住 |

## 剩余缺口

阻塞程度从大到小：

1. **28 份源码一次登记超出燃料。** 单元闸门按 import 闭包登记源码，登记名由
   `--root bootstrap` 从源文件路径算出。`std/parse` 的闭包是 28/28：`std/parse` →
   `std/recognize` → `std/declarations` → `std/handlers/let` → `../meta`，再从
   `meta.lain` 回到全部模块。28 份源码共 583217 字节，驱动按规模推得 4e9 步燃料上限，
   实测 6 分 44 秒烧完、产物为空。28 个单元里只有 5 个的闭包不经过 `meta.lain`：
   `std/emit`（1）、`std/regions`（2）、`std/types`（2）、`std/lex`（3）、
   `std/registry`（3）；其余 23 个的闭包都等于全部 28 份。
2. **`return <loop>` 形态没有降级。** `std/lex` 的 `bs_skip_line` 函数体是
   `return loop scan(i: u64 = from) { ... };`。Meta 只有 `let x = loop ...` 的处理，
   在函数头之后立即报 `host_status=4`，产物截断。
3. **循环值与声明类型之间没有收窄。** `std/types` 的 `meta_bytes_equal` 里
   `let same: bool = loop ...`，产物把循环的结果发成 `#bits<64>`，而函数声明返回
   `bool`（`#bits<1>`），`#return` 处被 verify 报 2019
   （`#return value does not match the declared result`）。同一份产物里
   `bs_repr_size` 的 `#return 8` 也是没有按声明宽度发出的字面量。
4. **顶层字符串绑定的数据符号多一层 `_storage`。** 样例 `bytes` 的
   `let greeting: addr = "hi";` 发出 `data lain_examples_bytes__greeting_storage_storage`，
   过程里引用的是 `%lain_examples_bytes__greeting_storage`，verify 报 2002
   （未定义的值）。
5. **被降级单元自己的顶层值绑定不发模块前缀。** 样例 `modules_main` 引用
   `lain_examples_modules_lib__answer` / `__twice` / `__base`，而被导入模块自己降级出来
   的过程名是 `answer()` / `twice()` / `base()`。bootstrap 的顶层绑定只有静态数据与函数，
   暂时不触发这条。

## 复现

```bash
cmake --build build
python scripts/build_meta_unit.py --check-twice      # 28 份 + 整批指纹跑两次一致
bash scripts/regress.sh                              # lain0 全量回归
bash scripts/regress.sh --round-trip
powershell -NoProfile -File scripts/selfhost_gate.ps1
powershell -NoProfile -File scripts/selfhost_gate.ps1 -Only std/regions
./build/lain-meta.exe --units-dir build/lain0/units/ --root bootstrap \
    --lower 0 --out build/arith.out bootstrap/lain/examples/arith.lain
```

`--check-twice` 要求整批指纹两次一致，两条 `regress.sh` 要求 `PASS=38 FAIL=0`；
arith 要求 `host_status=0`、`region grants=2`、`output bytes=74`。单元闸门需要
`build/check_unit.exe`，编译命令见 `docs/development.md`。两条编译路径的逐份差分用
`python scripts/diff_two_compilers.py` 观测。
