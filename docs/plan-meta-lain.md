# Meta 管线

`bootstrap/` 下的 28 个 Lain 模块构成 Meta。`lain0 --unit bootstrap/SOURCE_ORDER` 把它们编成
一份 LAINIR；`lain-meta` 把这份单元装进 LainVM，用 Meta 降级给定的表面源码，产出 canonical
LAINIR。

## 组成部分

- `bootstrap/SOURCE_ORDER`：模块清单，每行 `<相对路径> [<根名> …]`，路径省略 `.lain`，顺序
  即降级顺序。`meta` 的三个入口 `lain_std_abi_version` / `lain_std_initialize` /
  `lain_std_lower` 标成根名，产物里保持平坦名字。
- `scripts/build_meta_unit.py`：调 `lain0 --unit bootstrap/SOURCE_ORDER` 产出
  `build/lain0/bootstrap.l1`，并写出驱动要的单元清单 `build/lain0/bootstrap_unit.txt`
  （清单里写 `../build/...`，驱动会把每行当作 `bootstrap/<行>` 打开）。
- `seed/meta_driver.c`：驱动。装入单元、登记宿主能力、把源码交给 `lain_std_lower`，
  打印执行摘要。
- `seed/kernel/src/meta/host.c`、`seed/kernel/src/meta/apply.c`：宿主能力与 Meta 的
  apply 侧实现。

## 构建与运行

```bash
cmake -S seed -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build
python scripts/build_meta_unit.py
./build/lain-meta.exe --unit build/lain0/bootstrap_unit.txt \
    arith.lain bootstrap/lain/examples/arith.lain
```

位置参数成对给出「逻辑路径 源文件」。逻辑路径是 import 解析注册表里的名字，与源文件路径
分开：`arith.lain` 是注册名，`bootstrap/lain/examples/arith.lain` 是实际文件。

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

- 一次 `lain0` 调用只接受单个输入文件；整份 Meta 由 `--unit` 与清单一次编成，不经过拼接。
- 模块键与跨模块引用规则见 `docs/04-bootstrap-dialect.md` 的《模块：成员位置与符号命名》。
- `symbol_addr` 只取本模块的静态数据，名字要带生成后缀 `_storage`。
- `ref`、`rw` 不能作形参名（保留字）。
