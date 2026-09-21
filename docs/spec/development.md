# 开发约定

## 目录

| 路径 | 用途 |
| --- | --- |
| `seed/` | C 实现的 LAINIR parser、验证器、装载器、VM、后端、fold 和宿主能力 |
| `bootstrap/` | 手写 LAINIR Meta 与初代 prelude |
| `scripts/` | 构建与验收入口 |
| `docs/` | 设计、规范、进度与历史文档 |
| `build/` | 二进制、临时输入、快照和日志，不提交 |

根 README 和 AGENTS 只保留导航及必要工作规则。

## 构建

从仓库根执行：

```powershell
python scripts/build.py
python scripts/check_meta.py
```

文档修改另运行 `python scripts/check_docs.py`，检查集中存放规则、本地链接和代码围栏。

构建器优先选择 zig cc，其次 clang、gcc/cc，构建 Meta 驱动并执行 `meta_for.lain` 冒烟。
预期 `main = 10`。退出码 0 为通过、1 为失败、2 为缺前置。
不要运行根目录的旧 `build.py`；它的根路径计算和清单设置不适用于当前目录。

## 单条 Meta 用例

PowerShell 示例：

```powershell
$env:LAIN_META_MANIFEST = 'bootstrap/SOURCE_ORDER'
build/meta_boot.exe seed/tests/meta_body_chain.lain main 12 '' - std/prelude.lain=bootstrap/lain/std/prelude.lain
```

命令格式：`meta_boot <源码> <入口> <期望值> [断言文本] [模式] [逻辑路径=文件]...`。
Linux 下可执行文件为 `build/meta_boot`。手动运行必须设置 manifest；默认回退路径已经失效。
prelude 与 import 模块通过逻辑路径显式注册，额外语料位于 `seed/tests/modules/`。

| 模式 | 行为 |
| --- | --- |
| `-` | 执行并断言返回值 |
| `tree` / `types` | 输出语法树 / 类型注册表 |
| `canonical` | 输出规范 LAINIR |
| `c` | 输出 C，不执行 |

对新增数值用例再传一次错误期望，例如将上面的 12 改成 99，确认退出失败。
拒绝用例同时断言具体诊断码；必要时断言产物中的物理指令。

## 回归

改动前后分别构建并记录：

```powershell
python scripts/check_meta.py snapshot build/before.json
# 修改并重新运行 python scripts/build.py
python scripts/check_meta.py snapshot build/after.json
python scripts/check_meta.py compare build/before.json build/after.json
```

快照覆盖现有 `.lain` 语料的 stdout 和退出码，只过滤 bootstrap、caps、meta、scratch 四类计数行。
新增能力还须加入 `scripts/check_meta.py` 用例表。已有输出一致只证明回归范围内未改变行为。
需要隔离旧版本时可以在 `build/` 下建立 worktree，必须记录实际基线，不能遗漏未提交改动。

VSpace 专项工作的入口和未完成项见 [进度索引](../implementation/README.md)。
`.github/workflows/lain-bootstrap.yml` 仍引用旧脚本，不能作为当前验收依据。

## 修改限制

- 语言语义放在 Meta。只有指令语义或宿主能力确实缺失时才修改 C seed。
- VSpace 的分配、地址模型等决策仍未完成：不要修改 `seed/src/vm/**`，也不要自行加区段绕过问题。
  先给出最小复现和两个方案，经作者确认后按批准范围实施。
- 生成物只落 `build/`，不手工修改产物让检查通过。
- `SOURCE_ORDER` 固定拼接顺序，同一份源码清单必须产生逐字节相同的模块。
- `.l1` 和 `.lain` 使用 LF；代码注释及提交信息使用中文，提交信息带实测数值或产物文本。
- 源码内存须显式授权；不要用扩大授权掩盖指针或暂存区布局错误。
- 暂存区布局以 `bootstrap/std/scope.l1` 文件头为准；递归发射中的共享临时状态必须保存或独立分配。

这些规则同时适用于人工与代理修改。作者已经明确批准的后续动作不重复请求确认。
