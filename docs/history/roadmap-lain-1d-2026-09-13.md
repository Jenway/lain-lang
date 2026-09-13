# 编码 1d：bootstrap Meta 边界与执行路径审计

日期：2026-09-13。当前计划见 [`../roadmaps/lain-roadmap.md`](../roadmaps/lain-roadmap.md)。
本快照只完成边界、现有路径与基线审计，不宣称编码 1e–1h、正式运行组合或固定点完成。

## 1. 实际产物归属

`scripts/build_lain_compiler.py` 的 CORE_MODULES 与 BOOTSTRAP_STD_MODULES 决定归属，
文件目录不能替代产物证据。`meta_call.l1`、`meta_call_vm.l1`、`meta_eval_vm.l1`、
`meta_collect.l1`、`meta_values.l1`、`lower_program.l1` 等语言实现位于旧 compiler 目录，
但实际打入 `build/bootstrap/stdlib.l1`，不在 `compiler_core.l1`。
`check_lainir_boundaries.py` 本次退出 0。当前 core 通过 Meta ABI 编排通用阶段。
因此整理前“这些 builder 属于 compiler core，需要整体搬进库”的归属判断不准确。

## 2. 路径与调用证据

共同入口：`meta_collect.l1::program_collect_module` 在声明收集时调用
`meta_call.l1::program_collect_meta_call_candidate`。后者依次尝试 operation、module、
effect、scalar；这些是当前库内部的专用源码识别与生成路径，尚未完整 lower 原函数体。

| 路径 | 调用/生成链 | 既有 fixture |
| --- | --- | --- |
| arithmetic | `lainvm_eval_consteval_group` → consteval writer → capture → parse/find → vm-eval-bits | `formal_consteval_arithmetic.lain` |
| scalar | candidate → `lainvm_eval_meta_scalar_call` → `lainvm_meta_build_scalar_artifact` → `lainvm_meta_run_artifact` | `formal_meta_scalar_call.lain` |
| module | candidate → `lainvm_meta_module_factory_group` → `lainvm_eval_meta_module_call` → module builder → shared runner | `formal_meta_module_factory.lain` |
| effect | candidate → `lainvm_meta_effect_factory_node` → `lainvm_eval_meta_effect_call` → effect builder → shared runner | `formal_meta_effect_factory.lain` |
| operation | candidate → `lainvm_eval_meta_effect_operation` → operation builder → shared runner | 同上 |

scalar writer 生成目标过程与 `meta_entry` 中的显式 `#eval`；其余三个 builder 使用共同
描述值骨架，生成 addr 返回的目标过程及含 `#eval` 的 entry。arithmetic 有独立 runner，
生成含 `#eval` 的 `meta_eval`。上述五条当前路径均未发现绕过 `#eval` 的源语言求值。
这不证明所有未来语义已覆盖，也不把底层可执行普通 procedure 的物理 API 误认为违规。

每条 wrapper 的实际直接依赖、源文件归属、生成 IR 的 extern 与 SHA-256 由检查报告记录。
底层链：`seed/src/host/bootstrap.c::vm_eval_value` 调用
`seed/src/interpreter/eval_source.c::lainir_module_handle_run`，验证 entry 后建立临时 VM 控制流
并运行物理过程。共享 VSpace、预算和地址生命周期尚需独立契约验证，未由本审计证明。
`bootstrap.eval_source` / `bootstrap.eval-next` 在宿主注册，当前 bootstrap/std/src 的 .l1/.lain
源码无调用者；正式 `src/lainc/meta.lain` 的 Vm.eval 契约适配仍属编码 5。

## 3. 动态探针与局限

新增 `scripts/check_meta_pipeline_audit.py`，本次退出 0。
先重建当前 bundle；每例由原 bundle 编译、通过 verifier、运行到 42。
然后只修改临时探针 bundle，截获选定 wrapper 的 capture，写出生成 IR 并除零 Trap。
探针 bundle 先通过 verifier，且要求实际出现 sdiv Trap，保证捕获不是解析失败或未调用路径。
五份实际生成的 IR 均通过对应 entry verifier，含显式 `#eval`。
探针不修改源码或权威 cache，不作为修复构建的手段；所有输出只落 build。

机器可读报告与生成 IR 在 `build/meta-pipeline-audit/`，包含 bundle 和每份生成 IR 的 hash。
报告明确保留限制：effect fixture 的 main 直接返回 42，不消费 effect/operation 的语义；
捕获探针在执行前故意停止。两者不能证明 handler 行为、完整函数体 lowering 或共享 VSpace。
这些正是编码 1f/1g 后续需要建立的运行证据。

## 4. 阶段数据流缺口与最小迁移闭包

`compiler_api.l1::compiler_compile_request` 已读取 expand/elaborate 的 result 并交给下一阶段。
但 `bootstrap/std/entry.l1::lain_std_elaborate` 回传输入，`lain_std_lower` 忽略 elaborated_root，
仍传 syntax_index 给 `lower_program.l1::lain_std_lower_program`。
后者把声明/类型/导入收集、语义验证与物理发射合并在一个过程。因此 ABI 接缝存在、语言代码
属于 stdlib，却没有完整的阶段结果交接；改名字或目录不能填补缺口。

1e 的最小工作闭包首先是：

- `bootstrap/std/entry.l1`：建立并传递真正的库阶段结果。
- `bootstrap/compiler/lower_program.l1`：分离收集/验证与发射，保留错误优先级及资源所有权。
- `bootstrap/compiler/meta_collect.l1`、`meta_values.l1`、`meta_call.l1`、`meta_call_vm.l1`：
  保持现有环境、输入和编译期计算依赖；随后替换受限 body 识别/生成，不能在 core 加专用协议。
- `bootstrap/compiler/compiler_api.l1` 与 `compiler_context.l1`：只审计通用结果/资源交接，
  不新增语言构造或分类；如现有 ABI 足够，core 不改。
- `scripts/check_meta_stage_swap.py`：新增实际阶段结果被下一阶段消费的负对照。

类型与 effect 的 kind 4 重用在 `meta_values.l1`、输入检查、路径成员查询与 builder 的存储中
都有消费者，属于库内部表示；不新增 VM kind。错误签名的 effect fixture 仍须在库规则完善后
形成正反例。新 capability 如需跨越现有 ABI，先给最小复现；本审计未证明必须扩 ABI。

## 5. 本次生成产物指纹

Bundle SHA-256：`8a0d4f1be53bfa563bace65e48425e9f1f97995d7a539de7ff78a100d4c94767`。

- arithmetic：`644588c2157bf4dd25cfcaa9ea76ceddd40359bf2e81ad59c98aa00eaad3947a`。
- scalar：`8e9a835d3fb1aaed29dfb5bb56468746312b1d638247cc5e84e1c450f869901a`。
- module：`52c8dec0944cb45bcb3cac028e6b1d04340fda4717f9fc1784d503a3d49a9be7`。
- effect：`a76c8ae09992a5aa521717e3253fa40787f8f737f899004f317e4461b5a8afff`。
- operation：`d225a98f0796071115c14e2bed2ba47bb8ef843f227b3e146a8bd3d1ac4613f2`。
