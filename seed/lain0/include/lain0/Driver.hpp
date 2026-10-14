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
};

struct CompileResult {
  bool ok = false;
  std::string lainir{};  // 仅 ok 为 true 时有内容
  int verifyCode = 0;    // lainir_verify 的诊断码
  std::string verifyMessage{};
  uint32_t fileId = 0;
};

// 编一个源文件。诊断全进 diags（调用方负责打印）。
CompileResult compileFile(const std::string &path, const CompileOptions &opts,
                          SourceMgr &src, DiagSink &diags);

// 单个 DRC：把已经打出来的 canonical 文本喂回 lainir_parse，
// 固定点要求成立（print(parse(text)) == text）。
// 这是 lain0 的自校验，用来确认 print/parse 双向一致。
bool roundTripCheck(std::string_view text, int &outCode, std::string &outMsg);

} // namespace ln

#endif
