# LAINIR 维护计划

基线日期：2026-09-06。

## 当前基线

LAINIR 已经覆盖当前编译器自举所需的物理执行主干。公开文本使用规范物理类型和明确的整数操作；内存由 activation-scoped `#alloca`、`#lea`、typed load/store 和只读静态 `#data` 表达。seed parser、verifier、interpreter 与自举 C emitter 共享同一 IR 模型。

下面的工作独立于编译器自举主线。只有编译器主线遇到明确的物理能力缺口时，相关项目才升级为主线前置条件。

`lainc` 对 LAINIR 的 Builder、Artifact 和 Eval 能力约定及其 provider 迁移属于编译器主线，见 [`lainc-lainir-api-migration.md`](lainc-lainir-api-migration.md)。本文件记录该 API 边界之外的物理执行层演进。

## 1. Data relocation 和只读保证

- 定义 data 对其他 data 和 procedure 的 relocation；
- 让 verifier 在静态可知时拒绝只读写入和越界地址；
- 定义 native backend 对 alignment 和只读段的跨平台保证；
- 增加 parser、interpreter 和 native backend 的差分测试。

完成条件：同一 relocation fixture 在解释器和 native backend 中得到一致地址与数据，非法 relocation 和写入产生稳定诊断。

## 2. Activation 地址有效性

- 跟踪通过 store、call 和聚合内存间接逃逸的 `#alloca` 地址；
- 为 use-after-return 和越界访问提供确定性 trap；
- 明确累计 allocation budget 与当前存活字节预算；
- 保证递归调用和嵌套 `#eval` 的 activation 相互隔离。

完成条件：直接与间接逃逸、递归和嵌套 eval 的正负测试均不依赖宿主内存偶然行为。

## 3. 浮点和 SIMD

- 定义浮点常量文本及 canonical 输出；
- 完成 `#float<32>`、`#float<64>` 的调用 ABI；
- 对解释器和 native backend 做 NaN、无穷、舍入和比较差分测试；
- 决定 SIMD 是否进入公共文本语言，并在进入前保持内部预留状态。

完成条件：浮点程序跨执行后端结果一致；若公开 SIMD，必须同时具备文本语法、验证规则和至少一个 backend。

## 4. 诊断与公共 API

- 让所有运行时 trap 带 procedure 和准确源码位置；
- 统一 parser、verifier、interpreter 与 native backend 的错误分类；
- 为公开 IR enum/handle API 建立稳定版本或生成式 kind 映射；
- 删除自举 emitter 对易漂移裸枚举编号的依赖。

完成条件：错误 fixture 能稳定比较阶段、错误码、procedure 和位置；新增 IR kind 时不会要求手工同步多个数字表。

## 长期约束

- 不恢复 legacy LAINIR 类型别名和模糊整数操作；
- 字段布局始终由上层展开为 `#lea + #load/#store`；
- 新操作使用明确指令或声明过的外部过程，不加入宽泛的 `#primitive`；
- AST、module、generic、effect 和 source-language type 保持在 LAINIR 之外。
