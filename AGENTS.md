# 仓库工作规则

开始工作前读取 [开发约定](docs/spec/development.md) 和 [实现进度](docs/implementation/README.md)。
Meta 工作另读 [Meta 计划](docs/implementation/meta-parser-plan.md)。

- 语言规则放在 `bootstrap/`；只有物理指令或宿主能力缺失时才修改 `seed/`。
- `seed/src/vm/**` 的 VSpace 停线仍有效。相关决策需作者确认后才能修改实现，见开发约定。
- 构建使用 `python scripts/build.py`；生成物放 `build/`，不得手改产物来通过验收。
- 不覆盖其他任务或用户的工作区改动。新能力须有数值、拒绝码或产物文本的专项验收。
- `bootstrap/`、`seed/` 的注释及提交信息使用中文；提交信息附实测结果。
- 除本文件和根 README 导航外，文档只放 `docs/`。写作与分类规则见 [文档约定](docs/README.md#维护约定)。
