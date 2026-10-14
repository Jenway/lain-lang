#ifndef LAIN0_LEXER_HPP
#define LAIN0_LEXER_HPP

#include <cstdint>
#include <string_view>
#include <vector>

#include "lain0/Diag.hpp"
#include "lain0/Source.hpp"

namespace ln {

// 词法层零语义：只认单词与标点，不认语法结构。
//
// 词算子 `and` / `or` 是整词识别的关键字（不是 `&&` / `||`），
// `<<` / `>>` 是双字节标点，`//` 起行注释。
enum class Tok : uint8_t {
  EndOfFile,
  Ident,
  IntLit,
  StrLit,

  // 标点
  LParen,    // (
  RParen,    // )
  LBrace,    // {
  RBrace,    // }
  LBracket,  // [
  RBracket,  // ]
  Comma,     // ,
  Colon,     // :
  Semi,      // ;
  Dot,       // .
  Arrow,     // ->
  Assign,    // =
  Plus,      // +
  Minus,     // -
  Star,      // *
  Slash,     // /
  Percent,   // %
  Lt,        // <
  Gt,        // >
  Le,        // <=
  Ge,        // >=
  EqEq,      // ==
  Ne,        // !=
  Shl,       // <<
  Shr,       // >>
  Amp,       // &
  Pipe,      // |
  Caret,     // ^

  // 关键字
  KwFunc,
  KwLet,
  KwReturn,
  KwIf,
  KwElse,
  KwLoop,
  KwFor,
  KwWhile,
  KwIn,
  KwBreak,
  KwContinue,
  KwStruct,
  KwEnum,
  KwImport,
  KwExtern,
  KwAs,
  KwAnd,
  KwOr,
  KwTrue,
  KwFalse,
  KwRo,
  KwRef,    // ref T：不可变引用（LAINIR 里就是 #addr，附 pointee 类型）
  KwMut,    // mut ref T：可变引用
  KwSlice,  // slice T：胖指针 (addr, len)
};

struct Token {
  Tok kind = Tok::EndOfFile;
  Span span{};
  std::string_view text{}; // 指向源文件缓冲，不拥有
  uint64_t intValue = 0;   // IntLit：已规范化（含 0x 十六进制读成数值）
};

// 词法分析。注释被丢弃，不产生 token。
// 错误进 diags（code 见 D::Lex*），不抛异常。
std::vector<Token> lex(uint32_t fileId, std::string_view src, DiagSink &diags);

// 调试与诊断用：token 种类的稳定名字。
const char *tokName(Tok t);

// Pratt 解析用的优先级表。返回 0 表示不是二元算子。
int binaryPrecedence(Tok t);
bool isRightAssociative(Tok t);

// Tok -> 二元算子；不是二元算子返回 false。
bool tokBinaryOp(Tok t, int &binOp);

} // namespace ln

#endif
