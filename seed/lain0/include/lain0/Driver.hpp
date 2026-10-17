#ifndef LAIN0_DRIVER_HPP
#define LAIN0_DRIVER_HPP

// 编排：源码文件 -> LAINIR 文本。
//
// 固定顺序（不可调换）：
//   SourceMgr.loadFile -> lex -> parseModule -> analyze -> Lowerer.run
//   -> lainir_verify   -> lainir_print_to_string
//
// verify 之前的任何一步失败都不产出文本；verify 失败同样不产出文本。
// 「产物 ast 空但返回成功」在 lain0 里不允许存在 —— 这是 stage 0 那个
// 0 字节静默失败的对立面。

#include <string>
#include <string_view>
#include <vector>

#include "lain0/Ast.hpp"
#include "lain0/Diag.hpp"
#include "lain0/Seed.hpp"
#include "lain0/Sema.hpp"
#include "lain0/Source.hpp"

namespace ln {

struct CompileOptions {
  const char *moduleName = nullptr; // 空则用文件名主干
  bool dumpTokens = false;
  bool dumpAst = false;
  // 每个产物都回喂一次 lainir_parse 做定点自校验（print(parse(text)) == text）。
  // 单文件 / 清单两条路仍由 CLI 在拿到文本后自己查；拆分产出每份都要查，
  // 所以把它放进选项里，由 compileSplit 逐份应用。
  bool roundTrip = false;
};

struct CompileResult {
  bool ok = false;
  std::string lainir{};  // 仅 ok 为 true 时有内容
  int verifyCode = 0;    // lainir_verify 的诊断码
  std::string verifyMessage{};
  uint32_t fileId = 0;
};

// `--entry <文件> --split <目录>` 的产物清单：一个模块一份 `<模块键>.l1`。
struct SplitResult {
  bool ok = false;
  uint32_t fileCount = 0;            // 成功写出的份数
  std::vector<std::string> files{};  // 写出的文件路径，按降级顺序
  int verifyCode = 0;                // 第一份未过 verify 的诊断码（0 = 全过）
  std::string verifyMessage{};
};

// 编一个源文件。诊断全进 diags（调用方负责打印）。
CompileResult compileFile(const std::string &path, const CompileOptions &opts,
                          SourceMgr &src, DiagSink &diags);

// §3.7：按模块清单编一份模块（`lain0 --unit <清单>`）。与 compileFile 共用同一条
// 闸门链：verify 不过不产出文本、空产物算失败。清单打不开时返回 ok=false，
// 诊断里带 `打不开清单 <路径>`。
CompileResult compileUnit(const std::string &manifestPath,
                          const CompileOptions &opts, SourceMgr &src,
                          DiagSink &diags);

// §二（proposal-modules-v2）：按入口求 import 闭包，**每个模块各出一份 LAINIR**，
// 写到 outDir/<模块键>.l1（`std/lex` -> `std_lex.l1`）。没有清单：目录里的文件集合
// 就是产物。闸门一条不少，但落点必须挪：**一份单元自己过不了 verify** —— 跨单元
// 调用的名字不在本单元里（实测 `#call: unknown callee std_parse__meta_source_has_tree`，
// code 2015）。所以闸门按「合并件」走，与驱动装载时做的事完全一样：
//   * 闭包里任何一个模块的降级失败 -> 整体失败（ok=false，diags 已填）；
//   * 各份文本先拼成一份（按同一顺序、每份补一个换行），再 lainir_parse +
//     lainir_verify；不过就**一份文件都不写**，诊断给出合并件里的位置；
//   * 每份打出空文本算失败；写文件失败算失败（不静默丢产物）。
// 目录不存在就建（产物集合就是这个目录）。
SplitResult compileSplit(const std::string &entryPath, const std::string &outDir,
                         const CompileOptions &opts, SourceMgr &src,
                         DiagSink &diags);

// 单个 DRC：把已经打出来的 canonical 文本喂回 lainir_parse，
// 固定点要求成立（print(parse(text)) == text）。
// 这是 lain0 的自校验，用来确认 print/parse 双向一致。
bool roundTripCheck(std::string_view text, int &outCode, std::string &outMsg);

} // namespace ln

#endif
