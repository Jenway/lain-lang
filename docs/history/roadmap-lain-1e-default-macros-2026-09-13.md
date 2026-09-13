# 编码 1e：bootstrap 默认宏展开部分实现

日期：2026-09-13。只归档默认 ABI 已执行的基本展开切片，完整宏和阶段语义仍未完成。

## 实现

`bootstrap/std/macro_expand.l1` 实现库的宏声明查找、模板复制、参数配对、复合表达式
分组、嵌套展开、递归限制及声明移除。只依赖通用 AstApi 和已有库的文本/调用视图，
没有给 core、seed 或 VM 增加宏语义。

`bootstrap/std/entry.l1` 的默认 expand 阶段在发现宏时复制源码根，在副本展开，并经
`syntax_unit_set_root` 更新库的语义索引。缓存源码树保留原状。之后由已有 elaborate/lower
处理生成的 AST；编译器 core 不改动。

模板先分配 syntax context，随后代入调用者参数。代入只匹配模板 context，避免把
前一个调用者参数中的同名变量当作下一个宏形参。复合表达式从调用者 AST 复制，保留
其来源与 context。模板复制保留原 origin；完整 hygienic 绑定与错误 span 尚待证明。

## 验收

`python scripts/check_bootstrap_macro_expand.py` 已加入 baseline（现在 42 道）。
真实默认编译流水线验证：单参数 42、双参数 42、多声明 21、递归错误 4202、缺参数 4203、
多参数 4204、调用者同名变量 42、复合表达式 42、单 Atom 模板 42。正例产物经 verifier
并在物理解释器执行，错误用例检查非零退出及稳定诊断码。

`check_meta_stage_swap.py`、`check_lainc_lainir_api_snapshots.py` 和 `git diff --check` 通过。

## 全量和正式库状态

旧 40 道 baseline（session 91351）已全部通过，包含宏辅助归属与除法修复，但在本切片
接入默认 expand 前启动，不能据此宣称当前 42 道入口已全部通过。

正式库生产构建入口在独立目录重新执行（session 16846），包括 ABI/Meta/宏/诊断探针，
全部通过。证据在 `build/roadmap-review/formal-rebuild/verification.json`；源码未改变时
新旧产物 SHA256 不同，说明此前仅核验源码 manifest 不足以证明由当前 compiler 重建。
主入口新增正式库构建 gate，在其消费者检查前执行；42 道全量入口尚未重跑。

新同名参数用例在刚重建的正式库上运行返回 0，应为 42；`std/meta.lain` 的顺序代入仍按
名字匹配调用者参数。该缺陷保留在 active roadmap，正式库需要相同 context 隔离和回归。
完整宏绑定、attribute、跨模块展开、错误 span、非法 payload 与资源路径继续验收。
