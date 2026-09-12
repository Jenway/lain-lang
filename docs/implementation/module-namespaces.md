# 模块命名空间与 import 解析

Lain 源码中的 `import("...")` 字符串不是文件系统路径，而是一段**逻辑路径**。本文记录
当前实现实际使用的映射与解析规则，以及一条尚未解决的歧义风险。

## 1. 两套命名空间

| 前缀 | 含义 | 解析方式 |
| --- | --- | --- |
| `std::...` | 正式标准库，位于 `std/` | 精确路径匹配 |
| `packages::lain::...` | 编译器与运行时实现，位于 `src/` | 仅按**路径末段**匹配 |

`packages::lain::` 是唯一出现的包前缀，共 24 个源文件在用。

## 2. `packages::lain::` → `src/` 映射表

| 逻辑路径 | 物理文件 |
| --- | --- |
| `packages::lain::compiler::X` | `src/lainc/X.lain` |
| `packages::lain::lainir::api_contract` | `src/lainir/api_contract.lain` |
| `packages::lain::lainir::api::X` | `src/lainir/api/X.lain` |
| `packages::lain::lainvm::X` | `src/lainvm/X.lain` |

注意 `compiler::` 对应目录 `src/lainc/`（不是 `src/compiler/`）。

`std::` 映射为 `std/`，`::` 换成 `/`，省略 `.lain` 后缀：

| 逻辑路径 | 物理文件 |
| --- | --- |
| `std::core::vec` | `std/core/vec.lain` |
| `std::platform::memory` | `std/platform/memory.lain` |
| `std::memory_model` | `std/memory_model.lain` |

## 3. 解析规则（bootstrap 实现）

实现在 `bootstrap/compiler/workspace.l1`，由 `meta_collect.l1` 与 `workspace_cache.l1`
调用：

1. `workspace_import_is_std`（`workspace.l1:73-87`）逐字节判断 import 串是否以
   `"std::` 开头。
2. **std 分支** — `workspace_std_path_matches`（`workspace.l1:160-241`）：把 `std::`
   之后的逻辑路径与源文件路径中 `std/` 之后的相对部分**逐字节比较**，`::` 对应路径
   分隔符，忽略 `.lain` 后缀。
3. **非 std 分支** — `workspace_suffix_matches`（`workspace.l1:93-154`）：取 import 串
   **最后一个 `:`** 之后的子串（去掉可选 `.lain`），与候选源文件路径的**最后一个路径
   分量**比较。**`packages::lain::...` 这一段前缀完全不参与比较。**
4. `workspace_import_target`（`workspace.l1:314-364`）按源索引升序扫描，**第一个**匹配
   者胜出；无匹配返回 `source_count` 哨兵，表现为未解析 import（诊断 `4101`）。

源码注释确认了这一设计意图（`workspace.l1:89-92`）：

> Match an import string (including its quotes) against the final component
> of a source path.

而 `std::` 走精确匹配，正是为了避免 `std::core::memory` 与 `std::platform::memory`
塌缩到同一 basename（`workspace.l1:156-159` 的注释）。

## 4. 后果：同一闭包内的 basename 必须唯一

因为非 std 导入只比较末段，**任何两个非 `std::` 源文件同名，其 import 串就无法区分**。

`composed_compiler_sources()`（`scripts/lainc_sources.py:77-99`）当前闭包共 47 个文件
（2026-09-12 归档 6 个形式实现后从 53 减少），存在 3 组重复 basename：

| basename | 文件 | 是否可区分 |
| --- | --- | --- |
| `api_contract.lain` | `src/lainir/api_contract.lain`、`src/lainvm/api_contract.lain` | **不可区分（两侧都走末段匹配）** |
| `meta.lain` | `std/meta.lain`、`src/lainc/meta.lain` | 可区分：`std::meta` 走精确匹配 |
| `memory.lain` | `std/core/memory.lain`、`std/platform/memory.lain` | 可区分：两侧都是 `std::` |

### 4.1 已证实的同名项：`api_contract`

闭包顺序为 `stdlib_sources`（排序后的 `std/**/*.lain`）+ `src/lainir/api/SOURCES.txt`
+ `src/lainvm/SOURCES.txt` + `src/lainc/COMPILER_SOURCES.txt`（`scripts/lainc_sources.py:77-99`）。
`src/lainir/api_contract.lain` 位于 `src/lainvm/api_contract.lain` **之前**。

据此，`import("packages::lain::lainvm::api_contract")` 的末段扫描会先命中
`src/lainir/api_contract.lain`。该文件导出 `CapabilitiesShape` / `Contract`；而
`ExecutionShape` 只存在于 `src/lainvm/api_contract.lain`。

**已证实的后果**：`src/lainc/compiler.lain`、`compiler_api.lain`、`compiler_core.lain`、
`compiler_driver.lain`、`meta.lain` 中的 `lainvm_api` 绑定**指向 LAINIR 契约而非 LAINVM 契约**。

**实测结论（2026-09-12，运行时探针）**：

1. **`packages::lain::lainir::` 与 `packages::lain::lainvm::` 前缀不参与解析——只有最后一段
   basename 起作用。** 决定性实验：只提供 `src/lainir/api_contract.lain` 而
   `import("packages::lain::lainvm::api_contract")` → **成功**；反向（只提供
   `src/lainvm/api_contract.lain` 而 import `packages::lain::lainir::api_contract`）→ 也成功。
   两者都只用 basename 匹配，目录层级被忽略。
2. 因此本闭包内 `import("packages::lain::lainvm::api_contract")` **确实解析到
   `src/lainir/api_contract.lain`**（首匹配，索引 26 < 32）。
3. **但该误绑定目前不可观测**：模块成员引用（无论值位置还是类型位置）**不做校验**。
   实测 `x: vm_api.ExecutionShape`（仅 LAINVM 版有）与 `x: vm_api.NoSuchMemberAtAll`（两版都
   没有）**产物完全相同**——形参都变成 `#addr`。所以指向哪一份契约在行为上无差别。

**顺带查实的一处更广的限制**：既然成员引用不校验，**成员名拼错会被静默接受**。这既掩盖了
上面的歧义，也是一个独立的已知缺口（要修需在 elaborator 里做成员解析，属编码 5 之后的范围）。

现有门禁无法发现歧义：`scripts/check_lainvm_boundary.py` 断言的是**源码文本**
（`"packages::lain::lainvm::api_contract" in source`），与真实解析结果无关。

按 `docs/roadmaps/lain-roadmap.md` §15，是否给中间层级加真实解析属「必须先讨论」范围；
**在当前"成员引用不校验"的前提下，不动它不会造成可观测错误。**

## 5. 写新源码时的约束

- **不要引入与闭包内已有文件同名的非 std 源文件。** 新增 `src/**/*.lain` 前，先确认
  其 basename 未出现在 `std/`、`src/lainir/api/`、`src/lainvm/`、`src/lainc/` 中。
- 新增 `src/` 下的模块后，必须登记进对应的 SOURCES 清单（`COMPILER_SOURCES.txt`、
  `src/lainir/api/SOURCES.txt`、`src/lainvm/SOURCES.txt`），清单顺序即语义顺序。
  仅 `std/**/*.lain` 由 `rglob` 自动收集并排序。
- 导入 `std::` 时按完整逻辑路径书写；导入 `src/` 时逻辑路径的**最后一段必须等于文件名**。
- 不要依赖 `packages::lain::` 中间层级做语义区分——它们当前不参与解析。
