# Lain-Meta

> [!QUOTE]
> Meta 究竟是个什么样子，C Macro 搞了很多年，也并没有完全搞清楚。
>
> 可能 CPP 的思路比较好，搞了个静态反射，但是 `std::meta` 完整进标准不知道要哪一年。

## eval 与 apply

```text
eval(源码形式, 环境) → 编译期值
apply(LAINIR 过程, 实参, 能力与限额) → 执行结果
```

- eval 按语法绑定处理形式，在需要执行时生成自包含模块并调用 apply，再解释执行结果。
- apply 的 Meta 库入口委托 seed 通用服务执行普通过程。模块和入口是过程参数的物理表示。
- 两者自身可以用普通 LAINIR 过程实现，不成为 Lain 内建语法或 LAINIR 新指令。
- eval 返回值，生成过程属于内部编译步骤。值可以是标量、类型或可调用值；可调用值可交给 apply。
- 编译期值用于目标程序时，由类型规则生成常量或数据。
- 类型、语法树和过程对象的语义表示属于 Meta；下层标量/字节传输格式不等于 Meta 的全部值域。

Meta 库内部仍需展开、绑定和 lowering 等编译步骤。当前 `meta_eval_apply_decl_closure(root, args, arg_count)`
已在隔离 emitter 作用域内收集同源声明闭包、重新生成 LAINIR，并把显式标量 wire 实参与
apply 请求一起转交；它当前接受标量结果，也支持通过固定长度字节结果请求把结果按值复制回 Meta。C 层 `lainapply_proc_bytes` 检查独立 VSpace 的可读范围，按配额计费并在空间销毁前复制；`lainapply_proc_with_bytes_arg` 把输入副本以只读 `#addr` 映射给指定形参。Meta ABI 9 已接通字节实参通道，输入长度也计入 Expand 统一预算。通用 `eval(form, env)` / `apply(proc, args, caps, limits)` 仍缺过程值、跨模块身份、嵌套 eval 和结构化值语义，见实施计划。

## apply 契约

C 接口见 `seed/include/lainapply/apply.h`；下列是语义约定。

| 项 | 规则 |
| --- | --- |
| 模块 | 已验证的 LAINIR 文本。第一版无状态，不引入宿主句柄，因此模块必须自包含依赖过程 |
| 入口 | 模块内的普通 `#proc` |
| 实参 | 普通入口接收常量物理值；显式字节入口可把一个按值复制的只读字节块映射为一个 `#addr` 实参，其余实参仍按物理签名校验 |
| 限额 | 步数、栈、调用深度、配额和显式能力表（`LainApplyLimits`）；能力表必须由调用方先冻结 |
| 结果 | 标量；或不带地址、按值拷出的字节块；或稳定诊断码 |
| 失败 | 不交付部分结果；清除旧结果；错误位置属于生成的模块文本 |
| 拒绝 | 结果类型含地址一律拒绝；无授权的外部调用拒绝 |

标量入口 `lainapply_proc` 只交付标量。结果字节入口 `lainapply_proc_bytes` 要求调用方给出固定长度（1..`LAINAPPLY_BYTES_MAX`，上限 1 MiB），在独立 VSpace 内校验完整可读范围、按块长扣配额，并在销毁空间前复制。输入字节入口 `lainapply_proc_with_bytes_arg` 把调用方持有的块复制到独立 VSpace 并以只读 `#addr` 映射给指定形参；映射随执行销毁，复制占用计入配额。以上 C 层接口尚未与 Expand 统一预算挂钩。
Meta 宿主能力通道支持最多 8 个标量实参；每项以 kind、width、bits 三个 64 位单元传输。ABI 9 的字节实参变体另传一个授权可读字节范围，宿主复制后以只读映射交给 apply。
宿主回调专项验证了两参数结果、数量/类型/地址拒绝和 wire 预算；struct 布局用例验证 Meta TCB
经普通过程 `meta_apply_scalar_request` 构造两条 wire 实参并调用宿主 apply 得到布局结果。ABI 9 的 Meta TCB 专项把 `ABCD` 以只读字节实参传给求和过程，返回 266；输入长度纳入统一预算。Meta 内部可对可见 i32 和 struct 类型编码/解码 type_ref；两个同布局单字段 struct 的名义 ID 不同，size/alignment 均为 4/4；可按可见函数名生成 proc_ref，再由 `meta_eval_apply_proc_ref` 复用声明闭包生成路径执行，专项 `WireCallable` 返回 1。直接及互递归 struct 布局会拒绝（Meta 诊断 5）；checked align/add 有 UINT64_MAX 回绕边界断言，但真实超大布局拒绝尚未端到端验证；更深层聚合与 enum 类型值尚未接通，任意源码形式的 eval 结果也尚不能作为 Lain 值保存/传递；能力/限额值形式的通用 Meta 库 apply、嵌套 eval 和结构化值递归语义仍未实现。LMV1 wire 外框和引用身份校验已在 Meta 库实现。

### 拒码

| 码 | 含义 |
| --- | --- |
| 9340 | 入口不存在 |
| 9341 | 实参个数与过程签名不符 |
| 9342 | 实参类型与过程签名不符（类别或位宽不同） |
| 9343 | 普通实参含 `#addr`，或除显式字节入口指定的位置外过程声明了 `#addr` 形参 |
| 9344 | 结果含 `#addr`，或过程声明了多个结果 |
| 9345 | 字节块长度超上限，或分配/校验时长度非法 |
| 9346 | 字节块参数非法（空块没有存储、`data` 与 `length` 不一致、块缺失） |
| 9347 | 宿主未启用 AstOut，apply 没有统一预算来源（可恢复拒绝，回调用 0 返回） |
| 9348 | apply 无法把只读字节实参映射进本次独立 VSpace |
| 9349 | 留空 |

执行期失败沿用宿主 apply 执行段已有拒码（9305 起不了调用、9306 引擎 trap、9313/9314/9315/9318
装载/TCB/能力/栈，以及引擎自身的 1007 栈超限、1011 帧容量用尽），不另分配新码。

## 与宏展开的关系

现有 Expand 处理器在 Meta 映像内以普通过程调用运行，AstOut 负责候选树事务。
库 eval/apply 路径建设后，元程序可调用 eval，并由同一个 seed 执行服务承接生成的 LAINIR。
嵌套请求的上下文、权限和累计预算须显式管理，不能靠递归重置限额获得无限资源。

## 当前缺口与实施入口

当前已有按声明依赖闭包重新生成同源 LAINIR 并 apply 的路径，且 emitter 输出与外层产物隔离；
Meta 宿主 apply 通道支持标量实参数组和一个按值复制的只读字节实参；闭包入口目前接收显式标量 wire，执行结果支持标量和固定长度字节。结构化传输的类型/过程语义、类型值登记与通用库入口仍待实现。
后续任务包括通用编译上下文、跨模块声明身份、库级实参/能力/限额、嵌套 eval 与结构化值通道。


## 宿主上下文绑定

宿主能力**没有**“宿主地址”这个业务参数。可信 C 注册路径（`lainmeta_host_register`）在登记每个能力时把 `LainMetaHost *` 绑成该能力槽的 context，LAINVM 在宿主调用时把它注入回调的第一个参数。因此：

- Meta 只传**业务参数**，看不到也无法指定、伪造或读取宿主指针；
- 能力名仍是 `link_name`，也是最终 C 产物的外部符号名；最终 C ABI 只有业务参数，不因为 VM 的 context 注入而改变；
- 当前 ABI 为 **9**，继续使用能力槽隐式 Context；驱动检查版本并执行 initialize；apply 支持实参数组、受限字节结果复制和只读字节实参传入；`lain_meta_session_id()` 提供当前编译会话编号，供 Meta 验证 wire 引用归属；
- 上下文绑定解决的是“由哪个宿主兑现能力”，**不**替代业务缓冲权限：读写调用方给的地址仍由宿主自己的 `lainmeta_host_attach_space` 授权与范围检查把关；
- 生命周期由可信宿主负责：绑定的宿主必须活到所有引用该能力表的 TCB 销毁之后，先释放 TCB、再释放能力表、最后释放宿主。能力表不拥有 context，也不提供引用计数或撤销。


## 源码结构

RawAst 记录词、分隔符组、子节点与源码位置。`foo(x)` 的括号形状本身不决定它是函数调用、类型构造还是其他形式；Meta 结合绑定和上下文解释它。

机制层 Reader 只认识 Token 与括号 Group，不解释关键词、类型或调用。词法跳过 `//` 行注释及不嵌套的 `/* ... */` 块注释，`#` 留给 Meta 解释。宿主登记源码时两遍扫描，直接写入预分配的 LAINAST v1 Arena，并只读映射进 Meta 的 VSpace。

当前四个读取能力为 `lain_meta_ast_root(source)`、`lain_meta_ast_node_addr(ref)`、`lain_meta_ast_span_addr(ref,length)` 和 `lain_meta_ast_child_ref(group,index)`。引用采用段号与偏移，Context 来自 CSpace 槽。宿主先验证存活、格式、范围及 READ 权限，再读取或返回已授权地址；非法访问返回非零拒码，VM 停止执行。

Meta 用父组与孩子下标遍历，语法形状写入独立旁表，不复制或改写 AstIn。驱动 `tree` 模式读取平铺输入，`types` 模式读取公开类型摘要。Reader 语法失败报 34，Meta 返回 3；配额、分配和内部自检失败分类处理。旧树追加与替换能力已删除；可写 AstOut、事务预算与 statement 上下文的 `my_if` 展开已经接入生产驱动。预编译语法现经 Meta 内的 `SyntaxBinding` 表查找，五种顶层声明已有独立 declare/lower 入口；DECL/STMT 的 N→M 替换已由 `my_block` 验收；声明级候选根与声明索引在同一事务内验证和发布，函数体展开能读取同批生成声明。DEFER 单轮协调只唤醒具体依赖已就绪的任务。库动态登记、多轮声明闭合队列、表达式处理器和完整宏卫生仍未实现。

### 类型查看接口

`lain_meta_type_publish(descriptor)` 让 Meta 发布一条供诊断使用的类型摘要。`descriptor` 指向已授权的 64 字节，依次是八个 64 位字：`id`、`kind`、`repr_kind`、`repr_width`、`owner`、`namespace`、`arg_start`、`arg_count`。宿主复制该摘要并保存到当前编译请求结束；`types` 模式按发布顺序显示。类型身份、类别与实参区间的意义由 Meta 定义，宿主不据此推断语言规则。发布返回 1 表示成功；零身份或重复身份报 37，地址未经授权报 5，配额不足报 1044。失败不追加条目。宿主释放时归还摘要数组占用的配额。

当前两份独立 Meta 在共同的 `let NAME = INT;` 语料上都发布 `id=1, kind=1, bits/64, owner=0`，并由同一驱动读回。bootstrap 仍维护自己的类型注册表；公开摘要只用于查看，不替代其内部类型表示。

## 语义与 lowering

Meta 可以在内部建立表达式、类型、函数和模块对象。语义处理可以组织为展开（expand）、细化与检查（elaborate）、物理生成（lower）。这三个名字描述职责，当前 v0 ABI 只提供 `initialize` 和 `lower`，没有独立的 expand/elaborate 入口。

- **展开**：按 Meta 规则解释或变换源码形式，并保留诊断需要的来源信息。
- **细化与检查**：建立绑定、解析调用和类型，决定编译期值及布局；失败时给出诊断。
- **物理生成**：把已确定的值、布局和调用转换为 LAINIR，交给验证器、编译期折叠、VM 或后端。

## Meta 调用编译期执行

Meta procedure 解释源码并生成 LAINIR。编译期执行由 Meta 库 eval/apply 负责组织：eval
接收源码形式和环境，按语言规则生成自包含模块；apply 将普通过程、实参、能力与限额交给 seed
的通用执行服务。seed 只执行 LAINIR，不解释 Lain 形式或类型。

