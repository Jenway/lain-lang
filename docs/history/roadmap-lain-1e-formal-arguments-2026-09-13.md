# 编码 1e：正式宏参数隔离与递归表达式部分实现

日期：2026-09-13。归档正式宏同名参数失败项；完整 1e/1h 仍未完成。

## 实现

`std/meta.lain` 的参数代入只匹配当前模板的 syntax context，调用者 AST 保留原 context。
参数直接从调用者 AST 复制，移除 span 重解析与以源位置推进参数的旧路径。
复合参数的 origin 用有效范围覆盖，bootstrap 对应范围计算也同步修正。
模板通过 holder 复制并代入，之后取首节点和尾节点，支持单 Atom 模板；退役两条旧链复制/代入辅助路径。

验证发现原正式 lowering 不支持括号内变量和嵌套表达式，随后增加共享的递归标量 AST
表达式验证/输出，按优先级与左结合顺序生成物理算术操作。括号从 Group 子节点读取，
不将合成 Group 当作原始源码切片。局部返回也走该路径，移除把括号表达式直接写成 `%name`
的旧分支。本记录不宣称全部表达式、函数或旧 scalar writer 已迁移。

## 执行验收

- `python scripts/build_formal_stdlib.py` 完整源码重建和内置验收通过（session 24782）。
- 用 core 与刚重建的正式库组合 compiler bundle，执行 `check_bootstrap_macro_expand.py --compiler ...`，
  与 bootstrap 相同 12 个用例全部通过：三个原正例、三个诊断、同名变量、复合参数、
  单 Atom、混合优先级、减法左结合、嵌套宏参数。正例实际执行输出预期值，诊断非零退出。
- 重新执行正式 `verify_abi_entry()` 全部执行检查及六项新增宏回归通过（session 20985）。
  六项回归已登记到正式库生产构建入口。
- hygiene 探针给调用者参数设置 context 123，展开后验证两次代入的 context 和
  origin source/start/length 保留，同时原模板 context 1/2 的检查继续通过。
- bootstrap 的同一组 12 个用例通过；`git diff --check` 通过。

## 失败迭代与范围

中间验收曾返回 5203，后捕获错误结果 43；分别暴露表达式形状覆盖不足及局部返回旧分支。
均通过修改库源码解决。临时变体只用于定位，没有修改缓存 `.l1` 来完成构建。

仍需完整宏绑定/attribute/跨模块展开、错误 span、非法阶段 payload 与资源路径。
完整普通函数/表达式 lowering 以及 scalar 最小 writer 的退役仍属编码 1h。
旧 40 道 baseline 在此前状态全部通过；当前 42 道主入口的结果单独记录，不能沿用旧结果。
