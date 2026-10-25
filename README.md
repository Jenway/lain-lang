# The Lain-Lang

PL Experiment Lain

Lain-Lang 是一个 Toy Language，WIP。

编译器内核只保留 LAINIR 与 LainVM；源语言的语法、类型与降级规则由 `bootstrap/` 里
Lain 写的 **Meta** 定义，Meta 本身跑在 LainVM 上。C++ 写的 `lain0` 负责把 bootstrap
编成 LAINIR。

## 文档

| 文档 | 内容 |
| --- | --- |
| [docs/README.md](docs/README.md) | 架构总览与文档索引 |
| [docs/development.md](docs/development.md) | 构建、驱动选项、回归与闸门 |
| [docs/plan-selfhost.md](docs/plan-selfhost.md) | 自举进度、剩余缺口与复现命令 |
| [docs/01-lain-ir.md](docs/01-lain-ir.md) | LAINIR |
| [docs/02-lain-vm.md](docs/02-lain-vm.md) | LainVM |
| [docs/03-lain-meta.md](docs/03-lain-meta.md) | Meta 的职责与 apply 契约 |
| [docs/04-bootstrap-dialect.md](docs/04-bootstrap-dialect.md) | bootstrap 方言与降级形状 |
| [docs/lain0.md](docs/lain0.md) | lain0 C++ 前端 |
| [docs/plan-modules.md](docs/plan-modules.md) | 模块命名空间与符号命名 |
| [docs/proposal-modules-v2.md](docs/proposal-modules-v2.md) | 每模块一份产出的设计与取舍 |
| [docs/decisions.md](docs/decisions.md) | 设计决策记录 |
| [docs/diagnostic-codes.md](docs/diagnostic-codes.md) | 诊断码号段 |

## 快速开始

```bash
cmake -S seed -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build
bash scripts/regress.sh
python scripts/build_meta_unit.py --check-twice
```

2026 Jenway
