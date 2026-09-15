# Fixture 对比：绑定名称与默认 store

日期：2026-09-14。完整 44 项门禁尚未重新通过。

局部绑定迁移后，bootstrap 的参数局部化和确定性名称与正式实现产生了
结构差异。对比现在按过程统一绑定名称，仅折叠类型相同、没有后续赋值
的参数副本。无注解整数常量 store 根据 seed verifier 的推导规则记为
bits<64>。错误的参数顺序、可变参数副本和窄 store 差异负向检查均保留。

`check_stdlib_conformance.py` 从合入读取修复的当前 bootstrap bundle 与正式
库组合执行，退出 0（session 39008）：16 项 IR/运行对比、导入正例与
4101/4103 诊断通过。独立预期值的运行检查保留。此结果不证明更广的
effect/handler、完整正式类型环境或固定点。

完整门禁重跑记录在 `build/local-address-review/full-baseline-after-reads/`，
记录开始/结束源码指纹；上次失败日志保留在 `full-baseline/`。
