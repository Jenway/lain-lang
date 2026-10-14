#include "lain0/Diag.hpp"

#include <ostream>
#include <string>
#include <utility>

namespace ln {
namespace {

const char *levelName(DiagLevel level) {
  switch (level) {
  case DiagLevel::Note:
    return "note";
  case DiagLevel::Warning:
    return "warning";
  case DiagLevel::Error:
    break;
  }
  return "error";
}

} // namespace

void DiagSink::report(DiagLevel level, Span span, int code,
                      std::string message) {
  Diagnostic d;
  d.level = level;
  d.span = span;
  d.code = code;
  d.message = std::move(message);
  items_.push_back(std::move(d));
  if (level == DiagLevel::Error) {
    ++errorCount_;
  }
}

void DiagSink::printAll(const SourceMgr &src, std::ostream &os) const {
  for (const Diagnostic &d : items_) {
    // 没有 Span、或 Span 指向的文件不存在时，位置退化成 <unknown>。
    bool located = d.span.valid();
    if (located && src.path(d.span.file).empty()) {
      located = false;
    }
    if (!located) {
      os << "<unknown>: " << levelName(d.level) << ": " << d.message << " ("
         << d.code << ")\n";
      continue;
    }
    const Pos pos = src.position(d.span.file, d.span.begin);
    os << src.path(d.span.file) << ':' << pos.line << ':' << pos.col << ": "
       << levelName(d.level) << ": " << d.message << " (" << d.code << ")\n";
    const std::string_view line = src.lineText(d.span.file, pos.line);
    if (line.empty()) {
      continue;
    }
    os << "    " << line << "\n";
    // 插入符对齐到列号；列是 1 起始的字节列，因此缩进为 4 + (col - 1)。
    os << std::string(4 + (pos.col != 0 ? pos.col - 1 : 0), ' ') << "^\n";
  }
}

} // namespace ln
