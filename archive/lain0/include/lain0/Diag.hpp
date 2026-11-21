#ifndef LAIN0_DIAG_HPP
#define LAIN0_DIAG_HPP

#include <cstddef>
#include <string>
#include <vector>

#include "lain0/Source.hpp"
#include <ostream>

namespace ln {

enum class DiagLevel { Note, Warning, Error };

struct Diagnostic {
  DiagLevel level = DiagLevel::Error;
  Span span{};
  int code = 0;
  std::string message;
};

// lain0 自己的诊断号段。
// 注意：不许占用 seed codes.h 已登记的号段
// （1xxx Trap / 2xxx 验证码 / 3xxx 文本解析 / 9xxx image·tcb·backend·apply·meta）。
enum class D : int {
  // 词法 1xx
  LexInvalidChar = 100,
  LexUnterminatedComment = 101,
  LexBadNumber = 102,
  LexUnterminatedString = 103,
  // 语法 2xx
  ParseExpected = 200,
  ParseUnexpectedToken = 201,
  ParseNotAnExpression = 202,
  ParseNotAStatement = 203,
  ParseReserved = 204,
  // 语义 3xx
  SemaUnknownType = 300,
  SemaUnknownName = 301,
  SemaTypeMismatch = 302,
  SemaNoSuchOperator = 303,
  SemaDuplicateDeclaration = 304,
  SemaUnknownMember = 305,
  SemaWidthUnknown = 306,
  SemaAddressEscape = 311,
  SemaBadImport = 312,
  // 降级 4xx
  LowerUnsupported = 400,
  LowerNoReturnValue = 401,
  // 输出 5xx
  VerifyFailed = 500,
  PrintFailed = 501,
  RoundTripFailed = 502,
  // IO 6xx
  IoOpenFailed = 600,
  IoWriteFailed = 601,
};

inline int dcode(D d) { return static_cast<int>(d); }

class DiagSink {
public:
  void report(DiagLevel level, Span span, int code, std::string message);

  void error(Span span, D code, std::string message) {
    report(DiagLevel::Error, span, dcode(code), std::move(message));
  }
  void warn(Span span, D code, std::string message) {
    report(DiagLevel::Warning, span, dcode(code), std::move(message));
  }
  void note(Span span, D code, std::string message) {
    report(DiagLevel::Note, span, dcode(code), std::move(message));
  }

  bool hasErrors() const { return errorCount_ != 0; }
  size_t count() const { return items_.size(); }
  size_t errorCount() const { return errorCount_; }
  const std::vector<Diagnostic> &items() const { return items_; }

  bool anyVanilla() const { return items_.empty(); }

  // 输出形如 "path:line:col: error: msg"，并附一行源码与插入符。
  void printAll(const SourceMgr &src, std::ostream &os) const;

private:
  std::vector<Diagnostic> items_;
  size_t errorCount_ = 0;
};

} // namespace ln

#endif
