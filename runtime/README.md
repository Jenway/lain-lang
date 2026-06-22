# runtime/ — Lain Async Runtime (设计预留)

这个目录包含 Lain 的异步运行时实现（类似于 Rust 的 Tokio 或 Node.js 的 libuv）。

## 当前状态

**尚未接入编译器。** 运行时实现已完成（线程池、Linux io_uring、Windows IOCP、TCP 网络），
但 Lain 编译器目前还不能编译出需要运行时支持的程序。

## 内容

- `lain_executor.c` — 异步执行器（~1300 行）
- `manifest.toml` — 运行时模块清单

## 将来

当 Lain 支持编译独立可执行程序（而非仅编译为 C 中间代码）时，
这个运行时将成为 Lain 程序的底层执行引擎。
