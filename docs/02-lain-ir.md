# LAIN-IR Specification

LAIN-IR 是编译器的基础中间表示。

其文本格式通常使用 `#` 前缀。

## 类型系统

支持以下 6 种类型。

- `#bits<N>`: N 位无符号整数存储 (N=8,16,32,64)。
- `#float<N>`: 浮点存储 (N=32,64)。
- `#vec<N, T>`: SIMD 向量寄存器。(T 必须为 #bits 或 #float)。
- `#addr`: 内存地址。
- `#unit`: 仅含 1 个值，表示函数正常返回且不携带负载。
- `#never`: 不含值，表示控制流发散或不可达。

## 指令

操作数使用 `%` 作为虚拟寄存器标识符。

### 内存与绑定

- `#set %name = expr`: 不可变寄存器绑定（无生命周期语义）
- `#alloca type`: 栈上分配，返回 `#addr`
- `#load %addr ordering`: 内存读取
- `#store %addr %value ordering`: 内存写入
- `#offset %base %bits`: 计算地址偏移 (对应 LEA 指令)

_(内存序 ordering 必须为: relaxed, acquire, release, acqrel, seqcst)_

```Lain-IR
#proc memory_demo() -> #bits<32> {
  #let %buf = #alloca(8)

  #store 10, #lea(base=%buf, offset=0)
  #store 20, #lea(base=%buf, offset=4)

  #let %a = #load(#lea(base=%buf, offset=0))
  #let %b = #load(#lea(base=%buf, offset=4))

  #return #primitive(integer.add, %a, %b)
}
```

### 算术与位操作

- `#add %a %b`, `#sub %a %b`: 加减法 (无符号差异)
- `#smul`, `#umul`, `#sdiv`, `#udiv`, `#srem`, `#urem`: 有符号/无符号乘除与取余
- `#scmp op %a %b`, `#ucmp op %a %b`: 比较指令 (op: lt, le, gt, ge, eq, ne)
- `#fadd`, `#fma`: 浮点加法、融合乘加
- `#popcount`, `#clz`, `#rotl`: 硬件位操作原语映射
  ...

## 控制流（Control Flow）

- `#block name (params) { ... }`
- `#loop name (params) { ... }`
- `#break target_name (args)`
- `#continue target_name (args)`
- `#condbr %cond then_label else_label`
- `#switch %target { case... default }`

```Lain-IR
#proc sum_to_n(#bits<32>) -> #bits<32> {
  #set %n = #arg 0

  #loop sum_loop (0, 0) {
    #set %i = #block_arg 0
    #set %sum = #block_arg 1

    #condbr (#ugt %i %n) block_break block_body

  block_break:
    #break sum_loop (%sum)

  block_body:
    #set %next_sum = #add %sum %i
    #set %next_i = #add %i 1
    #continue sum_loop (%next_i, %next_sum)
  }
}
```

## 子程序与调用 (Subroutines)

SubRoutine，或者说 procedure

- `#proc name(args...) -> ret_type`
- `#call proc_name args...`
- `#tail_call proc_name args...`: 保证尾调用优化
- `#ret %val`: 返回值
- `#ret`: 返回 `#unit`

```Lain-IR
#proc memory_demo() -> #bits<32> {
  #set %buf = #alloca #bits<32>  // 分配 4 字节

  #set %ptr0 = #offset %buf 0
  #store %ptr0 10 relaxed

  #set %val = #load %ptr0 relaxed
  #ret %val
}
```

## 5. 编译期执行 (Compile-Time Execution)

LAIN-IR 具备在编译器宿主内部解释执行自身指令的能力。

### 常量折叠求值 (`#eval`)

触发解释器执行内部区块，求得纯值。

```Lain-IR
#proc main() -> #bits<32> {
  #set %x = #eval #block {
    #set %res = #call fib 10
    #break (%res)
  }
  #ret %x
}
```

### 编译期副作用执行 (`#comptime_call`)

在编译期间执行带有 I/O、文件读写等副作用的物理子程序。

```Lain-IR
#proc generate_bindings() -> #unit {
  #call read_file "api.json"
  #call write_file "bindings.lain"
  #ret
}

#proc main() -> #bits<32> {
  #comptime_call generate_bindings
  #ret 0
}
```

## 物理并发原语

提供绿色线程与协程的最底层支持：

- `#init_context %stack_addr %proc_addr`: 格式化寄存器现场。
- `#swap_context %curr %next`: 强制寄存器溢出并切换 PC。作为绝对的编译期优化屏障。
