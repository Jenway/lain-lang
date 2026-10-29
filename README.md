# The Lain-Lang

PL Experiment Lain

Lain-Lang 是一个 Toy Language，WIP。

编译器内核只保留 LAINIR 与 LainVM；源语言的语法、类型与降级规则由 `src/` 里
Lain 写的 **Meta** 定义，Meta 本身跑在 LainVM 上。C++ 写的 `lain0` 负责把 src
编成 LAINIR。

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
./build/lain0/lain0 --entry src/meta.lain --split build/units
./build/lain-meta --root "$PWD" --units-dir build/units --lower 0 --out build/arith.out examples/arith.lain
```

2026 Jenway
