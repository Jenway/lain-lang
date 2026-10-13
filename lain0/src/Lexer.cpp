#include "lain0/Lexer.hpp"

#include <limits>

#include "lain0/Ast.hpp"

namespace ln {
namespace {

// 字符分类一律按 unsigned char 判定：char 的符号性是实现定义的，
// 直接比较会让人误以为 0x80 以上的字节是负数。
bool isIdentStart(unsigned char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

bool isIdentCont(unsigned char c) {
  return isIdentStart(c) || (c >= '0' && c <= '9');
}

bool isDigit(unsigned char c) { return c >= '0' && c <= '9'; }

int hexValue(unsigned char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool isHorizontalSpace(unsigned char c) { return c == ' ' || c == '\t' || c == '\f' || c == '\v'; }

// 行尾同时认 LF 与 CR：Windows 的 CRLF 里 \r 属于换行，不是文本字符。
bool isLineBreak(unsigned char c) { return c == '\n' || c == '\r'; }

struct KeywordEntry {
  const char *text;
  Tok kind;
};

// 关键字先于标识符匹配，所以表里不需要排序；条目少，线性扫描足够。
constexpr KeywordEntry kKeywords[] = {
    {"and", Tok::KwAnd},       {"as", Tok::KwAs},
    {"break", Tok::KwBreak},     {"continue", Tok::KwContinue}, {"else", Tok::KwElse},
    {"enum", Tok::KwEnum},       {"extern", Tok::KwExtern}, {"false", Tok::KwFalse},
    {"for", Tok::KwFor},         {"func", Tok::KwFunc},     {"if", Tok::KwIf},
    {"import", Tok::KwImport},   {"in", Tok::KwIn},         {"let", Tok::KwLet},
    {"loop", Tok::KwLoop},       {"or", Tok::KwOr},         {"return", Tok::KwReturn},
    {"ro", Tok::KwRo},           {"struct", Tok::KwStruct}, {"true", Tok::KwTrue},
    {"while", Tok::KwWhile},
};

struct Punct2Entry {
  char text[2];
  Tok kind;
};

// 双字节标点必须整表先试，否则 "<=" 会被切成 "<" 加 "="。
constexpr Punct2Entry kPunct2[] = {
    {{'-', '>'}, Tok::Arrow}, {{'<', '='}, Tok::Le},  {{'>', '='}, Tok::Ge},
    {{'=', '='}, Tok::EqEq},  {{'!', '='}, Tok::Ne},  {{'<', '<'}, Tok::Shl},
    {{'>', '>'}, Tok::Shr},
};

struct Punct1Entry {
  char text;
  Tok kind;
};

constexpr Punct1Entry kPunct1[] = {
    {'(', Tok::LParen},   {')', Tok::RParen},   {'{', Tok::LBrace},
    {'}', Tok::RBrace},   {'[', Tok::LBracket}, {']', Tok::RBracket},
    {',', Tok::Comma},    {':', Tok::Colon},    {';', Tok::Semi},
    {'.', Tok::Dot},      {'=', Tok::Assign},   {'+', Tok::Plus},
    {'-', Tok::Minus},    {'*', Tok::Star},     {'/', Tok::Slash},
    {'%', Tok::Percent},  {'<', Tok::Lt},       {'>', Tok::Gt},
    {'&', Tok::Amp},      {'|', Tok::Pipe},     {'^', Tok::Caret},
};

const char *keywordSpelling(Tok kind) {
  for (const KeywordEntry &e : kKeywords) {
    if (e.kind == kind) return e.text;
  }
  return "keyword";
}

// 溢出按 bad number 报，不做回绕：静默截断会让后面的降级拿到错误常量。
bool accumulate(uint64_t &value, uint64_t base, uint64_t digit) {
  constexpr uint64_t kMax = std::numeric_limits<uint64_t>::max();
  if (value > (kMax - digit) / base) return false;
  value = value * base + digit;
  return true;
}

} // namespace

std::vector<Token> lex(uint32_t fileId, std::string_view src, DiagSink &diags) {
  std::vector<Token> out;
  const size_t n = src.size();
  size_t i = 0;

  auto makeSpan = [&](size_t begin, size_t end) {
    return Span{fileId, static_cast<uint32_t>(begin), static_cast<uint32_t>(end)};
  };
  auto emit = [&](Tok kind, size_t begin, size_t end) {
    Token t;
    t.kind = kind;
    t.span = makeSpan(begin, end);
    t.text = src.substr(begin, end - begin);
    out.push_back(t);
  };

  while (i < n) {
    const unsigned char c = static_cast<unsigned char>(src[i]);

    if (isHorizontalSpace(c) || isLineBreak(c)) {
      ++i;
      continue;
    }

    // 行注释：吃至行尾，不产生 token。
    if (c == '/' && i + 1 < n && src[i + 1] == '/') {
      while (i < n && !isLineBreak(static_cast<unsigned char>(src[i]))) ++i;
      continue;
    }

    if (isIdentStart(c)) {
      const size_t begin = i;
      while (i < n && isIdentCont(static_cast<unsigned char>(src[i]))) ++i;
      const std::string_view word = src.substr(begin, i - begin);

      Tok kind = Tok::Ident;
      for (const KeywordEntry &e : kKeywords) {
        if (word == e.text) {
          kind = e.kind;
          break;
        }
      }
      emit(kind, begin, i);
      continue;
    }

    if (isDigit(c)) {
      const size_t begin = i;
      uint64_t value = 0;
      bool bad = false;

      if (c == '0' && i + 1 < n && (src[i + 1] == 'x' || src[i + 1] == 'X')) {
        i += 2;
        size_t digits = 0;
        while (i < n) {
          const int d = hexValue(static_cast<unsigned char>(src[i]));
          if (d < 0) break;
          if (!accumulate(value, 16, static_cast<uint64_t>(d))) bad = true;
          ++i;
          ++digits;
        }
        // "0x" 后面一个十六进制数字都没有，是残缺的字面量。
        if (digits == 0) bad = true;
      } else {
        while (i < n && isDigit(static_cast<unsigned char>(src[i]))) {
          const uint64_t d = static_cast<uint64_t>(src[i] - '0');
          if (!accumulate(value, 10, d)) bad = true;
          ++i;
        }
      }

      // 数字与标识符粘连（0xZZ、12abc）整段算一个坏字面量：
      // 把粘连部分并进同一个 span，避免同一个错误报两次。
      if (i < n && isIdentCont(static_cast<unsigned char>(src[i]))) {
        bad = true;
        while (i < n && isIdentCont(static_cast<unsigned char>(src[i]))) ++i;
      }

      if (bad) {
        value = 0;
        diags.error(makeSpan(begin, i), D::LexBadNumber, "整数字面量不合法");
      }
      Token t;
      t.kind = Tok::IntLit;
      t.span = makeSpan(begin, i);
      t.text = src.substr(begin, i - begin);
      t.intValue = value;
      out.push_back(t);
      continue;
    }

    // 字符串字面量："…"。原文原样保留（含非 ASCII 字节），转义与字节化留给
    // 后面的阶段；docs/04 的口径是「字节按源码原样搬」。
    if (c == '"') {
      const size_t begin = i;
      ++i;
      size_t end = i;
      bool closed = false;
      while (i < n) {
        const char ch = src[i];
        ++i;
        if (ch == '"') {
          closed = true;
          end = i - 1; // 不含收尾引号
          break;
        }
      }
      if (!closed) {
        diags.error(makeSpan(begin, i), D::LexInvalidChar,
                    "字符串字面量没有收尾的引号");
        continue;
      }
      Token t;
      t.kind = Tok::StrLit;
      t.span = makeSpan(begin, i);
      t.text = src.substr(begin + 1, end - (begin + 1)); // 去掉两侧引号
      out.push_back(t);
      continue;
    }

    bool matched = false;
    if (i + 1 < n) {
      const unsigned char d = static_cast<unsigned char>(src[i + 1]);
      for (const Punct2Entry &e : kPunct2) {
        if (static_cast<unsigned char>(e.text[0]) == c &&
            static_cast<unsigned char>(e.text[1]) == d) {
          emit(e.kind, i, i + 2);
          i += 2;
          matched = true;
          break;
        }
      }
    }
    if (matched) continue;

    for (const Punct1Entry &e : kPunct1) {
      if (static_cast<unsigned char>(e.text) == c) {
        emit(e.kind, i, i + 1);
        ++i;
        matched = true;
        break;
      }
    }
    if (matched) continue;

    // 走到这里是不认识的字节（含 >= 0x80 的非 ASCII 字节与落单的 '!'）。
    diags.error(makeSpan(i, i + 1), D::LexInvalidChar, "无法识别的字符");
    ++i;
  }

  Token eof;
  eof.kind = Tok::EndOfFile;
  eof.span = makeSpan(n, n);
  eof.text = src.substr(n, 0);
  out.push_back(eof);
  return out;
}

const char *tokName(Tok t) {
  switch (t) {
  case Tok::EndOfFile: return "end of file";
  case Tok::Ident: return "identifier";
  case Tok::IntLit: return "integer literal";
  case Tok::StrLit: return "string literal";

  case Tok::LParen: return "(";
  case Tok::RParen: return ")";
  case Tok::LBrace: return "{";
  case Tok::RBrace: return "}";
  case Tok::LBracket: return "[";
  case Tok::RBracket: return "]";
  case Tok::Comma: return ",";
  case Tok::Colon: return ":";
  case Tok::Semi: return ";";
  case Tok::Dot: return ".";
  case Tok::Arrow: return "->";
  case Tok::Assign: return "=";
  case Tok::Plus: return "+";
  case Tok::Minus: return "-";
  case Tok::Star: return "*";
  case Tok::Slash: return "/";
  case Tok::Percent: return "%";
  case Tok::Lt: return "<";
  case Tok::Gt: return ">";
  case Tok::Le: return "<=";
  case Tok::Ge: return ">=";
  case Tok::EqEq: return "==";
  case Tok::Ne: return "!=";
  case Tok::Shl: return "<<";
  case Tok::Shr: return ">>";
  case Tok::Amp: return "&";
  case Tok::Pipe: return "|";
  case Tok::Caret: return "^";

  case Tok::KwFunc: case Tok::KwLet: case Tok::KwReturn: case Tok::KwIf:
  case Tok::KwElse: case Tok::KwLoop: case Tok::KwFor: case Tok::KwWhile:
  case Tok::KwIn: case Tok::KwBreak: case Tok::KwContinue:
  case Tok::KwStruct: case Tok::KwEnum: case Tok::KwImport: case Tok::KwExtern:
  case Tok::KwAs: case Tok::KwAnd: case Tok::KwOr: case Tok::KwTrue:
  case Tok::KwFalse: case Tok::KwRo:
    return keywordSpelling(t);
  }
  return "token";
}

// C 风格优先级：or < and < | < ^ < & < 相等 < 关系 < 移位 < 加减 < 乘除余。
// 位算子不在 T2 清单里，但 ast::BinOp 有对应算子，按 C 的相对次序插在 and 与相等之间，
// 清单里列出的那些层级的相对次序不受影响。
int binaryPrecedence(Tok t) {
  switch (t) {
  case Tok::KwOr: return 1;
  case Tok::KwAnd: return 2;
  case Tok::Pipe: return 3;
  case Tok::Caret: return 4;
  case Tok::Amp: return 5;
  case Tok::EqEq: case Tok::Ne: return 6;
  case Tok::Lt: case Tok::Le: case Tok::Gt: case Tok::Ge: return 7;
  case Tok::Shl: case Tok::Shr: return 8;
  case Tok::Plus: case Tok::Minus: return 9;
  case Tok::Star: case Tok::Slash: case Tok::Percent: return 10;
  default: return 0; // 不是二元算子
  }
}

// 这一层的二元算子全部左结合：ast::BinOp 里没有需要右折叠的算子。
// 参数留着是为了让优先级表的调用点读起来对称，改动时不必改签名。
bool isRightAssociative(Tok t) {
  (void)t;
  return false;
}

bool tokBinaryOp(Tok t, int &binOp) {
  using ast::BinOp;
  switch (t) {
  case Tok::Plus: binOp = static_cast<int>(BinOp::Add); return true;
  case Tok::Minus: binOp = static_cast<int>(BinOp::Sub); return true;
  case Tok::Star: binOp = static_cast<int>(BinOp::Mul); return true;
  case Tok::Slash: binOp = static_cast<int>(BinOp::Div); return true;
  case Tok::Percent: binOp = static_cast<int>(BinOp::Rem); return true;
  case Tok::Shl: binOp = static_cast<int>(BinOp::Shl); return true;
  case Tok::Shr: binOp = static_cast<int>(BinOp::Shr); return true;
  case Tok::Amp: binOp = static_cast<int>(BinOp::BitAnd); return true;
  case Tok::Pipe: binOp = static_cast<int>(BinOp::BitOr); return true;
  case Tok::Caret: binOp = static_cast<int>(BinOp::BitXor); return true;
  case Tok::EqEq: binOp = static_cast<int>(BinOp::Eq); return true;
  case Tok::Ne: binOp = static_cast<int>(BinOp::Ne); return true;
  case Tok::Lt: binOp = static_cast<int>(BinOp::Lt); return true;
  case Tok::Le: binOp = static_cast<int>(BinOp::Le); return true;
  case Tok::Gt: binOp = static_cast<int>(BinOp::Gt); return true;
  case Tok::Ge: binOp = static_cast<int>(BinOp::Ge); return true;
  case Tok::KwAnd: binOp = static_cast<int>(BinOp::LogicalAnd); return true;
  case Tok::KwOr: binOp = static_cast<int>(BinOp::LogicalOr); return true;
  default: return false;
  }
}

} // namespace ln
