# 编译脚本验证成功产物

`run_lain_compiler.py` 在报告成功前用 `toolchain.seed_exe("lainir-print")`
解析并验证生成的 LAINIR。物理验证失败时输出 verifier 诊断、移除无效程序
文件并返回失败。未改变 Lain 名字解析或 seed 的语言语义。

`check_compiler_artifact_verification.py` 两项通过：正常程序执行返回 42；
未解析成员调用无法以成功产物保留。独立导入反例报 verifier 2004 且无产物。
现有输入行正例及十一项失败诊断回归通过。检查已加入主基线，共 47 道。

此完成切片仅覆盖 Python 编译入口的物理验证。bootstrap 编译 API 本身的
验证与诊断、导入成员语义解析、lower 失败和 sink 生命周期、执行消除 #eval
仍由当前路线图跟踪，不能用本切片证明这些项目完成。
