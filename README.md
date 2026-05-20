# A toy language

现在我想要实现一个玩具语言

基于 LLVM IR

## 类型

静态强类型，类似于 `rust` 的类型机制, `u8`、`i32` 之类的

支持 tagged union

当然还有模式匹配


## 机制

### 效果系统（effect）

函数签名可携带效果集合，表明该函数可能产生的副作用。编译器对效果进行静态推导、合并与检查。

示例：
```rust
fn accept(self: &TcpListener) 
    -> TcpStream ! {IO, Suspend, Throws<NetError>} 
```

### RAII

提供 `defer` 机制

### 多态

支持静态多态 - interface

动态多态暂时先不支持


