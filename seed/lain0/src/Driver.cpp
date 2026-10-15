
// 编排：源码文件 -> LAINIR 文本。
//
// 顺序在 Driver.hpp 里已经定死：loadFile -> lex -> parseModule -> analyze
// -> Lowerer.run -> lainir_verify -> lainir_print_to_string。这里只负责把这条
// 链串起来，并在每一道闸门前决定还能不能往下走：
//
//   * 闸门之前的任何一步失败，CompileResult::lainir 保持空 ——
//     空产物加成功是 stage 0 那个 0 字节静默失败，lain0 不允许它复活；
//   * verify 的诊断码 / 行 / 列 / 消息要原样带回，定位权在 L1Diagnostic 自己；
//   * 打印出来是空文本同样算失败，绝不放行。

#include "lain0/Driver.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "lain0/Lexer.hpp"
#include "lain0/Lower.hpp"
#include "lain0/Parser.hpp"
#include "lain0/Seed.hpp"

namespace ln {
namespace {

// 模块名：显式给了就用给的，否则取文件名主干（去掉目录与最后一个扩展名）。
// 主干退化为空（点文件、以分隔符结尾）时退回 "lain" —— IR 的模块名不能是空串。
std::string moduleNameOf(const std::string &path, const CompileOptions &opts) {
  if (opts.moduleName != nullptr && opts.moduleName[0] != '\0') {
    return std::string(opts.moduleName);
  }
  const size_t slash = path.find_last_of("/\\");
  std::string base =
      (slash == std::string::npos) ? path : path.substr(slash + 1);
  const size_t dot = base.rfind('.');
  if (dot != std::string::npos && dot != 0) {
    base = base.substr(0, dot);
  }
  if (base.empty()) {
    base = "lain";
  }
  return base;
}

// L1Diagnostic 给的是行/列，lain0 的 Span 要字节偏移。SourceMgr 没有反查接口，
// 所以按行首表自己数一遍；越界的行/列夹到文本末尾，诊断退化到 EOF 而不是乱码。
uint32_t offsetOfLineCol(std::string_view text, uint32_t line, uint32_t col) {
  uint32_t off = 0;
  for (uint32_t l = 1; l < line; ++l) {
    const size_t nl = text.find('\n', off);
    if (nl == std::string_view::npos) {
      return static_cast<uint32_t>(text.size());
    }
    off = static_cast<uint32_t>(nl) + 1;
  }
  const uint32_t c = (col != 0) ? col - 1 : 0;
  const size_t at = static_cast<size_t>(off) + c;
  return static_cast<uint32_t>(std::min<size_t>(at, text.size()));
}

// ---------------- 调试转储 ----------------
// 两个 dump 只在命令行显式要求时跑，输出走 stdout，不进产物。

std::string typeSyntaxText(const ast::TypeSyntax &ts) {
  std::string s;
  if (ts.isRo) {
    s += "ro ";
  }
  if (ts.isArray) {
    s += "[";
    s += ts.element ? typeSyntaxText(*ts.element) : std::string("?");
    s += "]";
    return s;
  }
  if (ts.hasQualifier) {
    s += ts.qualifier;
    s += ".";
  }
  s += ts.name.empty() ? std::string_view("?") : ts.name;
  return s;
}

const char *binOpName(ast::BinOp op) {
  switch (op) {
  case ast::BinOp::Add: return "+";
  case ast::BinOp::Sub: return "-";
  case ast::BinOp::Mul: return "*";
  case ast::BinOp::Div: return "/";
  case ast::BinOp::Rem: return "%";
  case ast::BinOp::Shl: return "<<";
  case ast::BinOp::Shr: return ">>";
  case ast::BinOp::BitAnd: return "&";
  case ast::BinOp::BitOr: return "|";
  case ast::BinOp::BitXor: return "^";
  case ast::BinOp::Eq: return "==";
  case ast::BinOp::Ne: return "!=";
  case ast::BinOp::Lt: return "<";
  case ast::BinOp::Le: return "<=";
  case ast::BinOp::Gt: return ">";
  case ast::BinOp::Ge: return ">=";
  case ast::BinOp::LogicalAnd: return "and";
  case ast::BinOp::LogicalOr: return "or";
  }
  return "?";
}

void dumpExpr(const ast::Expr *e, std::ostream &os, int indent);
void dumpBody(const std::vector<ast::Stmt *> &body, std::ostream &os, int indent);

void dumpExpr(const ast::Expr *e, std::ostream &os, int indent) {
  const std::string pad(static_cast<size_t>(indent) * 2, ' ');
  if (e == nullptr) {
    os << pad << "<null-expr>\n";
    return;
  }
  switch (e->kind) {
  case ast::ExprKind::IntLit:
    os << pad << "int " << static_cast<const ast::IntLitExpr *>(e)->value << "\n";
    return;
  case ast::ExprKind::StrLit:
    os << pad << "str \"" << static_cast<const ast::StrLitExpr *>(e)->rawText
       << "\"\n";
    return;
  case ast::ExprKind::BoolLit:
    os << pad << "bool "
       << (static_cast<const ast::BoolLitExpr *>(e)->value ? "true" : "false")
       << "\n";
    return;
  case ast::ExprKind::Name:
    os << pad << "name " << static_cast<const ast::NameExpr *>(e)->name << "\n";
    return;
  case ast::ExprKind::Dotted: {
    const auto *d = static_cast<const ast::DottedExpr *>(e);
    os << pad << "dot ." << d->member << "\n";
    dumpExpr(d->object, os, indent + 1);
    return;
  }
  case ast::ExprKind::Call: {
    const auto *c = static_cast<const ast::CallExpr *>(e);
    os << pad << "call\n";
    dumpExpr(c->callee, os, indent + 1);
    for (const ast::Expr *a : c->args) {
      dumpExpr(a, os, indent + 1);
    }
    return;
  }
  case ast::ExprKind::Index: {
    const auto *i = static_cast<const ast::IndexExpr *>(e);
    os << pad << "index\n";
    dumpExpr(i->base, os, indent + 1);
    dumpExpr(i->index, os, indent + 1);
    return;
  }
  case ast::ExprKind::Unary: {
    const auto *u = static_cast<const ast::UnaryExpr *>(e);
    os << pad << "neg\n";
    dumpExpr(u->operand, os, indent + 1);
    return;
  }
  case ast::ExprKind::Binary: {
    const auto *b = static_cast<const ast::BinaryExpr *>(e);
    os << pad << "binary " << binOpName(b->op) << "\n";
    dumpExpr(b->lhs, os, indent + 1);
    dumpExpr(b->rhs, os, indent + 1);
    return;
  }
  case ast::ExprKind::Cast: {
    const auto *c = static_cast<const ast::CastExpr *>(e);
    os << pad << "cast as " << typeSyntaxText(c->target) << "\n";
    dumpExpr(c->operand, os, indent + 1);
    return;
  }
  case ast::ExprKind::Construct: {
    const auto *c = static_cast<const ast::ConstructExpr *>(e);
    os << pad << "construct " << typeSyntaxText(c->target) << "\n";
    for (const ast::FieldInit &f : c->fields) {
      os << std::string(static_cast<size_t>(indent + 1) * 2, ' ') << f.name
         << ":\n";
      dumpExpr(f.value, os, indent + 2);
    }
    return;
  }
  case ast::ExprKind::Table: {
    const auto *t = static_cast<const ast::TableExpr *>(e);
    os << pad << "table\n";
    for (const ast::Expr *el : t->elements) {
      dumpExpr(el, os, indent + 1);
    }
    return;
  }
  case ast::ExprKind::ValueIf: {
    const auto *v = static_cast<const ast::ValueIfExpr *>(e);
    os << pad << "value-if\n";
    dumpExpr(v->cond, os, indent + 1);
    os << std::string(static_cast<size_t>(indent + 1) * 2, ' ') << "then\n";
    dumpBody(v->thenBody, os, indent + 2);
    if (v->hasElse) {
      os << std::string(static_cast<size_t>(indent + 1) * 2, ' ') << "else\n";
      dumpBody(v->elseBody, os, indent + 2);
    }
    return;
  }
  }
  os << pad << "<expr?>\n";
}

void dumpStmt(const ast::Stmt *s, std::ostream &os, int indent) {
  const std::string pad(static_cast<size_t>(indent) * 2, ' ');
  if (s == nullptr) {
    os << pad << "<null-stmt>\n";
    return;
  }
  switch (s->kind) {
  case ast::StmtKind::Let: {
    const auto *l = static_cast<const ast::LetStmt *>(s);
    os << pad << "let " << l->name;
    if (l->hasType) {
      os << ": " << typeSyntaxText(l->type);
    }
    os << "\n";
    dumpExpr(l->init, os, indent + 1);
    return;
  }
  case ast::StmtKind::Return: {
    const auto *r = static_cast<const ast::ReturnStmt *>(s);
    os << pad << "return\n";
    if (r->value != nullptr) {
      dumpExpr(r->value, os, indent + 1);
    }
    return;
  }
  case ast::StmtKind::Assign: {
    const auto *a = static_cast<const ast::AssignStmt *>(s);
    os << pad << "assign\n";
    dumpExpr(a->target, os, indent + 1);
    dumpExpr(a->value, os, indent + 1);
    return;
  }
  case ast::StmtKind::ExprStmt: {
    os << pad << "expr-stmt\n";
    dumpExpr(static_cast<const ast::ExprStmt *>(s)->expr, os, indent + 1);
    return;
  }
  case ast::StmtKind::If: {
    const auto *i = static_cast<const ast::IfStmt *>(s);
    os << pad << "if\n";
    dumpExpr(i->cond, os, indent + 1);
    os << std::string(static_cast<size_t>(indent + 1) * 2, ' ') << "then\n";
    dumpBody(i->thenBody, os, indent + 2);
    if (i->hasElse) {
      os << std::string(static_cast<size_t>(indent + 1) * 2, ' ') << "else\n";
      dumpBody(i->elseBody, os, indent + 2);
    }
    return;
  }
  case ast::StmtKind::Loop: {
    const auto *l = static_cast<const ast::LoopStmt *>(s);
    os << pad << "loop";
    if (l->hasLabel) {
      os << " " << l->label;
    }
    os << "\n";
    for (const ast::LoopParam &p : l->params) {
      os << std::string(static_cast<size_t>(indent + 1) * 2, ' ') << p.name
         << ": " << typeSyntaxText(p.type) << "\n";
      if (p.init != nullptr) {
        dumpExpr(p.init, os, indent + 2);
      }
    }
    if (l->cond != nullptr) {
      os << std::string(static_cast<size_t>(indent + 1) * 2, ' ') << "while\n";
      dumpExpr(l->cond, os, indent + 2);
    }
    dumpBody(l->body, os, indent + 1);
    return;
  }
  case ast::StmtKind::For: {
    const auto *f = static_cast<const ast::ForStmt *>(s);
    os << pad << "for " << f->var << " in ..\n";
    dumpExpr(f->from, os, indent + 1);
    dumpExpr(f->to, os, indent + 1);
    if (f->hasAcc) {
      os << std::string(static_cast<size_t>(indent + 1) * 2, ' ') << "acc "
         << f->acc << ": " << typeSyntaxText(f->accType) << "\n";
      dumpExpr(f->accInit, os, indent + 2);
    }
    dumpBody(f->body, os, indent + 1);
    return;
  }
  case ast::StmtKind::Jump: {
    const auto *j = static_cast<const ast::JumpStmt *>(s);
    os << pad << (j->isBreak ? "break " : "continue ") << j->label << "\n";
    for (const ast::Expr *a : j->args) {
      dumpExpr(a, os, indent + 1);
    }
    return;
  }
  }
  os << pad << "<stmt?>\n";
}

void dumpBody(const std::vector<ast::Stmt *> &body, std::ostream &os,
              int indent) {
  for (const ast::Stmt *s : body) {
    dumpStmt(s, os, indent);
  }
}

void dumpDecl(const ast::Decl *d, std::ostream &os) {
  const std::string pad(2, ' ');
  if (d == nullptr) {
    os << pad << "<null-decl>\n";
    return;
  }
  switch (d->kind) {
  case ast::DeclKind::Func: {
    const auto *f = static_cast<const ast::FuncDecl *>(d);
    os << "func " << f->name << "(";
    for (size_t i = 0; i < f->params.size(); ++i) {
      if (i != 0) {
        os << ", ";
      }
      os << f->params[i].name << ": " << typeSyntaxText(f->params[i].type);
    }
    os << ")";
    if (f->hasRet) {
      os << " -> " << typeSyntaxText(f->ret);
    }
    os << "\n";
    dumpBody(f->body, os, 2);
    return;
  }
  case ast::DeclKind::Extern: {
    const auto *e = static_cast<const ast::ExternDecl *>(d);
    os << "extern " << e->name << "(";
    for (size_t i = 0; i < e->params.size(); ++i) {
      if (i != 0) {
        os << ", ";
      }
      os << e->params[i].name << ": " << typeSyntaxText(e->params[i].type);
    }
    os << ")";
    if (e->hasRet) {
      os << " -> " << typeSyntaxText(e->ret);
    }
    os << " = " << e->linkName << "\n";
    return;
  }
  case ast::DeclKind::Struct: {
    const auto *s = static_cast<const ast::StructDecl *>(d);
    os << "struct " << s->name << "\n";
    for (const ast::FieldDecl &f : s->fields) {
      os << pad << f.name << ": " << typeSyntaxText(f.type) << "\n";
    }
    return;
  }
  case ast::DeclKind::Enum: {
    const auto *e = static_cast<const ast::EnumDecl *>(d);
    os << "enum " << e->name << "\n";
    for (const ast::VariantDecl &v : e->variants) {
      os << pad << v.name;
      for (const ast::TypeSyntax &t : v.payload) {
        os << "(" << typeSyntaxText(t) << ")";
      }
      os << "\n";
    }
    return;
  }
  case ast::DeclKind::Import: {
    const auto *i = static_cast<const ast::ImportDecl *>(d);
    os << "let " << i->name << " = import(\"" << i->path << "\")\n";
    return;
  }
  case ast::DeclKind::StaticLet: {
    const auto *s = static_cast<const ast::StaticLetDecl *>(d);
    os << "let " << s->name;
    if (s->hasType) {
      os << ": " << typeSyntaxText(s->type);
    }
    os << "\n";
    dumpExpr(s->init, os, 2);
    return;
  }
  }
  os << pad << "<decl?>\n";
}

void dumpAst(const ast::Module &mod, std::ostream &os) {
  os << "module " << mod.name << "\n";
  for (const ast::Decl *d : mod.decls) {
    dumpDecl(d, os);
  }
}

void dumpTokens(const std::vector<Token> &toks, const SourceMgr &src,
                std::ostream &os) {
  for (const Token &t : toks) {
    os << tokName(t.kind) << " '" << t.text << "'";
    if (t.kind == Tok::IntLit) {
      os << " = " << t.intValue;
    }
    if (t.span.valid()) {
      const Pos p = src.position(t.span.file, t.span.begin);
      os << " @" << p.line << ":" << p.col;
    }
    os << "\n";
  }
}

// verify 的诊断来自 seed 的号段（2xxx），不能直接当成 lain0 的码，所以套在
// D::VerifyFailed 上，原始码与位置写进消息文本。
std::string verifyMessage(const L1Diagnostic &d) {
  std::string m = "LAINIR 验证失败 (code ";
  m += std::to_string(d.code);
  m += ")";
  if (d.line != 0 || d.column != 0) {
    m += " 于 ";
    m += std::to_string(d.line);
    m += ":";
    m += std::to_string(d.column);
  }
  const std::string_view reason(d.message);
  if (!reason.empty()) {
    m += ": ";
    m += reason;
  }
  return m;
}

} // namespace

CompileResult compileFile(const std::string &path, const CompileOptions &opts,
                          SourceMgr &src, DiagSink &diags) {
  CompileResult r{};

  // 模块加载与两轮分析都归 Program（docs/plan-modules.md §3）：源根 = 入口
  // 文件所在目录，整个 import 依赖图一次加载完，同键只解析一次。
  Program prog;
  if (!prog.load(path, src, diags)) {
    return r;
  }
  const ModuleUnit *entry = prog.root();
  if (entry == nullptr || entry->ast == nullptr) {
    return r;
  }
  r.fileId = entry->fileId;
  const std::string_view text = entry->text;

  if (opts.dumpTokens) {
    dumpTokens(entry->toks, src, std::cout);
  }
  if (diags.hasErrors()) {
    return r;
  }
  // 模块名由模块键决定：parser 只决定形状，不认文件名。
  const std::string moduleName = entry->ctx.key.empty()
                                     ? moduleNameOf(path, opts)
                                     : entry->ctx.key;
  entry->ast->name = std::string_view(moduleName);
  if (opts.dumpAst) {
    dumpAst(*entry->ast, std::cout);
  }
  if (diags.hasErrors()) {
    return r;
  }

  TypeStore types;
  types.initBuiltins();
  if (!prog.analyze(types, diags)) {
    return r;
  }
  if (diags.hasErrors()) {
    return r;
  }

  L1Builder *builder = lainir_builder_new();
  if (builder == nullptr) {
    diags.error(Span{}, D::LowerUnsupported, std::string("LAINIR builder 创建失败"));
    return r;
  }

  LowerOptions lopts{};
  lopts.moduleName = moduleName.c_str();
  lopts.program = &prog;
  Lowerer lower(builder, types, src, diags, lopts);
  const L1Module *ir = lower.run(prog);
  if (ir == nullptr || diags.hasErrors()) {
    lainir_builder_free(builder);
    return r;
  }

  // 闸门：不过 verify 就绝不打印，产物保持空。
  L1Diagnostic vdiag{};
  vdiag.code = 0;
  vdiag.line = 0;
  vdiag.column = 0;
  vdiag.message[0] = '\0';
  const int vrc = lainir_verify(ir, &vdiag);
  r.verifyCode = vdiag.code;
  r.verifyMessage = std::string(vdiag.message);
  if (vrc != 0) {
    Span sp{};
    sp.file = r.fileId;
    sp.begin = offsetOfLineCol(text, vdiag.line, vdiag.column);
    sp.end = sp.begin;
    diags.error(sp, D::VerifyFailed, verifyMessage(vdiag));
    lainir_builder_free(builder);
    return r;
  }

  char *printed = lainir_print_to_string(ir);
  lainir_builder_free(builder);
  if (printed == nullptr) {
    diags.error(Span{}, D::PrintFailed, std::string("LAINIR 文本打印失败"));
    return r;
  }
  r.lainir.assign(printed);
  std::free(printed);

  // 空产物 + 成功是明确禁止的状态：打成空串等同于什么都没做出来。
  if (r.lainir.empty()) {
    diags.error(Span{}, D::PrintFailed, std::string("LAINIR 文本为空"));
    return r;
  }
  if (diags.hasErrors()) {
    r.lainir.clear();
    return r;
  }

  r.ok = true;
  return r;
}

// §3.7：按清单编一份模块。清单每行 `<相对路径> [<根名> …]`，路径相对清单所在
// 目录、省略 `.lain`；源根就是清单所在目录。其余行为与 compileFile 一致 ——
// verify 不过不产出文本、空产物算失败，一个闸门都不少。
CompileResult compileUnit(const std::string &manifestPath,
                          const CompileOptions &opts, SourceMgr &src,
                          DiagSink &diags) {
  CompileResult r{};

  Program prog;
  if (!prog.loadUnit(manifestPath, src, diags)) {
    return r;
  }
  const ModuleUnit *entry = prog.root();
  if (entry == nullptr || entry->ast == nullptr) {
    return r;
  }
  r.fileId = entry->fileId;

  if (opts.dumpTokens) {
    dumpTokens(entry->toks, src, std::cout);
  }
  if (diags.hasErrors()) {
    return r;
  }
  // 模块名取根模块键（§3.4）。print.c 不打印模块名，但 IR 里不能是空串。
  const std::string moduleName =
      entry->ctx.key.empty() ? moduleNameOf(manifestPath, opts)
                             : entry->ctx.key;
  if (opts.dumpAst) {
    dumpAst(*entry->ast, std::cout);
  }
  if (diags.hasErrors()) {
    return r;
  }

  TypeStore types;
  types.initBuiltins();
  if (!prog.analyze(types, diags)) {
    return r;
  }
  if (diags.hasErrors()) {
    return r;
  }

  L1Builder *builder = lainir_builder_new();
  if (builder == nullptr) {
    diags.error(Span{}, D::LowerUnsupported, std::string("LAINIR builder 创建失败"));
    return r;
  }

  LowerOptions lopts{};
  lopts.moduleName = moduleName.c_str();
  lopts.program = &prog;
  Lowerer lower(builder, types, src, diags, lopts);
  const L1Module *ir = lower.run(prog);
  if (ir == nullptr || diags.hasErrors()) {
    lainir_builder_free(builder);
    return r;
  }

  L1Diagnostic vdiag{};
  vdiag.code = 0;
  vdiag.line = 0;
  vdiag.column = 0;
  vdiag.message[0] = '\0';
  const int vrc = lainir_verify(ir, &vdiag);
  r.verifyCode = vdiag.code;
  r.verifyMessage = std::string(vdiag.message);
  if (vrc != 0) {
    Span sp{};
    sp.file = r.fileId;
    sp.begin = offsetOfLineCol(entry->text, vdiag.line, vdiag.column);
    sp.end = sp.begin;
    diags.error(sp, D::VerifyFailed, verifyMessage(vdiag));
    lainir_builder_free(builder);
    return r;
  }

  char *printed = lainir_print_to_string(ir);
  lainir_builder_free(builder);
  if (printed == nullptr) {
    diags.error(Span{}, D::PrintFailed, std::string("LAINIR 文本打印失败"));
    return r;
  }
  r.lainir.assign(printed);
  std::free(printed);
  if (r.lainir.empty()) {
    diags.error(Span{}, D::PrintFailed, std::string("LAINIR 文本为空"));
    return r;
  }
  if (diags.hasErrors()) {
    r.lainir.clear();
    return r;
  }

  r.ok = true;
  return r;
}

bool roundTripCheck(std::string_view text, int &outCode, std::string &outMsg) {
  outCode = 0;
  outMsg.clear();
  if (text.empty()) {
    outCode = dcode(D::RoundTripFailed);
    outMsg = "canonical 文本为空";
    return false;
  }

  L1Builder *builder = lainir_builder_new();
  if (builder == nullptr) {
    outCode = dcode(D::RoundTripFailed);
    outMsg = "LAINIR builder 创建失败";
    return false;
  }

  L1Diagnostic diag{};
  diag.code = 0;
  diag.line = 0;
  diag.column = 0;
  diag.message[0] = '\0';

  // lainir_parse 只吃 NUL 结尾的 C 串；string_view 不保证，所以拷一份。
  const std::string owned(text);
  const L1Module *parsed = lainir_parse(builder, owned.c_str(), &diag);
  if (parsed == nullptr) {
    outCode = diag.code != 0 ? diag.code : LAINPARSE_ERR_SYNTAX;
    outMsg = "canonical 文本重新解析失败";
    if (diag.message[0] != '\0') {
      outMsg += ": ";
      outMsg += diag.message;
    }
    outMsg += " @line " + std::to_string(diag.line) + " col " +
              std::to_string(diag.column);
    lainir_builder_free(builder);
    return false;
  }

  char *again = lainir_print_to_string(parsed);
  lainir_builder_free(builder);
  if (again == nullptr) {
    outCode = dcode(D::PrintFailed);
    outMsg = "canonical 文本二次打印失败";
    return false;
  }
  const bool same = (owned == again);
  if (!same) {
    outCode = dcode(D::RoundTripFailed);
    outMsg = "固定点不成立：print(parse(text)) != text";
  }
  std::free(again);
  return same;
}

} // namespace ln
