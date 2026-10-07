# Effect

TO BE DONE in the future

Lain-Lang 会在标准库里实现这一高级特性。

Koka 和 Ocaml5 向我们展示了强大的 Effect 机制。

Effect 描述程序可能请求的操作，以及这些操作如何声明、传播和处理。例如读文件、报告业务错误或挂起执行。Meta 负责源语言的 effect 规则；执行环境负责已降低为物理操作后的权限与失败。

## 两个执行阶段

| 阶段 | 可能的操作 | 能力来源 |
| --- | --- | --- |
| 运行时 | I/O、原始内存访问、错误报告、挂起 | 运行时库和目标平台 |
| 编译期 | 读取源文件、生成 artifact、调用外部工具 | 编译驱动显式注入的宿主能力 |

语言中声明一个 effect 不自动授予宿主权限。编译期代码即使通过 Meta 的 effect 检查，实际执行仍要通过 LAINIR 和 VM 的能力边界。外部文件、网络、进程等操作必须由驱动提供对应服务；失败应有可诊断结果。

## Handler 与 lowering

Handler 是处理某类 operation 的程序。Meta 应检查 handler 是否覆盖所需操作、如何传播剩余 effect，以及 `resume` 的使用规则。若允许保存或多次恢复，必须规定恢复次数、保存状态的寿命和取消时的清理。

Meta 可以根据已确定的 handler 把 operation 降为普通调用、显式 continuation、状态机或 VM 支持的控制操作。选择 TCB 保存执行状态还是以 CPS（续体传递）显式表示剩余计算，属于 lowering 策略；两条路径的可用范围和运行代价都需要具体用例验证。VM 的 TCB、Endpoint 或分片执行能力存在，并不表示语言的 handler 已实现。
