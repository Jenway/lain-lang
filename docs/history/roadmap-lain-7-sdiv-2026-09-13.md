# 编码 7：参数位宽与负数除法修复快照

日期：2026-09-13。只归档当前负数除法失败项，完整 backend/native 阶段尚未完成。

## 问题与修改

旧生成 C 把 `#bits<32>` 参数写成 `uintptr_t`：`sdiv_hlp(uintptr_t a, uintptr_t b)`。
负数的无符号 32 位位模式在调用时扩成 64 位无符号值，因此已有 `_Generic` 除法宏
无法恢复原始位宽。原用例 seed 返回 3，Lain 后端返回 2147483647。

`src/lainc/backend_c.lain` 的过程定义、前置声明与一般 extern 声明现在按参数的物理
类型输出 C；地址参数仍为 uintptr_t。生成结果为 `sdiv_hlp(uint32_t a, uint32_t b)`，
与已有按宽度解释符号的除法宏组合后，结果正确。bootstrap 特定 prologue ABI 仍需整体审计。

`check_backend_differential.py` 在独立临时目录从当前源码重建 backend，避免使用过期缓存
或与别的检查重写同一 backend 产物。加入 8/16/32/64 位负数参数和返回值用例，除比较
两个后端，还断言原用例独立预期值 3、新宽度用例独立预期值 42。
C 形状检查的窄整数与浮点参数声明断言按物理类型更新。

## 已执行验收

- `python scripts/check_backend_differential.py`：独立重建后 8 个差分用例全部通过；
  原负数用例返回 3，8/16/32/64 位用例返回 42。
- `python scripts/check_backend_c_shape.py`：全部形状与拒绝用例通过。
- `python scripts/check_native_backend_migration.py`：通过。
- `python scripts/check_native_backend_canonical_diff.py`：通过。
- `python scripts/check_lainc_bootstrap_release.py`：通过。
- `python scripts/check_lainc_bootstrap_snapshot.py build/bootstrap/lainc.l1`：通过。
- `git diff --check`：通过。

## 证明范围

没有据此宣称完整有符号边界、除零/溢出 Trap、浮点操作、间接调用、alloca 生命周期或
native 编译器矩阵已完成。这些仍在当前路线图。
迁移前 39 道 baseline 曾在差分失败处终止；修复后的上述定向检查通过，40 道全量入口
尚未重跑，不能宣称本次已有完整全量通过结果。
