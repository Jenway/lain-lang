# 当前路线图

本目录从 2026-09-06 的代码状态重新开始，只记录尚未完成的工作。旧里程碑、完成项和实施记录见 [`../history/`](../history/)。

当前有三份相互配合的计划：

1. [`compiler-bootstrap.md`](compiler-bootstrap.md)：项目主线。先恢复正式标准库构建，再完成 `src/lainc` 自举和工具链切换。
2. [`lainc-lainir-api-migration.md`](lainc-lainir-api-migration.md)：主线中的依赖反转计划，固定 `lainc` 生成和执行 LAINIR 的能力 API，并移除 `src/lainc/l1_*`。
3. [`lainir-maintenance.md`](lainir-maintenance.md)：API provider 之外的 LAINIR 维护工作；一般不阻塞编译器自举。

当前主线：

```text
formal stdlib 可重建
  -> formal stdlib 成为默认语义实现
  -> lainc 通过稳定 API 使用 LAINIR
  -> src/lainc 可编译真实程序
  -> lainc gen2/gen3 固定点
  -> native 工具链切换
  -> 删除过渡实现
```

每一阶段都以可执行检查为完成条件。路线图不保存逐日进度；完成后的阶段整体移入历史文档，再从新的代码基线更新本目录。
