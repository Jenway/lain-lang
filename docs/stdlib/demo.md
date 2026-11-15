--- START OF FILE modules.md ---

# Lain-Lang 模块系统规范 (modules.md)

Lain-Lang 编译器底层（Seed、Lain-VM 与 LAINIR）不内置任何模块语法。模块系统完全作为编译期一等实体（First-Class Comptime Modules）在 Lain-Meta 库中实现。在降解为 LAINIR 时，所有模块边界彻底消解，不产生任何运行时对象或虚表开销。

---

## 1. 语法形式与核心实体

模块系统由两类语法构造组成：**结构体模块（Module Structure）** 与 **模块签名（Module Signature）**。

### 1.1 模块结构体 (`module`)
模块是一个命名的静态作用域，用于封装类型、常量、函数与子模块：

```ebnf
ModuleDecl ::= 'let' Ident ('=' | ':') (SignatureExpr)? 'module' '{' ModuleItem* '}'
ModuleItem ::= TypeDecl | StructDecl | ConstDecl | FunctionDecl | ModuleDecl
```

```plain
let MyModule = module {
    type Elem = #bits<32>;
    struct Header {
        magic: #bits<32>,
        len: usize,
    }
    let MAX_SIZE: usize = 4096;

    fn init_header(len: usize) -> Header {
        return Header { magic: 0x4C41494E, len: len };
    }
};
```

* **成员访问**：通过点号语法引用模块成员（如 `MyModule.Header`、`MyModule.init_header(10)`）；
* **作用域导入 (`open` / `use`)**：
  * `open MyModule;`：将模块内所有公共符号注入当前词法作用域；
  * `use MyModule::{ Header, init_header };`：选择性注入指定符号。

---

### 1.2 模块签名 (`sig`)
签名是模块的接口规格契约，描述模块必须具备的类型、值与函数形式：

```ebnf
SignatureDecl ::= 'let' Ident '=' 'sig' '{' SignatureItem* '}'
SignatureItem ::= 'type' Ident ('=' TypeExpr)? ';'
                | 'let' Ident ':' TypeExpr ';'
                | Ident '(' ParamList? ')' ('->' TypeExpr)? ('!' '{' EffectList? '}')? ';'
```

```plain
let Serializer = sig {
    type Target;
    serialize(val: Target, buf: #addr) -> usize;
    deserialize(buf: #addr) -> Option(Target);
};
```

* **类型规格**：签名中的类型可以为抽象类型（未指定具体布局），亦可为具象类型（约束具体物理类型，如 `type Target = #bits<64>;`）；
* **静态核验**：当为模块显式标注签名时（`let M: Serializer = module { ... };`），Meta 在细化（Elaborate）阶段静态核验模块是否完全满足签名要求；不匹配时在定义处拒绝。

---

## 2. 函子：参数化模块 (Functors)

Lain-Lang 不设独立的 `functor` 关键字。函子即**接收编译期参数并返回 `Module` 的普通编译期纯函数**。

### 2.1 泛型容器模式
针对 LAINIR 缺乏类型化指针（仅提供无类型 `#addr`）的物理现实，模块机制通过词法作用域闭包绑定物理存储与类型元数据：

```plain
let Vec = fn(T: Type) -> Module {
    return module {
        // 1. 语义导出：记录元素类型元数据
        type Item = T;

        // 2. 物理数据结构：纯物理裸地址，无伪类型指针
        struct Raw {
            data: #addr,
            len: usize,
            cap: usize,
        }
        type Self = Raw;

        fn new() -> Self {
            return Self { data: 0, len: 0, cap: 0 };
        }

        // 3. 专有操作：词法作用域天然捕获参数 T
        fn get(self: Self, idx: usize) -> Item {
            let offset = idx * sizeof(Item);
            let ptr = #lea(self.data, offset=offset);
            return unsafe {
                #load[Item.physical_type](ptr, align=Item.align)
            };
        }
    };
};
```

### 2.2 函子的纯函数记忆化规则 (Memoization)
* 函子调用遵循纯函数单例缓存规则；
* `Vec(i32)` 与其他编译单元传入的 `Vec(i32)` 返回相同的模块实例；
* 不同的实参（如 `Vec(i32)` 与 `Vec(f64)`）衍生出完全独立的模块命名空间。

---

## 3. 降解与链接性 (Lowering to LAINIR)

模块系统属于编译期前端结构，在下沉至 LAINIR 时执行彻底的扁平化消解：

### 3.1 符号修饰规则 (Name Mangling)
所有嵌套模块中的函数与静态变量，按层级通过唯一的间隔符（`$`）拍平为全局符号：

```plain
// 源码
MyModule.SubModule.compute(x)
// 降解为 LAINIR
#call @MyModule$SubModule$compute(%x)

// 函子实例化的函数
Vec(i32).get(v, 0)
// 降解为 LAINIR
#call @Vec$i32$get(%v, 0)
```

### 3.2 结构体布局消解
* 模块内部定义的结构体，其成员名在降解阶段完全剔除；
* 对 `self.data`、`self.len` 的访问，依照结构体各字段在 Meta 阶段计算出的物理字节偏移，直接降解为 LAINIR 的 `#lea` 指令；
* 模块本身不占用任何运行时物理存储。

--- END OF FILE modules.md ---

--- START OF FILE compilation-units.md ---

# Lain-Lang 编译单元与链接规范 (compilation-units.md)

本规范定义 Lain-Lang 在物理代码生成、增量缓存与链接阶段的核心协议，严格厘清逻辑模块（Module）与物理编译单元（Compilation Unit）的边界。

---

## 1. 核心概念与 M:N 映射

| 维度 | Module（逻辑模块） | Compilation Unit（编译单元） |
| :--- | :--- | :--- |
| **归属层级** | 语言前端与 Meta 语义模型 | 构建系统调度、增量缓存与后端链接器 |
| **生命周期** | 仅存在于编译期（Meta / Lain-VM 内存中） | 持久化于磁盘缓存与目标工件中（`.l1b` / `.o`） |
| **存在形式** | 词法作用域、环境字典、函子闭包 | 自包含的平坦符号表、代码段、数据段 |
| **机器认知** | 底层完全不可见（降解时蒸发） | 直接映射为 ELF/COFF 目标文件或 VM `Image` |

### 映射关系
* **一个编译单元内部可包含多个 Module**：单源码文件内的多个模块在降解时，其符号合并进入同一个编译单元；
* **物理划分完全由构建系统决定**：构建系统可按单个源文件、包（Package）或全局合并（Unity/LTO）模式划分编译单元，不受语言内模块嵌套结构的制约。

---

## 2. 编译单元的物理结构

每个编译单元在降解后，生成一个独立的物理中间工件（`.l1b` 二进制格式）：

### 2.1 结构成份
1. **导出符号表（Exports）**：在当前单元内部完整定义的数据对象（`@data`）与过程体（`#proc`）；
2. **导入符号声明（Imports）**：引用的外部过程签名，统一标记为 `#extern`：
   ```lainir
   // unit_b.l1: 仅记录外部过程签名
   #proc @math$Vector$add(%a: #addr, %b: #addr) -> #addr #extern "math$Vector$add"
   ```
3. **段数据（Segments）**：紧凑二进制编码的指令段、只读数据段与读写数据段。不包含任何未解析的 ASCII 文本字符串。

---

## 3. 增量编译与双哈希机制

为保证大规模工程下修改局部代码不引发全量重编，每个编译单元关联两组哈希：

```text
源码哈希 (Source Hash)       ──> 判定当前单元是否需要重新执行 Meta 降解
导出接口哈希 (Interface Hash) ──> 判定依赖当前单元的其他单元是否需要重新编译
```

### 3.1 缓存判定规则
1. **完全未修改**：当前单元 `Source Hash` 未变，直接从 `.lain-cache/units/<hash>.l1b` 读取预编译镜像，Meta 展开与降解耗时为 0；
2. **私有实现修改（Private Change）**：
   * 单元内部函数体修改，但导出过程的签名、结构体物理布局未变；
   * 当前单元的 `Source Hash` 变更，重新生成 `.l1b`；
   * 当前单元的 `Interface Hash` 保持不变，**所有依赖当前单元的其他编译单元跳过重编**；
3. **公共接口修改（Public Change）**：
   * 当前单元导出函数签名或数据布局改变；
   * `Interface Hash` 改变，下游直接或间接依赖该符号的编译单元级联触发重编。

---

## 4. 符号链接路径

编译单元依据执行环境分流为两条正交的链接路径：

```text
               独立编译单元集合 (.l1b)
                         │
          ┌──────────────┴──────────────┐
          ▼                             ▼
   [编译期执行场景]               [生产发布 AOT 场景]
          │                             │
   VM 内存装载器链接               调用后端生成目标文件 (.o)
          │                             │
   在 VSpace 槽位直填绝对指针      移交原生系统链接器 (lld/ld)
          │                             │
   零 IO 损耗直接运行             产出裸机器二进制可执行文件
```

### 4.1 编译期 VM 内存链接 (In-Memory Linker)
当在编译期运行宏求值、构建脚本或单元测试时：
* 宿主装载器将涉及的多个 `.l1b` 映像直接映射进同一个 `VSpace`；
* 装载器扫描各单元的导入表，从全局符号索引中解析目标过程在代码段的物理基址；
* 将调用点槽位（Slot）直接回填为目标指令偏移；
* 全程在内存指针级别完成，不落盘、不发射中间汇编。

### 4.2 运行期 AOT 原生链接 (Native Linker)
当最终生成脱离虚拟机的本地可执行文件时：
* 后端代码生成器将每个 `.l1b` 翻译为平台标准目标文件（如 Linux ELF `.o`、Windows COFF `.obj`）；
* 过程符号 `@name` 映射为目标文件的导出全局符号；
* 直接调用成熟的系统链接器（如 `lld`）执行最终机器码重定位与消除，完全兼容系统的调试信息（DWARF）与运行时 ABI。

---

## 5. 跨单元单态化去重 (Monomorphization Deduplication)

泛型函子跨编译单元调用时（如单元 A 与单元 B 均调用了 `Vec(i32)`）：
1. 依赖 `concepts.md` 中定义的纯函数记忆化规则，特化生成的符号名全局唯一（`@Vec$i32$get`）；
2. **弱符号折叠**：各单元在生成代码时，将该符号标记为平台链接器的弱符号（COMDAT / `linkonce_odr`）；
3. 原生链接器在最终链接时自动保留一份机器指令副本，丢弃其余冗余定义，避免二进制膨胀。

--- END OF FILE compilation-units.md ---

--- START OF FILE build-system.md ---

# Lain-Lang 编译期构建与包管理规范 (build-system.md)

Lain-Lang 彻底废黜外部独立的构建工具与配置语言（如 CMake、Cargo、TOML）。构建系统与包管理器作为普通标准库（`std::build`）运行在 Lain-VM 的能力沙箱中。

---

## 1. 两阶段自举流水线 (Two-Stage Pipeline)

当执行 `lain build` 命令时，物理执行环境分为严格有序的两阶段：

```text
[阶段 0: 宿主底座启动]
   seed.exe (C 原生程序) 启动，载入内置预编译的 Image_Meta
       │
       ▼
[阶段 1: 编译构建脚本]
   build.lain (源码) ──Reader──> RawAst ──Image_Meta──> build_module.l1 (IR)
       │
       ▼
[阶段 2: 执行构建图]
   在受限 VSpace_Build 中运行 build_module.l1 ──> 产出内存构建有向无环图 (Build DAG)
       │
       ▼
[阶段 3: 驱动目标编译]
   依据 DAG 调度物理线程池并行编译业务源码 ──> 交付终态机器码
```

---

## 2. 构建脚本规约 (`build.lain`)

构建脚本是声明为接收构建图句柄的普通 Meta 函数：

```plain
import std::build;

let configure = fn(b: &mut std::build::Graph) 
    ?{ env: TargetEnv }
{
    let app = b.add_executable("server");
    app.add_source("src/main.lain");

    // 引入依赖包
    let http = b.dependency(?{
        url:  "https://pkg.lain-lang.org/http",
        hash: "sha256:7f83b165...",
    });

    app.link_module("http", http.module());
};
```

---

## 3. 依赖项内存级挂载 (In-Memory Mounting)

包管理器摒弃“下载源码 $\to$ 写入磁盘 $\to$ 重新读盘解析”的传统模式：

1. **确定性拉取**：宿主底座根据构建脚本声明的 URL 与哈希，在宿主全局缓存中检索或下载源码；
2. **内存直接映射**：宿主 Reader 在内存中将依赖包源码切分为 `RawAst Arena` 切片；
3. **零落盘注入**：通过 `VSpace.map_external` 将切片直接只读挂载给主编译任务，依赖包在语法上直接作为 `Module` 呈现，中间不产生任何临时文件。

---

## 4. 构建沙箱防护契约 (Security & Hermetic Invariants)

构建脚本运行在 Lain-VM 的受限环境中，遵循能力安全模型（Capability-based Security）：

1. **默认真空**：禁止直接发起裸系统调用，无未授权文件读写，无任意网络套接字建立权限；
2. **CSpace 受限能力池**：
   * `cap_file_read`：仅限于当前项目工作目录及其子目录；
   * `cap_pkg_fetch`：仅限于拉取哈希值已在代码中硬编码锁定的依赖包；
3. **投毒硬熔断**：任何第三方依赖包的构建逻辑若尝试越权探测宿主私有文件（如 SSH 密钥、环境变量），VM 立即触发 `TRAP_CAP_DENIED` 强行终止编译，从物理层面杜绝供应链恶意代码注入。

--- END OF FILE build-system.md ---

--- START OF FILE desugaring.md ---

# Lain-Lang 标准语法脱糖协议 (desugaring.md)

Lain-Lang 机制层 Reader 仅输出纯形式的 `Token` 与 `Group`，不内置高级语法糖。本规范定义高级语法在 Meta 展开阶段（Expand Phase）的标准 AST 重写协议。

---

## 1. 统一方法调用糖 (UFCS)

为支持面向对象的点号链式调用，同时保持底层函数签名的纯粹性，语言支持 UFCS 重写。

### 1.1 重写规则
当表达式呈现 `receiver.method(args...)` 形式时：

```plain
// 源码形式
v.get(0);

// 展开阶段 AST 重写为：
IntVec.get(v, 0);
```

### 1.2 判定算法
1. 细化器（Elaborator）推导 `receiver` 的静态类型为 `T`；
2. 检索当前词法作用域，寻找导出了 `type Self = T;` 的模块 `M`；
3. 若模块 `M` 中包含签名首个参数为 `Self` 的函数 `method`，展开器将该 AST 节点就地重写为 `M.method(receiver, args...)`。

---

## 2. 惰性格式化代理协议 (Lazy Format Proxy)

为实现零堆内存分配的字符串插值，`f"..."` 宏执行调用点原位代理重写。

### 2.1 展开协议
```plain
// 源码输入
print(f"hello {name}, id: {id}");

// 展开阶段 (Expand) 原位重写为：
print(
    std::fmt::bind((name, id)) ?{ fmt: "hello {}, id: {}" }
);
```

### 2.2 物理步骤
1. 库宏 `f` 扫描字符串字面量中的大括号 `{}`；
2. 提取插值源码片段 `"name"` 与 `"id"`，调用 Reader 解析为子表达式 AST；
3. 提取静态模板字符串 `"hello {}, id: {}"`；
4. 向 `AstOut` 事务缓冲区追加合成代码：构造 `std::fmt::bind` 瞬时未装箱栈结构体，原表达式以引用元组传入，模板字符串作为编译期参数 `?{ fmt: ... }` 挂载；
5. 目标函数接收实现 `Formattable` 协议的对象，直接向底层物理缓冲区（`sink`）流式写入，消除中间 `String` 的内存申请。

---

## 3. 模式匹配与分支决策树脱糖 (Pattern Matching)

`match` 语法不是编译器内置关键字，而是 Meta 库实现的宏表单。

### 3.1 宏表单重写
```plain
// 源码形式
match opt {
    Some(x) => process(x),
    None    => fallback(),
}
```

### 3.2 决策树编译
在展开阶段，Meta 库在 Lain-VM 内部运行标准的 Luc Maranget 模式矩阵编译算法：
1. **穷尽性核验（Exhaustiveness Check）**：静态证明所有可能的数据构造标签均被覆盖；未覆盖时在编译期报错；
2. **冗余分支剔除（Redundancy Check）**：检测不可达的死分支并警告；
3. **物理展开**：将匹配形式转换为底层平坦指令：
   * 读取 Tag 字段；
   * 发射 LAINIR `#switch` 算子；
   * 针对匹配的分支发射基于 `#lea` 的 Payload 字段提取代码；
   * 降解为平坦的基本控制流。

--- END OF FILE desugaring.md ---