# Meta v0

范围：`bootstrap/SOURCE_ORDER` 所列的手写 LAINIR Meta 及 `bootstrap/lain/std/prelude.lain`。
宏卫生、完整 effect 和泛型工厂见设计文档，不属于这里列出的公开接口。

## 输入语言

顶层声明包括 `let`、`struct`、`enum`、`func`、`scalar`。import 通过绑定表达式使用。
`scalar` 只能声明在模块中，在被编译的主源码中声明会拒绝，诊断码 14。

```lain
func add3(a: i32, b: i32, c: i32) -> i32 {
  return a + b + c;
}
let main: i32 = add3(3, 4, 5);
```

局部变量需要类型标注；语句需要分号。函数体支持语句序列、局部绑定、return 和专用 for 累加形式。
表达式可以包含括号和嵌套调用。具体已覆盖的形式与限制见 [Meta 进度](../implementation/meta-parser-plan.md)。
不要把单个语料成功推广为所有组合都支持。

```lain
struct Pair { left: i32 right: i32 }
let p = Pair { left: 3, right: 4 };
let s: i32 = Pair.left(p);
```

记录布局由 Meta 计算，并生成字段偏移和大小过程。和类型使用标签与载荷布局；
投影未声明的变体拒绝码为 9，投影无载荷变体拒绝码为 10。
import 按逻辑路径匹配显式注册的源码；找不到模块拒绝码为 11。

## 标量与整数

prelude 中的 scalar 声明定义物理表示和算子表。当前 `i32` 和 `u32` 提供 `/`、`+`、`<`；
前者使用有符号除法和比较，后者使用无符号形式。其他标量的算子表以 prelude 为准。
物理表示为 `bits<32>` 不足以决定采用有符号还是无符号比较。

当前规则保留整数自身的 i32/i64 类型选择。算子解析先尝试上下文；只有两个操作数均为裸字面量、
且上下文找不到算子时，才退回字面量自身类型。有名字的操作数必须保留它的类型信息。
这是源语言规则，与 LAINIR 操作数按指令宽度解释的规则分开。

字面量不能装入声明的表示时拒绝，码 23。当前检查按无符号位容量计算，
因此 `let main: i8 = 255;` 被接受，`300` 被拒绝。不要在文档整理时改成另一套带符号范围规则。
`let main: i8 = 1 + 300;` 当前因物理返回类型不匹配得到 2019，不能把不同失败阶段都描述成码 23。
字面量是否应统一采用上下文/default i32 的新建议尚未替换此规则。

## Meta 物理入口

```lainir
#proc lain_std_abi_version() -> #bits<64>
#proc lain_std_initialize(%host: #addr) -> #bits<32>
#proc lain_std_lower(%host: #addr, %source: #bits<64>) -> #bits<32>
```

以上列出签名，不是可独立解析的完整模块。版本入口返回 1；lower 编译指定源码并把 LAINIR 文本写入宿主输出。
返回 0 表示成功，非零表示诊断。当前驱动直接调用 lower；不能据此宣称它已完成版本协商或阶段生命周期检查。
当前没有独立的 expand/elaborate 入口，也没有五阶段 MetaPassResult 协议。

## 宿主能力

所有能力以 host 地址为第一个显式参数。名称同时用作 link_name。

| 能力 | 用途 |
| --- | --- |
| `lain_meta_source_count` | 源码数量 |
| `lain_meta_source_data` / `lain_meta_source_length` | 源码字节与长度 |
| `lain_meta_source_path_data` | 源码逻辑路径 |
| `lain_meta_emit_reset` / `lain_meta_emit_write` | 重置和追加输出 |
| `lain_meta_emit_data` / `lain_meta_emit_length` | 输出地址与长度 |
| `lain_meta_scratch_data` / `lain_meta_scratch_size` | 暂存区地址与大小 |
| `lain_meta_fail` | 记录诊断码 |

共 11 个能力。完整物理声明在 `bootstrap/std/emit.l1`，宿主实现在 `seed/src/meta/host.c`。
驱动须同时检查 VM 终态、Meta 返回状态和 host.status，随后解析并验证输出。
源文本、路径和 scratch 要显式登记地址权限；传入地址值本身不会授予访问权限。
宿主回调自身的参数检查缺口见 [VM 工作](../implementation/README.md#vm)。

## 内部数据

RawAst 的初始节点描述词与分隔符组；识别 pass 在 Meta 内部补充语义形状。
节点占 48 字节：种类、起点、长度、首孩子、次兄弟、类型。类型槽为 0 表示尚未赋型。
这些偏移是当前 Meta 内部布局，不能当作未来替代 Meta 的公共 ABI。

scratch 布局以 `bootstrap/std/scope.l1` 文件头为单一来源。改变节点大小时，
同步修改 parse 访问器、容量检查、路径缓冲计算及测试驱动中的相关读取。
宿主测试驱动读取内部表只用于诊断；替换 Meta 时仍需处理这项耦合。

算子解析暂存区会被递归改写。先完成递归 lowering，再解析并立即发射物理算子。
实参表按调用层分配，不能让内层调用覆盖外层尚未发出的实参。

## 诊断

| 码 | 含义 |
| --- | --- |
| 3 / 4 / 6 | 缺操作数 / 值后有多余内容 / 类型没有对应算子 |
| 9 / 10 / 11 / 12 | 变体未声明 / 变体无载荷 / 模块缺失 / 非 import 绑定 |
| 14 / 16 / 17 | scalar 位置不合法 / 同层重名 / 暂存区不足 |
| 20 / 21 / 22 / 23 | 缺 return / 缺分号或局部类型 / 重复绑定 / 字面量超出表示容量 |

物理验证器的 2001、2002、2005、2019 分别表示同区域重复绑定、未定义值、操作数类型错误和返回类型错误。
这些失败来源不同，验收需保留具体码。
