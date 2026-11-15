# The Lain-Lang

PL Experiment Lain

Lain-Lang 是一个 Toy Language，WIP。

编译器内核只保留 LAINIR 与 LainVM；源语言的语法、类型与降级规则由 `src/` 里
Lain 写的 **Meta** 定义。Meta 本身也由 `lain0` 编译成 LAINIR，再跑在 LainVM 上
把用户的 `.lain` 源码降级成 LAINIR。

## 文档

| 文档 | 内容 |
| --- | --- |
| [docs/README.md](docs/README.md) | 架构总览与文档索引 |
| [docs/01-lain-ir.md](docs/01-lain-ir.md) | LAINIR |
| [docs/02-lain-vm.md](docs/02-lain-vm.md) | LainVM |
| [docs/03-lain-meta.md](docs/03-lain-meta.md) | Meta 的职责与 apply 契约 |
| [docs/stdlib/concepts.md](docs/stdlib/concepts.md) | 标准库概念 |
| [docs/stdlib/effects.md](docs/stdlib/effects.md) | 标准库效应 |

## 快速开始

```bash
cmake -S seed -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build
for f in examples/*.lain; do ./build/lain0/lain0 --round-trip "$f"; done
./build/lain0/lain0 --entry src/entry.lain --split build/units
./build/lain-meta-v2 build/units/*.l1 examples/arith.lain --out build/arith.l1
./build/lainir-verify build/arith.l1
./build/lainir-run --entry s0__total build/arith.l1
```

2026 Jenway
