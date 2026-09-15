# 常量重复检查按 Meta 作用域执行

`meta_bindings.l1` 的常量及 literal 收集改用当前 Meta 环境的
`meta_env_lookup_local` 检查重复声明。两个独立模块可分别声明同名常量；
当前作用域已有同名绑定仍拒绝。

默认 bootstrap 源码重建后，`check_constant_scopes.py` 三项通过：
独立模块的常量分别为 41、43，组合程序返回 42；根作用域重复报 5113；
同模块重复报 3013。Meta module validation、五项 pipeline audit 及
`git diff --check` 通过。主基线加入该检查，共 46 道。

该切片不完成普通值的环境读取迁移。旧全单元常量读取回退和普通函数输入
签名/调用的统一仍由当前路线图跟踪；编译期普通体原型未合入。
