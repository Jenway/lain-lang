# Meta 管线

`bootstrap/` 下的 28 个 Lain 模块构成 Meta。入口 `bootstrap/meta.lain` 顺 import 求闭包，
`lain0 --entry bootstrap/meta.lain --split build/lain0/units/` 把它们编成每个模块一份
LAINIR；`lain-meta` 用 `--units-dir` 装载整个目录，用 Meta 降级给定的表面源码，产出
canonical LAINIR。

## 组成部分

- `bootstrap/meta.lain`：入口。模块集合由它顺 `import("…")` 求闭包得到（实测 28 个）。
  三个入口 `lain_std_abi_version` / `lain_std_initialize` / `lain_std_lower` 带 `@root`
  标注，产物里保持平坦名字。
- `scripts/build_meta_unit.py`：调 `lain0 --entry bootstrap/meta.lain --split
  build/lain0/units/`，产出 `build/lain0/units/*.l1`（28 份，文件名 = 模块键）；
  `--check-twice` 连跑两次并比较整批指纹。
- `seed/meta_driver.c`：驱动。装入单元、登记宿主能力、把源码交给 `lain_std_lower`，
  打印执行摘要。
- `seed/kernel/src/meta/host.c`、`seed/kernel/src/meta/apply.c`：宿主能力与 Meta 的
  apply 侧实现。

## 构建与运行

```bash
cmake -S seed -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build
python scripts/build_meta_unit.py      # 入口 -> 每模块一份 LAINIR（28 份，文件名 = 模块键）
./build/lain-meta.exe --units-dir build/lain0/units/ \
    examples/arith.lain bootstrap/lain/examples/arith.lain
```

位置参数成对给出「逻辑路径 源文件」。逻辑路径是 import 解析注册表里的名字，与源文件路径
分开：`examples/arith.lain` 是注册名，`bootstrap/lain/examples/arith.lain` 是实际文件。

判据是 `host_status=0` 且产物过 `lainir_parse` + `lainir_verify`。驱动摘要行形如
`unit files=N bytes=N`、`verify subroutines=N data=N`、`run slice=N rounds=N steps=N
host_status=N`、`region grants=N`、`output bytes=N`，脚本直接断言这些数字。

## 当前状态

- `arith.lain`：`host_status=0`、`region grants=2`、`output bytes=74`，产物是
  `%r1 = #add[#bits<32>](1, 2)`。
- 全部 38 个样例中 26 个通过。`jumps`、`loop`、`loop_if`、`loop_local`、`loop_value`
  执行以 VM trap status 1004（load fault）结束、产物被截断；`modules_lib`、`modules_main`、
  `modules_twin`、`refs`、`strinline`、`symbol_addr`、`u8math` 返回非零 `host_status`。
- 诊断记录、差集统计与推进顺序在 `docs/selfhost-gap.md`。

## 约束

- 整份 Meta 由入口 `bootstrap/meta.lain` 顺 import 编成，不经过拼接，也没有模块清单。
- 模块键与跨模块引用规则见 `docs/04-bootstrap-dialect.md` 的《模块：成员位置与符号命名》。
- `symbol_addr` 只取本模块的静态数据，名字要带生成后缀 `_storage`。
- `ref`、`rw` 不能作形参名（保留字）。
