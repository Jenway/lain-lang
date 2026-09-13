# 编码 1h：正式普通返回表达式部分实现

日期：2026-09-13。本快照仅归档普通标量返回表达式切片，完整编码 1h 未完成。

## 实现

`std/meta.lain` 的 `meta_find_return_list` 通过共同递归 AST 表达式验证读取整个返回表达式，
要求存在声明终止符。`meta_copy_return_expression` 使用同一递归输出，移除平铺表达式
只写一个二元操作的分支；算术返回仍生成显式 LAINIR `#eval`。

三个执行反例在修复前均返回 5203：混合优先级 `14 + 15 * 2 - 14 / 7`、
左结合 `62 - 10 - 10`、Group 后继续运算 `(20 + 1) * 2`。修复后均执行得到 42。
表达式规则由正式库拥有，未修改 core、Parser 或 seed。

## 执行验收

- 隔离源码编译、完整 ABI/metadata 与 12 个共用宏用例通过。
- 修复写入正式源码后，`python scripts/build_formal_stdlib.py` 生产重建退出 0（session 12405）。
  构建入口执行新增的 `check_formal_expression_lowering.py`。
- `python scripts/check_formal_expression_lowering.py --compiler build/lainir/formal_stdlib_abi_probe.l1`
  再次通过三个正例：产物含 `#eval`、通过 verifier、实际执行到 42；缺少右操作数和未知
  操作数两个反例返回 5203，且不生成产物。
- `check_stdlib_conformance.py` 全部当前 fixture 的 IR/执行/导入诊断检查通过。
- `check_function_shape_conformance.py` 九项对比通过；四项正式侧 SKIP 仍存在，继续由编码 2 跟踪。
- `check_policy_conformance.py` 通过；它不证明 operation/handler 已执行。
- 当前生产 manifest 的源码和产物 SHA-256 与实际文件一致。

源码 SHA-256：`4d114692cc19ca7fdb877d7359fcd0e8af6eb05f6d763a3ac19858feb3b0bb4c`。
生产库 SHA-256：`f560a7e370381b74b18af8e774b9a58238ae0a1249ea63fe9cdebdcda84171a2`。

## 范围与剩余项

42 道主门禁此前全部通过，使用的是本切片修复前的正式库产物；不能将该结果视为修复后
全量 gate 证明。本切片生产重建与相关回归结果如上。修复后的两次 srclainc 源码闭包比较
仍在运行，其结果须另外确认；源码闭包确定性不等于编译器固定点。

普通函数、多个绑定、完整调用/类型规则仍有形状专用路径；bootstrap 的 scalar writer、
module/effect 等专用 artifact builder 与函数体分派仍需退役。完整 origin/hygiene、资源、
Trap 和 backend 前消解 `#eval` 的证明继续由当前路线图跟踪。
