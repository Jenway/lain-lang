# LAIN-VM 实施路线图

基线日期：2026-09-07。

LAIN-VM 是 `#eval` 和未来运行期执行环境共同使用的控制面。当前优先级是为编译期执行定义可验证的最小环境；完整的虚拟地址空间、线程上下文、同步端点和 OS/裸机下沉属于后续路线，不能提前当作 LAINIR 已实现的指令。

## 当前边界

当前已经存在并纳入 contract 的内容：

- 命名的只读静态 `#data`；
- 当前 procedure activation 内有效的 `#alloca`；
- `#lea` 与 typed `#load/#store`；
- Eval 的 step、call-depth、allocation 限制；
- 显式传入的 capability。

当前不存在的内容：

- `VSpace`、`TCB`、`Endpoint`、`Trap`、`CSpace` 的公共 Lain 对象；
- `#swap_context`、软件 MMU、demand paging；
- 面向 Linux 或裸机的 VM lowering ABI。

## 与编译器自举的关系

`#eval` 是 Meta 执行的物理承载。因此 API 迁移中的 EvalApi 不能只验证函数签名，还必须由同一份 VM contract 固定执行隔离、activation、配额、trap 和 capability 的行为：

```text
EvalApi
  -> VM session
     -> activation / memory / quota / capability
     -> typed LAINIR execution
     -> value or owned object result
```

VM 的阶段 0 和阶段 1 是 `lainc-lainir-api-migration.md` 阶段 5 完成的前置条件。阶段 2 以后可以与 compiler fixed point 并行，但任何新增 VM 操作都要先进入 LAINIR 规范、API contract 和 provider contract tests。

## 阶段 0：冻结 Eval 执行契约

- 定义一次 VM session 的创建、运行、成功返回、trap、limit 和取消语义；
- 固定 activation 的建立与销毁顺序，禁止 activation 地址跨 procedure 返回；
- 固定 `#data` 只读、`#alloca` activation 生命周期和 typed memory 的检查；
- 固定 step、depth、allocation quota 的计数点和诊断；
- 固定 capability 的显式传入、空 capability 默认值和拒绝错误。

完成条件：seed provider 和 test provider 通过同一组 session、生命周期、配额、capability 和 trap contract tests。

## 阶段 1：完成 Eval provider

- 将上述契约接到正式 EvalApi；
- 为 scalar 返回值定义复制 ABI；
- 为 type、module、AST 等非 scalar 结果定义 provider-owned opaque object；
- 覆盖 nested eval、递归、activation escape、超限和 capability 拒绝；
- 让 Meta 只观察 EvalApi 结果，不读取 interpreter 或 memory model 内部表。

完成条件：Meta 的编译期执行在默认 provider 中通过 verifier、evaluator 和 owner 释放测试，且结果可重复。

## 阶段 2：虚拟执行控制面

在编译器自举固定点之后，定义并实现需要运行期调度的最小对象：VSpace 资源范围与权限容器、TCB 执行现场、Endpoint 同步边界、Trap 统一诊断入口、CSpace 运行期 capability 表。

每个对象都必须先有 API contract、至少一个可运行 provider 和验证用例；对象名称本身不构成新的 LAINIR 指令。

## 阶段 3：平台 lowering

- 定义解释器、native backend、Linux 和裸机对 VM 对象的 lowering；
- 比较 trap、权限、地址有效性和同步语义；
- 只有在多个 backend 共享同一 contract 后，才考虑软件 MMU、`#swap_context` 或 demand paging。

完成条件：同一 VM fixture 在解释器和至少一个 native/platform backend 中得到一致的结果和诊断。

## 当前执行顺序

```text
EvalApi contract
  -> VM session / activation / quota contract
  -> default provider Eval
  -> Meta owner/result migration
  -> gen2/gen3 fixed point
  -> VM control plane
  -> platform lowering
```

`docs/04-lain-vm.md` 是本路线图所引用的架构提案。若提案内容与 LAINIR 或 API contract 冲突，以当前规范和 contract tests 为准。
