# 效果系统、异步与错误处理设计

## 效果系统的本质

在 Lain 中，**效果系统（Effect System）是整个运行时层的语义核心。**
它并不是一个单纯的语言特性，而是**一种将“不纯粹的、涉及硬件的控制流改变”以强类型方式进行声明和静态审计的机制**。

任何涉及物理硬件或运行状态的操作，在底层都映射为了精细化的效果行：

```
                        Lain 运行时效果映射
┌─────────────────────────────────┬──────────────────────────────────┐
│             高级效果            │         降级后的物理 L1 映射      │
├─────────────────────────────────┼──────────────────────────────────┤
│  IO (系统调用)                  │  #syscall 原语                   │
├─────────────────────────────────┼──────────────────────────────────┤
│  RawPtr (裸指针读写)             │  #load / #store 物理指令         │
├─────────────────────────────────┼──────────────────────────────────┤
│  Throws<E> (业务预期失败)       │  无 Unwind, 降解为普通 C 返回值  │
├─────────────────────────────────┼──────────────────────────────────┤
│  Suspend (异步挂起)             │  #swap_context 上下文置换        │
└─────────────────────────────────┴──────────────────────────────────┘
```

---

## 一、 错误处理的双轨制分治

Lain 拒绝用同一种机制处理所有“Unhappy Path”。我们根据错误的物理性质，将其彻底解耦：

### 1. 预期业务失败：`Throws<E>` 效果 (零展开成本)

像文件不存在、解析失败等，属于“预期的业务流程分支”。

- **物理实现**：它绝对不使用昂贵、复杂的运行时栈展开（Unwinding）和异常查表。Scheme 宏会在编译期，将 `Throws<E>` **完全重写并降级为最普通的 C 语言 Tagged Union 返回值（即结构体 `{ uint8_t tag, T val, E err }`）**。
- **性能特征**：在物理汇编上只产生最简单的 `cmp` 和 `jmp` 指令。正常路径（Happy Path）与错误路径（Unhappy Path）的执行速度完全一致。

### 2. 致命代码 Bug：`#trap()` / Abort (零内存垃圾)

像数组越界、空地址访问、或安全断言（Assert）失败，这属于“程序员的逻辑漏洞”，在物理上是不可恢复的。如果强行捕获并继续运行，会产生灾难性的内存损坏风险。

- **物理实现**：L1 编译器遇到此类崩溃时，**直接在 Codegen 阶段发射单条两字节的 `#trap()` 硬件陷阱指令**（在 x86 上为 `ud2`，在 ARM 上为 `brk`）。
- **性能特征**：它在物理文件里不产生任何异常保护（EH）恢复表，不占用任何内存体积。一旦触发，CPU 硬件瞬间抛出中断自毁，彻底阻断黑客利用“受损内存状态”进行安全溢出攻击的可能。

---

## 二、 异步与协程（`Suspend` 效果的物理闭环）

在 Rust 等传统语言中，异步会带来著名的 **“函数颜色问题（What Color is Your Function）”**：红色的 `async` 函数不能在蓝色的同步函数里直接调用，导致颜色污染。

在 Lain 中，**因为 Suspend 效果可以被 handle 掉，函数颜色问题在物理层面彻底消失了。**

### 1. 零污染的同步表面：

高层网络库在写 read 时，表面上是一个完全干净、没有任何 `async` 关键字的同步风格函数：

```rust
pub impl TcpStream {
    pub fn read(self: &TcpStream, buf: &mut [u8]) -> usize ! {IO, Suspend, Throws<NetError>} {
        loop {
            match raw_socket_read(self.fd, buf) {
                Ok(n) => return n,
                Err(EWouldBlock) => {
                    // 1. 如果没数据，构造 Waker
                    let w = make_waker(self.fd, get_current_task())
                    // 2. 注册到底层的 io_uring 中
                    runtime::register_io_uring(self.fd, w)
                    // 3. 【触发挂起】：
                    //    L1 编译器看到该 perform，会强制溢出所有活跃寄存器回栈
                    //    并调用 #swap_context 切回主调度器！
                    perform Suspend::suspend(w)
                }
                Err(e) => perform Throws::throw(NetError::from(e))?
            }
        }
    }
}
```

### 2. 效果处理器与 Executor 100% 解耦

因为 `Suspend` 只是一个普通的库效果（`std/effects/suspend.lain`），它不与任何特定的运行时绑定。
用户可以在最外层，通过一行 `handle` 来任意决定使用什么样的异步调度引擎：

```rust
fn main() {
    handle {
        accept_loop()
    } with {
        // 当协程内部触发 Suspend 时，在这里被拦截：
        Suspend::suspend(waker) => {
            // 保存当前协程上下文，利用 L1 #swap_context 原语切回物理线程调度器
            l1::swap_context(waker.task.ctx_ptr, &mut sched_ctx as l1::addr)
        }
    }
    // 这里决定了它的执行策略：
    // 换成 thread_pool_executor(threads: 8) 或者单线程 event_loop 只需要改这一行，
    // 内部所有的 TcpStream 业务代码一字不改！
    with io_uring_executor(threads: 4)
}
```

---

## 三、 无 `unsafe` 的安全借用审计（`RawPtr` 效果）

Lain 废除了粗暴的 `unsafe` 关键字。我们通过精准的效果细分来约束硬件操作：

```rust
// 1. 这不是 unsafe 函数，它只是一个普通的、标记了物理效果的函数
fn raw_copy(dst: l1::addr, src: l1::addr, n: usize) -> () ! {RawPtrWrite, RawPtrRead} {
    let mut i = 0
    while i < n {
        // L1 物理内存存储：直接对应 mov 机器指令
        l1::store(dst + i, l1::load(src + i, l1::bits(8)), relaxed)
        i += 1
    }
}

// 2. 调用者必须显式 handle 这些效果：
fn safe_api(dst: &mut [u8], src: &[u8]) -> () {
    handle {
        raw_copy(dst.ptr, src.ptr, src.len)
    } with permit_raw_ptr() // 显式消除 RawPtr 效果，代表“我为这次物理操作的安全负责”
}
```

这带来了极致细粒度的安全审计：代码审计者不需要阅读成千上万行带有 `unsafe` 的大代码块。他们只需要查看函数签名，就能立刻区分：

- 这个函数是否操作了裸指针（`! {RawPtrWrite}`）？
- 它是否操作了共享内存（`! {MemAcqRel}`）？
- 它是否调用了底层汇编（`! {Asm}`）？

所有的危险物理行为，都在编译期被效果系统死死盯防，并以最高效的物理指令低成本运行。
