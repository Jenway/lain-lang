# Effect

TO BE DONE in the future

Lain-Lang 会在标准库里实现这一高级特性。

Koka 和 Ocaml5 向我们展示了强大的 Effect 机制。

Lain-Lang 提供受限的**单次代数效应（One-shot Algebraic Effects）**机制。该机制旨在提供受类型系统严格追踪的非局部控制流原语，其物理实现不依赖操作系统的栈展开表（如 DWARF unwinding）。

## 1. 核心代数效应机制 (Mechanism)

### 1.1 效应声明与操作分类
效应是一组命名的操作（Operations）集合，定义计算过程中可能发起的控制转移：

```ebnf
EffectDecl    ::= 'effect' Ident ('(' TypeParamList? ')')? '{' OperationDecl* '}'
OperationDecl ::= 'op' Ident '(' ParamList? ')' '->' ReturnType ';'
```

根据操作的返回值类型，操作在语义上严格划分为两类：
1. **可恢复操作（Resumable Operation）**：返回具体的有效类型 `T`。处理器捕获该操作后，可通过 `resume` 将结果送回发生点并继续原执行流；
2. **中止性操作（Abortive Operation）**：返回不可达类型 `Never`。该操作**不具备恢复能力**，一旦触发，执行流直接沿调用栈向外退出，处理器严禁亦无法调用 `resume`。

---

### 1.2 函数签名与效应集
函数签名通过 `!{ ... }` 显式标注其可能向外泄漏的未捕获效应集合：

```plain
fn perform_task() -> ResultType !{ EffectA, EffectB }
```

* **纯度规则**：缺省 `!{ ... }` 或显式声明为 `!{}` 的函数保证为纯控制流函数，内部严禁发起任何未捕获效应；
* **传播操作符（`!`）**：调用可能发起效应的子函数时，必须使用后缀操作符 `!`。该操作符指示将子函数的效应集合并入当前函数的效应签名中，未捕获时自动向上传播。

---

### 1.3 处理器与单次恢复契约 (`handle`)
效应由动态包围在外层的 `handle` 表达式捕获并处理：

```ebnf
HandleExpr  ::= 'handle' Expr 'with' '{' HandlerArm* '}'
HandlerArm  ::= 'return' '(' Ident ')' '=>' Block
              | Ident '(' ParamList? ')' ('with' 'resume' '(' Ident ')')? '=>' Block
```

* **返回值处理 (`return`)**：当被包围的计算正常终结时，控制流进入 `return` 分支交付最终结果；
* **操作拦截**：匹配具体的操作发起点，接收传入参数；
* **单次恢复契约（One-Shot Invariant）**：
  * 可恢复操作捕获的续体（Continuation）在物理上**仅允许被 `resume` 恰好一次**；
  * 严禁将续体进行持久化保存、多次复制或回溯调用；
  * 若处理器分支在未调用 `resume` 的情况下退出，被挂起的执行栈帧立即销毁，运行时逆序触发该作用域内注册的所有 `defer` 清理块。

---

### 1.4 效应行多态 (Effect Row Polymorphism)
效应签名支持泛型集合变量，用于在不感知具体效应类型的前提下透传环境效应：

```plain
// E 是效应行变量（Row Variable）
fn transform(f: fn(A) -> B !{ E }) -> Result !{ E }
```

* **效应代数**：效应集合支持并集与消解操作。当高阶函数内部 `handle` 掉了集合中的某个具体效应时，该效应从外层签名中消除；未处理的效应继续透明透传。

---

### 1.5 物理降解契约 (Lowering)
根据操作的控制流特征，Meta 将其确定性地降解为不同的底层机器代码：

1. **中止性操作（`Never`）的降解**：
   * 因其绝不恢复，降解时不保留任何协程帧与状态机；
   * Meta 将其直接编译为**多路离散状态码**（如 `#bits<8>` 寄存器值），调用点展开为单周期的 SSA 分支指令（`#switch`），零堆内存分配；
2. **可恢复操作的降解**：
   * 在编译期（Lain-VM）：降解为虚拟机 `TCB` 的让出与 `Endpoint` 同步会合；
   * 在运行期（AOT）：根据目标平台配置，降解为无栈状态机（Stackless State Machine）或平台原生的汇编级纤程栈切换（Stack-switching Fiber）。

---

## 2. 典型应用范例 (Application Patterns)

以下范例展示如何利用上述核心机制解决具体的系统级工程问题。

### 2.1 范例一：三通道完成模型 (Value-Error-Stopped)

该模式展示如何利用**中止性效应**与普通返回值，构建零分配、无异常展开开销的“三出口”执行模型（对齐 C++ P2300 `std::execution`）。

#### 效应声明
```lain
// 声明两个中止性操作：错误与取消均不可恢复
effect Error(E: Type) { op raise(err: E) -> Never; }
effect Cancel         { op cancel() -> Never; }
```

#### 生产端：业务逻辑编写
```lain
fn fetch_record(id: #bits<64>) -> Record
    !{ async, error(DbError), cancel }
{
    if is_timeout() {
        perform cancel(); // 触发取消通道，沿调用栈退出
    }
    if is_corrupted() {
        perform error(DbError.Corrupted); // 触发错误通道，沿调用栈退出
    }
    return load_record(id)!; // 成功通道
}
```

#### 消费端：三出口拦截
```lain
handle fetch_record(id) with {
    return(record) => on_success(record), // 通道 1：值
    error(err)     => on_failure(err),    // 通道 2：错误
    cancel()       => on_stopped(),       // 通道 3：取消
}
```

* **降解实况**：在编译产物中，该结构被降解为一个返回 `(status_code, payload)` 的普通过程，`handle` 处仅生成一条对 `status_code` 的 `#switch` 指令。取消与错误完全不产生异常对象与回溯开销。

---

### 2.2 范例二：消除函数染色的数据管道 (Colorless Pipeline)

该模式展示如何利用**效应行多态**，仅编写一份算法代码即可同时驱动同步内存遍历与异步 I/O 流。

#### 协议与算子定义
```lain
// 迭代器协议携带关联效应集
let Iterator = std::concept(Self, Item) {
    next(self: &mut Self) -> Option(Item) !{ Self.Effects };
};

// 累加器利用行多态透传输入流的效应
let sum = std::func(iter: Iter)
    ?{
        T: Addable,
        Iter: Iterator(T),
    }
    !{ Iter.Effects } // 效应行多态：透传 Iter 自带的效应
    -> T
{
    let acc = T.zero();
    loop {
        let opt = iter.next()!; // 传播当前数据源的效应
        match opt {
            Some(x) => acc = acc + x,
            None    => break acc,
        }
    }
};
```

#### 场景实例化对比
1. **纯内存切片数据源**：
   * `SliceIter.Effects` 声明为 `!{}`；
   * 调用 `sum(slice.iter())` 时，`Iter.Effects` 实例化为空集；
   * Meta 在降解时抹去所有控制流开销，原地生成纯寄存器 `#loop` 汇编循环。
2. **异步网络数据源**：
   * `TcpStream.Effects` 声明为 `!{ async, error(IoError) }`；
   * 调用 `sum(tcp_stream)` 时，`sum` 自动特化为携带异步挂起与错误的函数；
   * 算法逻辑完全复用，标准库无需提供独立的 `async_sum`。
