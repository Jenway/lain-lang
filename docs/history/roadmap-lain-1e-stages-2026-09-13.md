# 编码 1e 部分实现：bootstrap 库阶段结果交接

日期：2026-09-13。只归档实际实现的阶段交接，不宣称完整编码 1e 或后续路线完成。
当前计划见 [`../roadmaps/lain-roadmap.md`](../roadmaps/lain-roadmap.md)。

## 实现

- `bootstrap/compiler/lower_program.l1::lain_std_elaborate_program` 从 syntax index 收集
  类型、模块、导入、函数与环境并语义验证，返回库内部 unit。
- `bootstrap/std/entry.l1::lain_std_elaborate` 把该 unit 与真实状态装入既有 MetaPassResult。
- `lain_std_lower` 消费 elaborated_root，调用 `lain_std_emit_program`，不再从 syntax index
  重建语义。compiler core 与公开 Meta ABI 无改动。
- 借用的 Source/RawAst 仍由 workspace 管理，临时向量在成功 lower 或失败 elaborate 后释放。
  失败 elaborate 写诊断文件并停止阶段，不发布物理程序。
- 返回类型无效的 5104 分支记录具体类型节点，补齐此前只有函数名而缺节点 span 的诊断。
- 旧 `lain_std_lower_program` 留作既有库 embedding API 的两阶段组合委托，不另建求值协议。

## 执行证据

新 `scripts/check_meta_stage_swap.py` 通过：

- 原始编译器结果 42。
- 库 expand 复制/替换 AST 后结果 7，lower 检查 origin source/start/length 与 hygiene 标签。
- 库 elaborate 改变语义 unit 内函数体后结果 7，证明 lower 消费该 unit。
- 库 lower 替换物理输出后结果 13。
- 真实非法返回类型报 5104，诊断节点 start 指向 bogus；即使 lower 被换成 Trap，失败后也未调用它。
- 探针只修改临时 bundle 的库过程，逐个比较 core 过程体不变；探针与输出均经 verifier。

输入行/依赖位宽、bootstrap consteval、五条生成 #eval 的审计、stdlib swap、规范 artifact
快照、函数签名及 span、表达式优先级、program entry 和 Meta AST 一致性检查通过。
函数形状一致性 gate 仍有 4 个正式侧 SKIP，未宣称补齐。
新 gate 已登记 baseline 与脚本索引；全量 baseline 状态须以本次实际运行输出为准。

## 未完成与新发现

- 正式 `std/meta.lain` 与正式 lainc 的阶段语义和执行证明仍缺，见编码 2/5。
- core 的 `raw_ast.l1` 中还有 `ast_find_macro_call`、`ast_expand_macro_call`、`ast_macro_param`
  等宏辅助规则和 `lain_macro_*` 探针。此前边界 gate 主要禁止 meta/program/lower 等过程名，
  未证明这些 ast/lain_macro 名字下的语义归属正确。需审计依赖后分离通用 AstApi 与库宏规则，
  加入能捕获回流的边界检查；不能把当前 gate 通过当成 core 完全没有语言语义。
- 跨阶段资源失败、替换库返回非法 payload 的处理及更广的诊断/AST metadata 覆盖继续验收。
- 专用 body 识别、描述值 builder 与受限表达式覆盖仍在，属于编码 1f–1h 的完整迁移工作。
