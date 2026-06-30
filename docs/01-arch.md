# Architecture

1. **Parser**: 将文本解析为无语义的结构拓扑树 (LAIN-AST)。
2. **Meta**: 接收 LAIN-AST，执行类型检查、宏展开、效果降级，输出物理指令 (LAIN-IR)。
3. **Evaluation**: 遇到 `#eval` 时，在编译器内部解释执行 LAIN-IR。
4. **Backend**: 将 LAIN-IR 转换为 LLVM IR 或 C 代码。
