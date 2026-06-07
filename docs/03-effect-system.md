# 效果系统、异步与错误处理

## 效果系统概述

效果系统是运行时层的核心，统一了原本分散的概念：异步、错误处理、不安全操作、IO、内存分配等全部是"效果"。

### 核心语法

```lain
// 函数签名声明效果
fn fetch(url: &str) -> Bytes ! {IO, Suspend, Throws<NetError>}
//                           ↑ 这个函数能做的所有副作用

// 触发效果
perform Throws::throw(error)
perform Suspend::suspend(waker)

// 消费（handle）效果
handle expr with {
    Throws::throw(e) => { log(e); resume(default_value) }
    Suspend::suspend(w) => { event_loop.register(w) }
}

// 纯函数 = 效果集为空
fn add(a: i32, b: i32) -> i32  // ! {}，优化器可最激进处理
```

### 效果统一的概念

| 概念 | 效果 | 说明 |
|------|------|------|
| 异步 | `Suspend` | async/await 即 Suspend 效果的语法糖 |
| 错误处理 | `Throws<E>` | 零开销返回值语义 |
| 不可恢复错误 | `Panic` | unwind 语义，明确付出代价 |
| 不安全操作 | `RawPtr` / `GlobalMut` / `Asm` | 取代 unsafe 关键字 |
| IO | `IO` | 系统调用 |
| 内存分配 | `Alloc` | 堆分配 |
| 并发内存访问 | `MemRelaxed` / `MemAcquire` / `MemRelease` / `MemSeqCst` | 精细化内存序效果 |
| 调用栈追踪 | `Trace` | debug build 自动注入，release 优化掉 |

### 没有 unsafe 关键字

```lain
// 不是这样（Rust 风格）：
unsafe {
    *ptr = 42    // 能做任何危险的事，审计困难
}

// 而是这样：
fn write_ptr(ptr: *mut i32, val: i32) -> () ! {RawPtr} {
    *ptr = val   // 签名精确说明了危险类别
}

// 调用方显式消费效果：
handle write_ptr(ptr, 42) with permit_raw_ptr()
```

效果签名精确说明危险类别（`! {RawPtr}` 表示只有裸指针，没有 GlobalMut/Asm），代码审计可以按效果集合聚焦。

---

## 错误处理

### C++ 异常的性能问题

C++ 异常的实现：throw → 运行时展开调用栈 → 查表找 handler → 调用所有析构函数。happy path 零成本，unhappy path 极贵。问题是 unhappy path（文件不存在、解析失败等）往往是常见路径。

### Lain 的方案：两种不同的效果

```
Throws<E>：返回值语义
  perform Throws::throw(e) → 编译器变换成 return Err(e)
  handle ... with Throws → 编译器变换成 match
  等价于 Rust 的 Result<T,E>，零开销

Panic：unwind 语义
  真正的栈展开
  用于不可恢复的程序员错误
  明确付出代价，签名里可见 ! {Panic}
```

**关键：两者的语义不同，不能混淆。`! {Throws<E>}` 是零开销的，`! {Panic}` 是有 unwind 代价的。选择是显式的。**

### 编译期变换

```lain
// 原始函数
fn foo() -> i32 ! {Throws<ParseError>} {
    let x = parse(s)   // parse 也有 Throws<ParseError>
    x + 1
}

// 变换后（概念上）
fn foo() -> Result<i32, ParseError> {
    let x = match parse(s) {
        Ok(v)  => v,
        Err(e) => return Err(e),
    }
    Ok(x + 1)
}
```

`?` 是 Scheme 宏，展开成 match + return + Trace 帧记录。

### 效果推导

大多数时候不需要手写效果标注，编译器推导：

```lain
fn process(path: str) -> i32 {
    let text = fs::read(path)?     // Throws<IoError>
    let n    = parse_int(text)?    // Throws<ParseError>
    let conn = db::connect()?      // Throws<DbError>
    n
}
// 编译器推导出：! {Throws<IoError>, Throws<ParseError>, Throws<DbError>}
```

只在公开 API 需要手写标注，内部函数全部推导。

### 多种错误合并

```lain
@derive(From)
enum AppError {
    Io(IoError),
    Parse(ParseError),
    Db(DbError),
}

pub fn process(path: str) -> i32 ! {Throws<AppError>} {
    let text = fs::read(path)?    // From<IoError> 自动转换
    let n    = parse_int(text)?   // From<ParseError> 自动转换
    n
}
```

---

## 异步

### 函数颜色问题在效果系统里的解决

关键洞察：**Suspend 效果可以被 handle 掉，所以不存在"函数颜色"问题。** 同一份业务代码，绑定不同的 handler，得到不同的执行模型。

```lain
// 业务逻辑：只是一个普通函数，效果签名完全由编译器推导
fn handle_conn(stream: TcpStream)
    -> () ! {IO, Suspend, Throws<NetError>}
{
    defer stream.close()
    let mut buf = [u8; 4096]
    loop {
        let n = stream.read(&mut buf)?   // Suspend 效果在这里产生
        stream.write(buf[..n])?
    }
}

// main 里选择 executor：
handle accept_loop(listener) with io_uring_executor(threads: 4)
// 换成 thread_executor() 只改这一行，业务代码不动
```

与 Rust 的对比：
- Rust：`handle_conn` 必须是 `async fn`（颜色污染），必须用 `tokio::spawn`（executor 绑定）
- Lain：业务代码无颜色，executor 是库，handle 点替换

### executor 完全解耦

标准库只提供效果声明，不捆绑 executor：

```lain
// 标准库：
effect Suspend {
    fn suspend(waker: Waker) -> ()
}

// 用户/库提供具体 executor：
fn io_uring_executor(threads: u32) -> Handler!{Suspend, Spawn}  // Linux
fn iocp_executor(threads: u32) -> Handler!{Suspend, Spawn}      // Windows
fn thread_pool_executor(threads: u32) -> Handler!{Suspend, Spawn}
fn blocking_executor() -> Handler!{Suspend, Spawn}              // 测试用
```

换 executor 只改一行调用，编译器不强制绑定任何特定运行时。

---

## 效果系统的完整图景

所有 "特殊控制流" 都是效果：

```lain
@effect struct Throws<E>  { fn throw(e: E) -> !        }  // 错误处理
@effect struct Suspend    { fn suspend(w: Waker) -> ()  }  // 异步
@effect struct Alloc      { fn alloc(n: usize) -> ptr   }  // 内存分配
@effect struct IO         { fn syscall(...) -> i64      }  // IO 操作
@effect struct Spawn      { fn spawn(task: fn()) -> ()  }  // 任务调度
@effect struct Trace      { fn record_frame(f: Frame)   }  // 调用栈追踪
@effect struct NonDet     { fn choose() -> bool         }  // 非确定性（测试用）
```

特点：
- 编译器推导效果（不强制手写标注）
- 函数签名完整声明副作用（不可能意外忽略错误路径）
- 不可能出现"意外的 Throws<E>"（编译器会报错："你调用了 ! {Throws<DbError>} 的函数但没有 handle 它"）
- debug build：Trace 自动记录调用帧；release build：noop，优化器删除

---

## @effect 是库

`@effect` 本身是 Scheme 宏（`std/meta/effects/base.scm`），不是编译器内置。它生成 handler 基础设施（vtable + dispatch）。编译器只需要提供 `perform` / `handle` / `resume` 三个原语。
