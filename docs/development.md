# 开发约定

## 目录

| 路径 | 用途 |
| --- | --- |
| `seed/` | C 实现的 LAINIR parser、验证器、装载器、VM、后端和宿主能力 |
| `bootstrap/` | LAINIR Meta 库 |
| `docs/` | 设计、规范与进度|
| `build/` | 二进制、临时输入、快照和日志，不提交 |

根 README 和 AGENTS 只保留导航及必要工作规则。

## 构建

### CMake：seed 库

```powershell
cmake -S . -B build/cmake -G Ninja -DCMAKE_C_COMPILER=clang
cmake --build build/cmake
```
