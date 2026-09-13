# 编码 2：正式类型表达式语法切片

日期：2026-09-13。只归档 AST 类型边界解析，完整签名语义和类型环境未完成。

正式库 `std/meta.lain` 递归读取引用、限定名、物理 bits 类型、工厂调用及成员后缀。
修复悬空引用、多余 type token、返回类型后额外声明 token 三个形状反例。
Parser、core、seed 不增加语言类型规则。

隔离完整 ABI 验证通过。生产 `check_function_shape_conformance.py` 自动重建正式库后
退出 0（session 91245）：12 项 bootstrap/正式 pipeline 诊断一致，剩余 1 项 SKIP。
正例通过实际正式 Meta hook 检查，不能据此宣称完整 formal lowering 或类型语义有效。
生产 manifest 中全部源码及库产物 SHA-256 与当前文件一致。

非法返回类型先于畸形 effect 行的诊断优先级、完整类型环境和诊断 span 仍由当前路线图跟踪。
此前 42 道全量门禁结果早于此修复，未作为本切片全量证明。
