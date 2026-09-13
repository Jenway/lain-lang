# 编码 1e：bootstrap 宏辅助规则归属快照

日期：2026-09-13。本切片只完成宏辅助过程的库归属；完整编码 1e 仍未完成。

## 实现

从 `bootstrap/compiler/raw_ast.l1` 移出 20 个宏辅助过程和探针，放入
`bootstrap/std/ast_macros.l1`，由 `BOOTSTRAP_STD_MODULES` 构建到 stdlib。
core 保留无语言规则的解析、节点访问、复制、替换、父节点搜索、文本替换与计数等通用操作。
宏参数配对、逗号分隔表达式扫描、声明/调用识别与展开属于库。

`check_lainir_boundaries.py` 增加宏过程与宏拼写的回流检查。
`check_library_ast_macros.py` 已登记到 baseline（现在 40 道）。

## 执行证据

- `python scripts/check_library_ast_macros.py`：重建 core/stdlib/组合 bundle，均经 verifier；
  宏辅助过程在库；边界负对照命中；嵌套展开输出
  `prog:lety=((21+1)+(21+1)); expanded:3 residual:0`。
- 同一检查仅替换库 `ast_expand_macro_call` 的参数代入行为，输出变为
  `prog:lety=((x+1)+(x+1)); expanded:3 residual:0`；比较全部 core 过程体，确认未改变。
- `python scripts/check_meta_stage_swap.py`：阶段结果消费、origin/hygiene 与失败诊断检查通过。
- `python scripts/check_meta_pipeline_audit.py`：五条物理 #eval 生成路径核验通过，限制仍有效。
- `python scripts/check_lainc_lainir_api_snapshots.py`：规范产物和 import 反例快照通过。

单源码模式由 seed 当作 IR 输入。检查只在临时探针中调整 source-count guard，使用第二个
空源码选中宿主编译器模式，探针仍只读源码 0；没有修改 seed 或真实编译器产物。

## 证明范围及剩余工作

这些是已有宏辅助规则与探针的迁移，不能证明默认 `lain_std_expand` ABI 已支持任意用户宏。
正式库阶段语义、跨阶段失败与资源清理、更广的 origin/hygiene 仍在当前路线图。

迁移前启动的 39 道全量 baseline 已终止：通过前面的检查，在 backend differential 失败，
有符号除法 seed 返回 3、Lain 后端返回 2147483647；后续 release/snapshot 未执行。
迁移后本切片的定向检查通过，尚未重跑 40 道全量 baseline。
