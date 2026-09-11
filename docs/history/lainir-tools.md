# LAIN-IR 工具链路线与当前边界

> **历史实现记录。** 这套工具已移至 `docs/history/lainir-tools/`，不属于当前 `src/`
> 或 `seed/` 构建路径。下面的路径和命令只描述归档前的实现状态。
> 文中出现的源码路径均为归档前路径，不代表其当前位置。
> LSP 的 C 宿主（`seed/src/host/lsp_host.c`、`seed/src/host/lsp_host.h`、
> `seed/src/cli/lsp_main.c`）已删除：这套工具已归属历史实现，Lain 成熟后将直接
> 用 Lain 重写 LSP，不再保留 C 适配层。LAIN-IR 侧实现保留在
> `docs/history/lainir-tools/`。

目标是让 formatter、语法高亮和 LSP 的“规则”都由 LAIN-IR 执行。C 只做宿主适配：读文件、读写标准输入输出、分配内存。C 不实现词法、格式化、JSON-RPC 或编辑器语义。

## 已落地

- `src/lainir/tools/source.l1`：无损字节扫描器。保留空白和注释 trivia，输出 token、位置和诊断链表。
- `src/lainir/tools/parser.l1`：基于 token 的 dumb AST，保留节点范围、子节点和兄弟链，暂不做类型检查或表达式 lowering。
- `src/lainir/tools/formatter.l1`：格式化入口 `lainir_format`。先构造 parser AST，
  再通过 `parser_ast_boundary` 按 block 节点的 source range 决定结构性花括号的
  换行和缩进；字符串和行注释保持原样，常见运算符使用固定排版规则。
- `src/lainir/tools/highlight.l1`：把 token/trivia 映射为高亮区间，并区分
  directive、keyword、type、function、property 等语义类别。
- `src/lainir/tools/highlight_cli.l1`：入口 `lainir_highlight`，输出 JSON 区间数组。
- `src/lainir/tools/lsp.l1`：Content-Length/CRLF framing、文档存储和协议派发。
- `src/lainir/tools/lsp_tools.l1`：在 LAIN-IR 中接入 source/highlight，并序列化
  `semanticTokens/full`；formatting 使用同一套字节排版规则生成全文 TextEdit。
- `seed/zig-out/bin/lainir-lsp`：只提供 LSP 的 stream/allocator 能力；同时把
  `bootstrap.allocate-pages` 映射到同一个 allocator，方便加载 source/highlight。

## 构建与检查

```text
zig build                         # 在 bootstrap 目录执行
python tests/lainir_tools/run_tools.py
python tests/lainir_tools/lsp_client_e2e.py  # 独立 JSON-RPC 客户端回归
```

formatter：

```text
python scripts/bundle_lainir.py -o formatter_bundle.l1 \
  src/lainir/tools/source.l1 \
  src/lainir/tools/parser.l1 \
  src/lainir/tools/formatter.l1
lainir-seed formatter_bundle.l1 lainir_format out.l1 input.l1
```

高亮模块需要把三个 LAIN-IR 文件合并（合并脚本只处理声明去重，不承载逻辑）：

```text
python scripts/bundle_lainir.py -o highlight_bundle.l1 \
  src/lainir/tools/source.l1 \
  src/lainir/tools/highlight.l1 \
  src/lainir/tools/highlight_cli.l1
lainir-seed highlight_bundle.l1 lainir_highlight highlights.json input.l1
```

LSP 通过标准输入输出运行：

```text
python scripts/bundle_lainir.py -o lsp_bundle.l1 \
  src/lainir/tools/source.l1 \
  src/lainir/tools/highlight.l1 \
  src/lainir/tools/parser.l1 \
  src/lainir/tools/lsp.l1 \
  src/lainir/tools/lsp_tools.l1
lainir-lsp lsp_bundle.l1
```

输入和输出都是标准 JSON-RPC framing：`Content-Length: N\r\n\r\n` 后跟 N 个字节。
当前单个输入 frame 上限为 16 MiB；超限、截断或非法 header 会干净结束连接，
不会写出半个 JSON 响应。
先发送 `textDocument/didOpen`，再发送 `textDocument/semanticTokens/full` 或
`textDocument/formatting`，即可看到对应的真实结果；未打开 URI 时才返回空结果。

semantic token 的固定顺序是：`comment`、`string`、`number`、`directive`、`variable`、
`operator`、`punctuation`、`invalid`、`keyword`、`type`、`function`、`property`。

`didChange` 同时支持 full-sync 和带 `range` 的增量编辑；文档会记录新旧字节
范围、起止行列和 edit generation。带 `range` 的编辑会把重扫窗口扩展到受影响
token 的边界（包括相邻 trivia），只替换这段 token 链；后续 parser 仍从新 token
快照重建 AST，以保证诊断和格式化的一致性。无法找到安全的旧 token 窗口时才退回
整文档扫描。文档还记录最后一次扫描的起止字节、次数和是否使用了 full fallback。

## 下一阶段计划

早期用于检查 RawAst、module、record 和 `std::consteval` 的独立 frontend artifact
已经移除。编译器只保留 `compiler_compile` 这一条正式入口；Meta 编译期执行会在
LAIN-VM 的临时 TCB 路径完成后重新接入。

首个正式编译入口也已经可以运行：

```text
python scripts/build_lain_compiler.py
python scripts/run_lain_compiler.py -o build/lainir/main.l1 tests/lainir_lain/fixtures/function_return.lain
# 多源文件也可以一次传入：
python scripts/run_lain_compiler.py -o build/lainir/main.l1 \
  tests/lainir_lain/fixtures/function_main_external.lain \
  tests/lainir_lain/fixtures/function_helpers.lain
```

它通过 LAIN-IR 的 `compiler_compile` request/result 边界，把一组 `std::func`
声明、物理整数签名、直接调用、局部变量和首批控制流 lower 成可执行 LAIN-IR；
syntax-unit index 复用缓存的 RawAst，顶层 `std::consteval` 会在编译期求值，
未解析 import/循环依赖会在 lowering 前报错。更大范围的类型系统和完整
`src/lainc/COMPILER_SOURCES.txt` 编译器闭包仍在当前自举计划中。

1. **增量扫描边界强化**：为跨 token、字符串、注释和文件尾的编辑补充更多稳定点
   回归；AST 继续按快照重建，避免把语法树复用和 token 拼接混为一谈。
2. **压力与故障宿主测试**：已覆盖大文件、长行、深嵌套、EOF 截断、短写/读、
   allocator 失败和异常退出；继续增加跨平台 CI 样例。
3. **编辑器适配回归**：协议级客户端已经覆盖标准 initialize、didOpen、didChange、
   formatting、semanticTokens、publishDiagnostics 和 shutdown；真实 VS Code/Neovim
   GUI smoke test 留作跨平台 CI 的可选补充。

当前实现已经能真实运行 formatter、highlight、parser 和 LSP framing。LSP 已解析数值
request id、URI、text、version，并维护文档；打开文档后会返回真实 semantic-token 数组、
全文 formatting edit 和 parser diagnostics。当前明确拒绝尚未实现的 `\\u` JSON 转义，避免
把错误数据静默解释成错误文本。
