# Effect（单次代数效应）

Lain-Lang 提供受限的**单次代数效应（One-Shot Algebraic Effects）**机制，为标准库与用户代码提供类型可追踪、零异常展开表开销（无需 DWARF Unwinding）的非局部控制流原语。

> 说明：本规范采用统一 `let` 声明语法与现代零裸指针类型体系（`Ref(T)`、`Mut(T)`、`Slice(T)`、`i64`、`u64`、`bool`）。

---

## 1. 核心代数效应机制

### 1.1 效应声明与两类操作

效应是一组命名的控制转移操作集合，用 `effect` 表达式绑定到标识符：

```ebnf
EffectExpr    ::= 'effect' ('(' TypeParamList? ')')? '{' OperationDecl* '}'
OperationDecl ::= 'op' Ident '(' ParamList? ')' '->' ReturnType ';'
```

```lain
// 参数化与无参效应声明
let Error = func(E: Type) -> Effect {
  return effect { op raise(err: E) -> Never; };
};

let Cancel = effect {
  op cancel() -> Never;
};

let Yield = func(T: Type) -> Effect {
  return effect { op emit(val: T) -> bool; };
};
```

根据操作返回值的可返回性，操作严格分为两类：
1. **可恢复操作（Resumable Operation）**：返回具体有效类型 `T`（如 `bool`、`u64`）。外层 `handle` 捕获该操作后，可通过 `resume(value)` 将结果送回发起点并继续原执行流；
2. **中止性操作（Abortive Operation）**：返回不可达类型 `Never`。该操作**不具备恢复能力**，触发后控制流沿调用栈单向退出，处理器分支不得也不可能调用 `resume`。

---

### 1.2 函数签名中的效应集 (`!{ ... }`) 与传播 (`!`)

三区函数签名的第三区 `!{ ... }` 显式声明过程允许向外逃逸的未捕获效应集：

```lain
let perform_task = func(id: u64) -> ResultVal
  !{ Error(DbError), Cancel }
{
  ...
};
```

* **纯度约束**：省略 `!{ ... }` 或显式写 `!{}` 表示控制流纯过程，内部严禁发起未在函数内部捕获的效应；
* **显式传播操作符（`!`）**：调用带有效应的子函数时，须在调用处标出后缀操作符 `!`（如 `load_record(id)!`），表明将其未捕获效应并入当前函数的效应集或交由外层 `handle` 拦截。未标 `!` 也未被本地捕获时，编译期直接报错。

---

### 1.3 处理器与单次恢复契约 (`handle ... with`)

效应由包围在外层的 `handle` 表达式动态捕获并处理：

```ebnf
HandleExpr  ::= 'handle' Expr 'with' '{' HandlerArm* '}'
HandlerArm  ::= 'return' '(' Ident ')' '=>' ExprOrBlock
              | Ident '(' ParamList? ')' ('with' 'resume' '(' Ident ')')? '=>' ExprOrBlock
```

* **`return(val)` 分支**：被包围的计算正常完成时进入此分支；
* **操作拦截分支**：按发起的效应操作匹配对应分支并绑定传入实参；
* **单次恢复契约（One-Shot Invariant）**：
  * 可恢复操作捕获的续体（Continuation `resume`）在物理上**仅允许最多调用一次**；
  * 严禁对续体进行复制、持久化保存跨过处理器作用域或多次回溯调用；
  * 若处理器分支直接退出而未调用 `resume`，被挂起的栈帧按作用域规则销毁，并逆序执行沿途登记的 `defer` 清理块。

---

### 1.4 效应行多态 (Effect Row Polymorphism)

高阶函数或泛型容器支持在 `?{ ... }` 与 `!{ ... }` 中通过行变量透传未知效应集：

```lain
// E 为效应行变量（编译期值）：sum/transform 对其内容保持透明
let transform = func(x: A, f: Fn((A) -> B !{ E }))
  ?{ A: Type, B: Type, E: EffectRow }
  !{ E }
  -> Result
{
  let b: B = f(x)!;
  return wrap(b);
};
```

* **效应消除律**：若函数体内部用 `handle ... with` 完全拦截了某个效应（如拦截了 `Error(DbError)`），该效应自动从函数对外暴露的 `!{ ... }` 签名中消除，剩余未处理效应继续向上透传。

---

### 1.5 物理降解契约 (Lowering to LAINIR)

Meta 依据操作是否可恢复，将其确定性地降解为两套零运行时意外开销的底层结构：

1. **中止性操作（`-> Never`）的降解**：
   * 因永不恢复，完全无需协程帧或堆分配状态机；
   * 编译器将带有中止性效应的过程改写为返回 `(status_code: #bits<8>, payload)` 的普通值过程，在 `handle` 处展开为单条 **`#switch`** 分支指令。错误与取消路径的代价等价于手动返回 tagged struct。
2. **可恢复操作（Resumable）的降解**：
   * **Tail-Resumptive（尾恢复证据传递）**：若处理器分支直接返回给 `resume(...)`（如状态读取/日志采集），直接降级为编译期注入的普通回调参数调用，零栈切换成本；
   * **一般单次续体**：在编译期（Lain-VM）映射为 `TCB` 让出与 `Endpoint` 同步会合；在运行期（AOT）依据平台策略降解为无栈状态机或平台纤程单次栈切换。

---

## 2. 典型应用范例

### 2.1 范例一：三通道完成模型 (Value-Error-Stopped)

利用两个中止性效应与普通返回值，以零异常表与零堆分配构造结构化并发“值 / 错误 / 取消”三出口（语义对齐 C++ P2300 `std::execution`）：

```lain
let fetch_record = func(id: u64) -> Record
  !{ Error(DbError), Cancel }
{
  if is_timeout() {
    perform Cancel.cancel();
  }
  if is_corrupted() {
    perform Error(DbError).raise(DbError.Corrupted);
  }
  return load_record(id)!;
};

let status: i64 = handle fetch_record(id) with {
  return(rec) => on_success(rec),
  raise(err)  => on_failure(err),
  cancel()    => on_stopped(),
};
```
* **降解实况**：整个三通道控制流降解为对 `#bits<8>` 状态标签的一次 `#switch`，与手写错误码返回具有完全一致的机器指令密度，却具备完整的编译期穷尽跟踪保证。

---

### 2.2 范例二：消除函数染色的数据管道 (Colorless Pipeline)

利用效应行多态，同一份迭代器归约逻辑既能零开销跑在纯同步切片上，也能跑在异步流上：

```lain
// Iterator Concept 允许实现模块自带关联效应集 `Effects`
let Iterator = func(Self: Type, Item: Type) -> Type {
  return struct {
    Effects: EffectRow,
    next: Fn((Mut(Self)) -> Opt(Item) !{ Effects }),
  };
};

// sum 对输入流效应完全透明（无 sync/async 函数染色分裂）
let sum = func(iter: Mut(Iter))
  ?{
    T: Type,
    Iter: Type,
    it: Iterator(Iter, T),
    m: Monoid(T),
  }
  !{ it.Effects }
  -> T
{
  let acc: T = m.zero();
  loop {
    let opt: Opt(T) = it.next(iter)!;
    match opt {
      Some(x) => { acc = m.combine(acc, x); },
      None    => { break acc; },
    }
  };
};
```

* **纯内存切片实例化**：当 `SliceIterMod` 的 `Effects` 为 `!{}` 时，`it.Effects` 实例化为空集，`sum` 降解为纯寄存器 `#loop` 循环，零分支、零状态机；
* **异步 / 带错流实例化**：当 `TcpStreamMod` 的 `Effects` 为 `!{ Async, Error(IoError) }` 时，同一个 `sum` 自动特化为可挂起与可传播错误的过程，标准库无需重复编写 `async_sum`。
