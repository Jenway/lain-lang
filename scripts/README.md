# scripts 索引

`scripts/` 是一个**扁平目录**：没有 `__init__.py`，也没有包管理器配置。每个脚本都是独立程序，直接以仓库根为 cwd 调用：

```text
python scripts/<name>.py [args...]
```

约定：

- 脚本用 `ROOT = Path(__file__).resolve().parents[1]` 定位仓库根，因此**必须从仓库根运行**。
- 退出码：`0` = 通过，`1` = 检查失败，`2` = 前置产物缺失（例如还没有 `build/seed/bin/` 下的 seed 可执行文件，或 `build/` 下的其它生成物）。
- 所有生成物（bundle、snapshot、报告、可执行文件）一律写入 `build/`，不作为源码提交；`seed/` 只保留源码，不再产生任何生成物。

## 依赖顺序

生成步骤必须按顺序完成，依赖它们的 check 才能通过：

1. `python scripts/build_seed.py` — 唯一 Zig 构建入口：在 `seed/` 内运行 `zig build`，但把 Zig 缓存与安装前缀都重定向到仓库根，产出 `build/seed/bin/{lainir-seed, lainir-print, lainir-lsp, lainir-vm-control-test}`（Windows 下带 `.exe`）。可选 `--optimize Debug|ReleaseSafe|ReleaseFast|ReleaseSmall`。
2. `python scripts/build_lain_compiler.py` — 产出 `build/bootstrap/*`（`lainc.l1`、`compiler_core.l1`、`stdlib.l1` 及对应 stamp/manifest）。
3. `python scripts/build_formal_stdlib.py` — 产出 `build/lainir/formal_stdlib.l1` 及其 manifest。

依赖 2/3 产物的 check（stdlib、snapshot、policy、snapshots 等）必须先跑对应的生成步骤；只读源码的 check 可以单独运行。

### `toolchain.py` 与 `build_seed.py`

- `scripts/toolchain.py` — 构建产物路径的**唯一事实来源**：导出 `ROOT`、`BUILD`、`SEED_BIN`（`build/seed/bin`）、`SELFHOST_BIN`（`build/selfhost/bin`），以及 `seed_exe(name)` / `selfhost_exe(name)`（在 Windows 上自动补 `.exe`）。
- `scripts/build_seed.py` — **唯一**的 Zig 构建入口。它强制 `ZIG_LOCAL_CACHE_DIR=build/zig-cache`、`ZIG_GLOBAL_CACHE_DIR=build/zig-cache-global`，以 `--prefix build/seed` 在 `seed/` 中运行 `zig build`，退出码透传，成功后打印安装目录。

**不要在脚本里再硬编码 seed 目录下的 `zig-out` 输出前缀（旧路径，已废弃）。** seed 可执行文件一律通过 `toolchain.py` 的 `seed_exe()` 取，自举编译器一律通过 `selfhost_exe()` 取；新脚本也不得自行调用 `zig build`，只调用 `build_seed.py`。

## `build/` 与 `target/` 布局

两者都在 `.gitignore` 内，不是源码。**除下表列出的路径外，其余内容都是可安全删除的临时产物。**

`build/`：

| 路径 | 内容 | 由谁生成 |
| --- | --- | --- |
| `bootstrap/` | `lainc.l1`、`compiler_core.l1`、`stdlib.l1` 及 `*.stamp.json`、`stdlib.manifest.json` | `build_lain_compiler.py`、`freeze_lainc_bootstrap.py` |
| `lainir/` | `formal_stdlib.l1` + manifest + `formal_stdlib_abi_probe.l1`、`srclainc.l1` | `build_formal_stdlib.py`、`build_srclainc.py` |
| `native-lainc-matrix/` | 矩阵 gate 的中间产物 | `check_native_lainc_matrix.py` |
| `baselines/` | 基线报告（含每项 gate 的 stdout/stderr） | `capture_lain_bootstrap_baseline.py` |
| `profiles/` | 分阶段性能报告 | `profile_lainc_bootstrap.py` |
| `release/`、`install/` | 打包产物与版本化安装前缀 | `package_lainc_bootstrap.py`、`install_lainc_bootstrap.py` |
| `seed/` | `bin/{lainir-seed, lainir-print, lainir-lsp, lainir-vm-control-test}`：seed 工具链可执行文件 | `build_seed.py` |
| `selfhost/` | `bin/lainir-compiler`：自举得到的 native LAINIR 编译器 | `run_lainir_self_host.py` |
| `zig-cache/`、`zig-cache-global/` | 被 pin 到工作区内的 Zig 缓存（所有 Zig 消费者共用） | `build_seed.py`、`run_lainir_self_host.py`、`build_lainc_native.py`、`check_lainir_compiler.py`、`check_native_backend_migration.py` |
| `backend_c_entry.l1`、`lainc-native.c`、`lainc.exe` | 后端与 native 编译器产物 | `run_lain_backend.py`、`build_default_lainc.py` |

`target/`：

| 路径 | 内容 |
| --- | --- |
| `zig-cache/` | **遗留路径**：旧检出里由脚本 pin 到工作区内的 Zig 缓存。所有脚本已统一改用 `build/zig-cache{,-global}`，此目录不再被任何脚本创建，可安全删除；`.gitignore` 仍忽略它，避免旧残留重新出现在 diff 中。 |

注意：

- `build/` 目录**本身必须存在**：`check_native_backend_migration.py` 与 `check_native_backend_canonical_diff.py` 会在其下建临时目录。
- `build/lainir/srclainc.l1` 重建耗时较长（分钟级），删除前请确认可以接受重跑成本。
- Zig 缓存目录由 `build_seed.py` 用环境变量强制指定；`target/zig-cache/` 只可能来自旧检出。删除缓存只会导致下次构建变慢，不影响正确性。
- 手工调试留下的临时文件（`build/*.log`、`build/tmp_*.l1`、`target/*.exe` 等）不属于任何脚本的约定输出路径，可随时清理。

## 推荐入口

- `python scripts/check_lainc_lainir_api_baseline.py` — 主入口：顺序执行 15 道 lainc → LAINIR API 迁移 gate，任一失败即以该 gate 的退出码结束。
- `python scripts/capture_lain_bootstrap_baseline.py` — 采集可复现的机器可读 bootstrap 基线报告（同时记录成功与失败的 gate）。

## 脚本索引

### `build_*`

| 脚本 | 作用 |
| --- | --- |
| `build_seed.py` | 唯一的 Zig 构建入口：把 seed 可执行文件装到 `build/seed/bin/`，缓存写入 `build/zig-cache{,-global}`。 |
| `build_lain_compiler.py` | 从 `bootstrap/compiler` 源码构建第一版可执行的 Lain → LAIN-IR 编译器包，写入 `build/bootstrap/lainc.l1`。 |
| `build_formal_stdlib.py` | 编译面向用户的 `std/**/*.lain`，生成与 bootstrap stdlib 分离的 `build/lainir/formal_stdlib.l1` 及 manifest。 |
| `build_default_lainc.py` | 从已生成的 bootstrap bundle 构建 native lainc。 |
| `build_lainc_native.py` | 从生成好的 canonical-L1 compiler 构建 native lainc 可执行文件。 |
| `build_srclainc.py` | 使用显式选择的 LAINIR capability provider 构建 `src/lainc`。 |

### `check_*`

| 脚本 | 作用 |
| --- | --- |
| `check_lainc_lainir_api_baseline.py` | 主入口：顺序执行 15 道 lainc → LAINIR API 迁移 gate。 |
| `check_lainc_lainir_api.py` | 检查源码层面的 lainc → LAINIR capability 边界（`--final` 时切换为移除 gate）。 |
| `check_lainc_lainir_api_snapshots.py` | 用 checked-in canonical artifact 基线守护 formal compiler 的回归。 |
| `check_backend_manifest.py` | 检查 backend manifest 的 logical capability 分类。 |
| `check_backend_abi_contract.py` | 保持 `BackendShape`、ABI 文档与源码清单同步。 |
| `check_seed_backend_adapter.py` | 检查 seed/native host 暴露了全部 backend ABI binding。 |
| `check_native_backend_migration.py` | 在多过程 fixture 上运行 native backend 迁移 gate。 |
| `check_native_backend_canonical_diff.py` | 去掉 host 声明和 trace 后，比较历史 driver 与当前 logical-capability driver 的过程。 |
| `check_lainir_api_behavior.py` | 验证 seed provider 与 recording test provider 共享的行为契约。 |
| `check_compile_context.py` | 用 seed runtime 验证 `CompileContextV1` 与 `MetaPassResultV1`。 |
| `check_lain_vm_contract.py` | 验证 provider-neutral 的 TCB、VSpace、Trap 与 scheduler 契约。 |
| `check_lainir_physical_safety.py` | 验证 `#data` 与 `#alloca` 的物理安全 trap。 |
| `check_lainir_provider_smoke.py` | 通过公共 API 编译并运行默认 `Provider(Memory)`。 |
| `check_stdlib_conformance.py` | 在共享 scalar slice 上比较 bootstrap 与 formal stdlib。 |
| `check_srclainc_artifact.py` | 重建 Lain compiler source closure 并证明产物确定性。 |
| `check_lainc_bootstrap_snapshot.py` | 验证生成的 bootstrap bundle 及其 source manifest。 |
| `check_bootstrap_consteval.py` | 通过生成的 LAINIR `#eval` 验证 bootstrap Meta 算术。 |
| `check_bootstrap_vm_api.py` | 运行 seed 侧 Artifact/Procedure/arguments/eval 契约。 |
| `check_eval_tcb.py` | 验证 C seed 的临时 TCB `#eval` 执行契约。 |
| `check_lain_backend_abi.py` | 检查 Lain 编写的 backend 是否符合 capability ABI v1。 |
| `check_lainc_bootstrap_release.py` | 验证 lainc bootstrap release 的打包与安装。 |
| `check_lainir_boundaries.py` | 检查 core artifact 只暴露 ABI 入口、不含语言级语义 pass 实现。 |
| `check_lainvm_boundary.py` | 检查 LAINIR/LAINVM 契约边界：契约存在、lainc 只依赖契约、Meta 通过 VM 请求编译期执行、归档实现不得回到 `src/`。 |
| `check_meta_ast_conformance.py` | 对比第一代 RawAst 输出与 formal `std::meta` 输出。 |
| `check_meta_module_validation.py` | 对比 bootstrap 与 formal `std::meta` 的模块形状校验。 |
| `check_meta_form_swap.py` | 证明形式识别真的走了库：替换库谓词 `lain_std_is_module_declaration` 的返回值，编译行为必须随之改变；相同即失败。 |
| `check_native_formal_stdlib.py` | 通过 native compiler 运行 formal stdlib lowering slice。 |
| `check_native_lainc_determinism.py` | 证明 native lainc 对同一输入两次输出一致。 |
| `check_native_lainc_matrix.py` | 用代表性源码矩阵运行 native Lain compiler。 |
| `check_policy_conformance.py` | 在固定输入上比较 bootstrap 与 formal stdlib 的 policy ABI。 |
| `check_stdlib_swap.py` | 证明替换 bootstrap stdlib 会改变行为而无需重建 core。 |

### `run_*`
| 脚本 | 作用 |
| --- | --- |
| `run_lain_compiler.py` | 把第一版 Lain 源码子集编译为可执行 LAIN-IR 文本。 |
| `run_lain_backend.py` | 用 frozen compiler 自举生成的 backend L1，并按 capability ABI 校验声明清单。 |
| `run_lainir_self_host.py` | 用 seed 执行 `seed/lainir/compiler.l1` 并安装 native LAINIR compiler 到 `build/selfhost/bin/lainir-compiler`（见下文限制）。 |

### 其他单件

| 脚本 | 作用 |
| --- | --- |
| `lainc_sources.py` | 共享 helper：统一 Lain 编译器源码的 canonical 清单（见下文）。 |
| `backend_manifest.py` | 共享 helper：从 canonical LAIN-IR 生成确定性 link/capability manifest（见下文）。 |
| `bundle_lainir.py` | LAIN-IR 构建期模块打包器（LAIN-IR 本身没有源码级 import）。 |
| `canonicalize_lainir.py` | 只改格式的 canonical 化：排序 extern 与过程块、统一 LF、去尾空白、单个结尾 LF。 |
| `capture_lain_bootstrap_baseline.py` | 采集可复现的机器可读 bootstrap 基线报告（同时记录成功与失败的 gate）。 |
| `freeze_lainc_bootstrap.py` | 从 LAIN-IR frontend bundle 生成冻结的 bootstrap lainc（当前小 Lain 子集）。 |
| `install_lainc_bootstrap.py` | 把已验证的 lainc bootstrap package 安装到带版本前缀。 |
| `package_lainc_bootstrap.py` | 打包生成的 lainc bootstrap bundle 及其 manifest。 |
| `profile_lainc_bootstrap.py` | 在编译器之外记录 bootstrap 各步骤的耗时/资源报告，写入 `build/profiles/`。 |
| `prove_lainc_fixed_point.py` | 比较两代编译器的 canonical text、extern、label/signature 与 procedure body SHA-256。 |
| `split_seed_compiler.py` | 把 `seed/lainir/compiler.l1` 无损拆分为有序 source section。 |

## 共享 helper

下列文件**不是入口**，而是被其他脚本 `import`：

- `lainc_sources.py`：统一 Lain 编译器源码的 canonical 清单，供构建和边界检查复用（如 `composed_compiler_sources`、`lainir_api_sources`、`lainvm_sources`）。
- `backend_manifest.py`：从 canonical LAIN-IR 生成确定性的 link/capability manifest。
- `toolchain.py`：构建产物路径的唯一事实来源（`SEED_BIN`、`SELFHOST_BIN`、`seed_exe()`、`selfhost_exe()`），供脚本定位 seed 与自举编译器可执行文件；不要另行硬编码路径。

由于脚本之间使用**扁平 import**（例如 `from prove_lainc_fixed_point import prove`、`from backend_manifest import build_manifest`），运行任何脚本都必须以仓库根为 cwd；否则会出现 `ModuleNotFoundError`。

## 当前预期的不可用项

- 重建 bootstrap 可以跑通：`build_lain_compiler.py` 仍能重新生成 `build/bootstrap/lainc.l1` 并编译普通物理子集。
- **gen2/gen3 固定点 gate 尚未完成**：`srclainc.l1` 目前是库产物，没有可供 seed 调用的 `compiler_compile` 入口，两次构建一致并不等于编译器固定点。依据 `README.md` 的 Bootstrap status 与 `docs/roadmaps/lain-roadmap.md` §3.2、§12。
- `run_lainir_self_host.py` 自举的是 **LAINIR 编译器**，不能作为 Lain 编译器的固定点证据。
- `profile_lainc_bootstrap.py` 仍假定旧的单文件输入和 `compiler_compile` 入口，不能原样用作当前固定点驱动。
