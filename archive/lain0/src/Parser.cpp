#include "lain0/Parser.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ln {
namespace {

Span joinSpans(Span first, Span last) {
  if (!first.valid()) {
    return last;
  }
  if (!last.valid() || first.file != last.file) {
    return first;
  }
  return Span{first.file, std::min(first.begin, last.begin),
              std::max(first.end, last.end)};
}

bool isQuoted(const Token &token) {
  // StrLit 的 text 在词法阶段就已经去掉两侧引号了，所以不能按首尾引号判断。
  if (token.kind == Tok::StrLit) return true;
  return token.text.size() >= 2 && token.text.front() == '"' &&
         token.text.back() == '"';
}

std::string_view unquote(std::string_view text) {
  return text.substr(1, text.size() - 2);
}

bool binaryOp(Tok kind, ast::BinOp &op) {
  switch (kind) {
  case Tok::Plus:
    op = ast::BinOp::Add;
    return true;
  case Tok::Minus:
    op = ast::BinOp::Sub;
    return true;
  case Tok::Star:
    op = ast::BinOp::Mul;
    return true;
  case Tok::Slash:
    op = ast::BinOp::Div;
    return true;
  case Tok::Percent:
    op = ast::BinOp::Rem;
    return true;
  case Tok::Shl:
    op = ast::BinOp::Shl;
    return true;
  case Tok::Shr:
    op = ast::BinOp::Shr;
    return true;
  case Tok::Amp:
    op = ast::BinOp::BitAnd;
    return true;
  case Tok::Pipe:
    op = ast::BinOp::BitOr;
    return true;
  case Tok::Caret:
    op = ast::BinOp::BitXor;
    return true;
  case Tok::EqEq:
    op = ast::BinOp::Eq;
    return true;
  case Tok::Ne:
    op = ast::BinOp::Ne;
    return true;
  case Tok::Lt:
    op = ast::BinOp::Lt;
    return true;
  case Tok::Le:
    op = ast::BinOp::Le;
    return true;
  case Tok::Gt:
    op = ast::BinOp::Gt;
    return true;
  case Tok::Ge:
    op = ast::BinOp::Ge;
    return true;
  case Tok::KwAnd:
    op = ast::BinOp::LogicalAnd;
    return true;
  case Tok::KwOr:
    op = ast::BinOp::LogicalOr;
    return true;
  default:
    return false;
  }
}

bool isDeclarationStart(Tok kind) {
  switch (kind) {
  case Tok::KwFunc:
  case Tok::KwExtern:
  case Tok::KwStruct:
  case Tok::KwEnum:
  case Tok::KwLet:
    return true;
  default:
    return false;
  }
}

bool isStatementKeyword(Tok kind) {
  switch (kind) {
  case Tok::KwLet:
  case Tok::KwReturn:
  case Tok::KwIf:
  case Tok::KwBreak:
  case Tok::KwContinue:
    return true;
  default:
    return false;
  }
}

struct TypeSyntaxStorage final : ast::Node {
  explicit TypeSyntaxStorage(ast::TypeSyntax syntax)
      : value(std::move(syntax)) {}
  ast::TypeSyntax value{};
};

class Parser {
public:
  Parser(const std::vector<Token> &tokens, size_t start, ast::AstArena &arena,
         DiagSink &diags)
      : tokens_(tokens), pos_(start), arena_(arena), diags_(diags) {}

  size_t position() const { return pos_; }

  ast::Module *parseModule() {
    auto *module = arena_.make<ast::Module>();
    while (!atEnd()) {
      const size_t start = pos_;
      // 声明属性（`@root`）写在顶层声明前，可写一行或多行。
      bool isRoot = false;
      Span rootSpan{};
      if (!parseDeclAttributes(isRoot, rootSpan)) {
        synchronizeDeclaration(start);
        continue;
      }
      ast::Decl *decl = parseDeclaration();
      if (decl != nullptr) {
        decl->isRoot = isRoot;
        decl->rootSpan = rootSpan;
        module->decls.push_back(decl);
        continue;
      }
      synchronizeDeclaration(start);
    }
    return module;
  }

  ast::Expr *parseExpression() { return parseExpression(1, true); }

private:
  const Token &current() const {
    static const Token eof{};
    if (pos_ >= tokens_.size()) {
      return eof;
    }
    return tokens_[pos_];
  }

  const Token &peek(size_t distance) const {
    static const Token eof{};
    if (distance > tokens_.size() || pos_ >= tokens_.size() - distance) {
      return eof;
    }
    return tokens_[pos_ + distance];
  }

  const Token &previous() const {
    if (pos_ == 0 || tokens_.empty()) {
      return current();
    }
    return tokens_[std::min(pos_ - 1, tokens_.size() - 1)];
  }

  bool atEnd() const {
    return pos_ >= tokens_.size() || current().kind == Tok::EndOfFile;
  }

  const Token &advance() {
    if (!atEnd()) {
      ++pos_;
    }
    return previous();
  }

  bool check(Tok kind) const { return !atEnd() && current().kind == kind; }

  bool match(Tok kind) {
    if (!check(kind)) {
      return false;
    }
    advance();
    return true;
  }

  void expected(std::string_view description) {
    std::string message = "应为 ";
    message.append(description);
    message.append("，实际为 ");
    message.append(tokName(current().kind));
    diags_.error(current().span, D::ParseExpected, std::move(message));
  }

  bool expect(Tok kind, std::string_view description) {
    if (match(kind)) {
      return true;
    }
    expected(description);
    return false;
  }

  bool takeIdent(Token &out, std::string_view description) {
    if (!check(Tok::Ident)) {
      expected(description);
      return false;
    }
    out = advance();
    return true;
  }

  ast::TypeSyntax *storeType(ast::TypeSyntax syntax) {
    return &arena_.make<TypeSyntaxStorage>(std::move(syntax))->value;
  }

  bool parseType(ast::TypeSyntax &type) {
    type = {};
    const Span start = current().span;
    const bool isRo = match(Tok::KwRo);

    // 引用 / 切片类型：ref T / mut ref T / slice T。
    if (match(Tok::KwRef)) {
      ast::TypeSyntax inner;
      if (!parseType(inner)) return false;
      type.span = joinSpans(start, previous().span);
      type.ptrKind = ast::TypeSyntax::PtrKind::Ref;
      type.inner = storeType(std::move(inner));
      return true;
    }
    if (match(Tok::KwMut)) {
      if (!expect(Tok::KwRef, "'ref'")) return false;
      ast::TypeSyntax inner;
      if (!parseType(inner)) return false;
      type.span = joinSpans(start, previous().span);
      type.ptrKind = ast::TypeSyntax::PtrKind::MutRef;
      type.inner = storeType(std::move(inner));
      return true;
    }
    if (match(Tok::KwSlice)) {
      ast::TypeSyntax inner;
      if (!parseType(inner)) return false;
      type.span = joinSpans(start, previous().span);
      type.ptrKind = ast::TypeSyntax::PtrKind::Slice;
      type.inner = storeType(std::move(inner));
      return true;
    }

    if (match(Tok::LBracket)) {
      ast::TypeSyntax element;
      if (!parseType(element)) {
        return false;
      }
      if (!expect(Tok::RBracket, "']'")) {
        return false;
      }
      type.span = joinSpans(start, previous().span);
      type.name = element.name;
      type.hasQualifier = element.hasQualifier;
      type.qualifier = element.qualifier;
      type.isArray = true;
      type.isRo = isRo;
      type.element = storeType(std::move(element));
      return true;
    }

    if (isRo) {
      expected("'['");
      return false;
    }

    Token first;
    if (!takeIdent(first, "类型名")) {
      return false;
    }
    type.span = first.span;
    type.name = first.text;
    if (match(Tok::Dot)) {
      Token second;
      if (!takeIdent(second, "限定类型名")) {
        return false;
      }
      type.span = joinSpans(first.span, second.span);
      type.hasQualifier = true;
      type.qualifier = first.text;
      type.name = second.text;
    }
    return true;
  }

  bool parseParameters(std::vector<ast::ParamDecl> &params) {
    if (!expect(Tok::LParen, "'('")) {
      return false;
    }
    if (match(Tok::RParen)) {
      return true;
    }

    while (!atEnd()) {
      Token name;
      if (!takeIdent(name, "参数名")) {
        return false;
      }
      if (!expect(Tok::Colon, "':'")) {
        return false;
      }
      ast::TypeSyntax type;
      if (!parseType(type)) {
        return false;
      }
      params.push_back(
          ast::ParamDecl{joinSpans(name.span, type.span), name.text, type});

      if (match(Tok::RParen)) {
        return true;
      }
      if (!expect(Tok::Comma, "','")) {
        return false;
      }
      if (match(Tok::RParen)) {
        return true;
      }
    }
    expected("')'");
    return false;
  }

  // 顶层声明前的属性行（docs/proposal-modules-v2.md §四）：`@root`，一行或多行。
  // 只支持 `root` 一个属性；不认识的属性名**报错**，不当普通名字吞掉。
  // 顶层之外（函数体、结构体体、表达式里）的 `@` 由 parsePrefix 报错。
  // 返回 false 表示报错，调用方按恢复处理。
  bool parseDeclAttributes(bool &isRoot, Span &rootSpan) {
    while (check(Tok::At)) {
      const Token at = advance();
      if (!check(Tok::Ident)) {
        diags_.error(at.span, D::ParseUnexpectedToken,
                     "声明属性名应为标识符，实际为 " +
                         std::string(tokName(current().kind)));
        return false;
      }
      const Token name = advance();
      if (name.text != "root") {
        diags_.error(joinSpans(at.span, name.span), D::ParseUnexpectedToken,
                     "不认识的声明属性 @" + std::string(name.text) +
                         "（只支持 @root）");
        return false;
      }
      if (!isRoot) {
        isRoot = true;
        rootSpan = joinSpans(at.span, name.span);
      }
    }
    return true;
  }

  ast::Decl *parseDeclaration() {
    switch (current().kind) {
    case Tok::KwFunc:
      return parseFuncDeclaration();
    case Tok::KwExtern:
      return parseExternDeclaration();
    case Tok::KwStruct:
      return parseStructDeclaration();
    case Tok::KwEnum:
      return parseEnumDeclaration();
    case Tok::KwLet:
      return parseTopLetDeclaration();
    default:
      diags_.error(current().span, D::ParseUnexpectedToken,
                   "顶层只能出现声明");
      return nullptr;
    }
  }

  ast::Decl *parseFuncDeclaration() {
    const Token start = advance();
    Token name;
    if (!takeIdent(name, "函数名")) {
      return nullptr;
    }

    std::vector<ast::ParamDecl> params;
    if (!parseParameters(params)) {
      return nullptr;
    }
    if (!expect(Tok::Arrow, "'->'")) {
      return nullptr;
    }
    ast::TypeSyntax ret;
    if (!parseType(ret)) {
      return nullptr;
    }

    std::vector<ast::Stmt *> body;
    if (!parseBlock(body)) {
      return nullptr;
    }

    auto *decl = arena_.make<ast::FuncDecl>(
        joinSpans(start.span, previous().span));
    decl->name = name.text;
    decl->params = std::move(params);
    decl->ret = ret;
    decl->hasRet = true;
    decl->body = std::move(body);
    return decl;
  }

  ast::Decl *parseExternDeclaration() {
    const Token start = advance();
    Token name;
    if (!takeIdent(name, "外部过程名")) {
      return nullptr;
    }

    std::vector<ast::ParamDecl> params;
    if (!parseParameters(params)) {
      return nullptr;
    }
    if (!expect(Tok::Arrow, "'->'")) {
      return nullptr;
    }
    ast::TypeSyntax ret;
    if (!parseType(ret)) {
      return nullptr;
    }
    if (!expect(Tok::Assign, "'='")) {
      return nullptr;
    }

    Token linkName;
    if (!takeIdent(linkName, "链接名")) {
      return nullptr;
    }
    if (!expect(Tok::Semi, "';'")) {
      return nullptr;
    }

    auto *decl = arena_.make<ast::ExternDecl>(
        joinSpans(start.span, previous().span));
    decl->name = name.text;
    decl->params = std::move(params);
    decl->ret = ret;
    decl->hasRet = true;
    decl->linkName = linkName.text;
    return decl;
  }

  ast::Decl *parseStructDeclaration() {
    const Token start = advance();
    Token name;
    if (!takeIdent(name, "结构名") || !expect(Tok::LBrace, "'{'") ) {
      return nullptr;
    }

    std::vector<ast::FieldDecl> fields;
    while (!atEnd() && !check(Tok::RBrace)) {
      if (match(Tok::Comma)) {
        diags_.error(previous().span, D::ParseUnexpectedToken,
                     "结构字段使用空格分隔");
        continue;
      }
      Token fieldName;
      if (!takeIdent(fieldName, "字段名")) {
        return nullptr;
      }
      if (!expect(Tok::Colon, "':'")) {
        return nullptr;
      }
      ast::TypeSyntax fieldType;
      if (!parseType(fieldType)) {
        return nullptr;
      }
      fields.push_back(ast::FieldDecl{
          joinSpans(fieldName.span, fieldType.span), fieldName.text, fieldType});
    }
    if (!expect(Tok::RBrace, "'}'")) {
      return nullptr;
    }
    match(Tok::Semi);

    auto *decl = arena_.make<ast::StructDecl>(
        joinSpans(start.span, previous().span));
    decl->name = name.text;
    decl->fields = std::move(fields);
    return decl;
  }

  ast::Decl *parseEnumDeclaration() {
    const Token start = advance();
    Token name;
    if (!takeIdent(name, "枚举名") || !expect(Tok::LBrace, "'{'") ) {
      return nullptr;
    }

    std::vector<ast::VariantDecl> variants;
    while (!atEnd() && !check(Tok::RBrace)) {
      if (match(Tok::Comma)) {
        diags_.error(previous().span, D::ParseUnexpectedToken,
                     "枚举变体使用空格分隔");
        continue;
      }
      Token variantName;
      if (!takeIdent(variantName, "变体名")) {
        return nullptr;
      }
      ast::VariantDecl variant;
      variant.span = variantName.span;
      variant.name = variantName.text;
      if (match(Tok::LParen)) {
        ast::TypeSyntax payload;
        if (!parseType(payload) || !expect(Tok::RParen, "')'")) {
          return nullptr;
        }
        variant.span = joinSpans(variantName.span, previous().span);
        variant.payload.push_back(payload);
      }
      variants.push_back(std::move(variant));
    }
    if (!expect(Tok::RBrace, "'}'")) {
      return nullptr;
    }
    match(Tok::Semi);

    auto *decl = arena_.make<ast::EnumDecl>(
        joinSpans(start.span, previous().span));
    decl->name = name.text;
    decl->variants = std::move(variants);
    return decl;
  }

  ast::Decl *parseTopLetDeclaration() {
    const Token start = advance();
    Token name;
    if (!takeIdent(name, "绑定名")) {
      return nullptr;
    }

    ast::TypeSyntax type;
    bool hasType = false;
    if (match(Tok::Colon)) {
      if (!parseType(type)) {
        return nullptr;
      }
      hasType = true;
    }
    if (!expect(Tok::Assign, "'='")) {
      return nullptr;
    }

    if (!hasType && match(Tok::KwImport)) {
      if (!expect(Tok::LParen, "'('")) {
        return nullptr;
      }
      if (!isQuoted(current())) {
        expected("模块路径字符串");
        return nullptr;
      }
      const Token path = advance();
      if (!expect(Tok::RParen, "')'") || !expect(Tok::Semi, "';'")) {
        return nullptr;
      }
      auto *decl = arena_.make<ast::ImportDecl>(
          joinSpans(start.span, previous().span));
      decl->name = name.text;
      // StrLit 已无引号可去；别的 token 形态才需要 unquote。
      decl->path =
          path.kind == Tok::StrLit ? path.text : unquote(path.text);
      return decl;
    }

    ast::Expr *init = parseExpression();
    if (init == nullptr || !expect(Tok::Semi, "';'")) {
      return nullptr;
    }
    auto *decl = arena_.make<ast::StaticLetDecl>(
        joinSpans(start.span, previous().span));
    decl->name = name.text;
    decl->type = type;
    decl->hasType = hasType;
    decl->isRo = hasType && type.isRo;
    decl->init = init;
    return decl;
  }

  bool parseBlock(std::vector<ast::Stmt *> &body) {
    if (!expect(Tok::LBrace, "'{'") ) {
      return false;
    }
    while (!atEnd() && !check(Tok::RBrace)) {
      const size_t start = pos_;
      ast::Stmt *stmt = parseStatement();
      if (stmt != nullptr) {
        body.push_back(stmt);
      } else {
        synchronizeStatement(start);
      }
    }
    return expect(Tok::RBrace, "'}'");
  }

  ast::Stmt *parseStatement() {
    switch (current().kind) {
    case Tok::KwLet:
      return parseLetStatement();
    case Tok::KwReturn:
      return parseReturnStatement();
    case Tok::KwIf:
      return parseIfStatement();
    case Tok::KwLoop:
      return parseLoopStatement();
    case Tok::KwBreak:
      return parseJumpStatement(true);
    case Tok::KwContinue:
      return parseJumpStatement(false);
    default:
      return parseExpressionOrAssignmentStatement();
    }
  }

  ast::Stmt *parseLetStatement() {
    const Token start = advance();
    Token name;
    if (!takeIdent(name, "绑定名")) {
      return nullptr;
    }

    ast::TypeSyntax type;
    bool hasType = false;
    if (match(Tok::Colon)) {
      if (!parseType(type)) {
        return nullptr;
      }
      hasType = true;
    }
    if (!expect(Tok::Assign, "'='")) {
      return nullptr;
    }
    ast::Expr *init = parseExpression();
    if (init == nullptr || !expect(Tok::Semi, "';'")) {
      return nullptr;
    }

    auto *stmt = arena_.make<ast::LetStmt>(
        joinSpans(start.span, previous().span), name.text);
    stmt->type = type;
    stmt->hasType = hasType;
    stmt->isRo = hasType && type.isRo;
    stmt->init = init;
    return stmt;
  }

  ast::Stmt *parseReturnStatement() {
    const Token start = advance();
    ast::Expr *value = nullptr;
    if (!check(Tok::Semi)) {
      value = parseExpression();
      if (value == nullptr) {
        return nullptr;
      }
    }
    if (!expect(Tok::Semi, "';'")) {
      return nullptr;
    }
    auto *stmt = arena_.make<ast::ReturnStmt>(
        joinSpans(start.span, previous().span));
    stmt->value = value;
    return stmt;
  }

  ast::Stmt *parseIfStatement() {
    const Token start = advance();
    ast::Expr *condition = parseExpression(1, false);
    if (condition == nullptr) {
      return nullptr;
    }

    std::vector<ast::Stmt *> thenBody;
    if (!parseBlock(thenBody)) {
      return nullptr;
    }
    std::vector<ast::Stmt *> elseBody;
    bool hasElse = false;
    if (match(Tok::KwElse)) {
      hasElse = true;
      if (!parseBlock(elseBody)) {
        return nullptr;
      }
    }

    auto *stmt = arena_.make<ast::IfStmt>(
        joinSpans(start.span, previous().span));
    stmt->cond = condition;
    stmt->thenBody = std::move(thenBody);
    stmt->elseBody = std::move(elseBody);
    stmt->hasElse = hasElse;
    return stmt;
  }

  ast::Stmt *parseLoopStatement() {
    const Token start = advance();
    Token label;
    bool hasLabel = false;
    if (check(Tok::Ident)) {
      label = advance();
      hasLabel = true;
    }
    if (!expect(Tok::LParen, "'('")) {
      return nullptr;
    }

    std::vector<ast::LoopParam> params;
    if (!match(Tok::RParen)) {
      while (!atEnd()) {
        Token name;
        if (!takeIdent(name, "循环参数名") ||
            !expect(Tok::Colon, "':'")) {
          return nullptr;
        }
        ast::TypeSyntax type;
        if (!parseType(type)) {
          return nullptr;
        }
        ast::Expr *init = nullptr;
        if (match(Tok::Assign)) {
          init = parseExpression();
          if (init == nullptr) {
            return nullptr;
          }
        }
        const Span end = init != nullptr ? init->span : type.span;
        params.push_back(
            ast::LoopParam{joinSpans(name.span, end), name.text, type, init});
        if (match(Tok::RParen)) {
          break;
        }
        if (!expect(Tok::Comma, "','")) {
          return nullptr;
        }
        if (match(Tok::RParen)) {
          break;
        }
      }
    }

    ast::Expr *condition = nullptr;
    if (match(Tok::KwWhile)) {
      condition = parseExpression(1, false);
      if (condition == nullptr) {
        return nullptr;
      }
    }
    std::vector<ast::Stmt *> body;
    if (!parseBlock(body)) {
      return nullptr;
    }
    match(Tok::Semi);

    auto *stmt = arena_.make<ast::LoopStmt>(
        joinSpans(start.span, previous().span));
    stmt->label = hasLabel ? label.text : std::string_view{};
    stmt->hasLabel = hasLabel;
    stmt->params = std::move(params);
    stmt->cond = condition;
    stmt->body = std::move(body);
    return stmt;
  }

  ast::Expr *parseForExpr() {
    const Token start = advance();
    Token variable;
    if (!takeIdent(variable, "循环变量名")) {
      return nullptr;
    }

    ast::TypeSyntax variableType;
    bool hasVariableType = false;
    if (match(Tok::Colon)) {
      if (!parseType(variableType)) {
        return nullptr;
      }
      hasVariableType = true;
    }
    if (!expect(Tok::KwIn, "'in'")) {
      return nullptr;
    }

    ast::Expr *from = parseExpression(1, false);
    if (from == nullptr || !expect(Tok::Dot, "范围运算符 '..'") ||
        !expect(Tok::Dot, "范围运算符 '..'")) {
      return nullptr;
    }
    ast::Expr *to = parseExpression(1, false);
    if (to == nullptr) {
      return nullptr;
    }

    bool hasAcc = false;
    std::string_view acc;
    ast::TypeSyntax accType;
    ast::Expr *accInit = nullptr;
    if (check(Tok::Ident) && current().text == "acc") {
      const Token accTok = current();
      advance();
      hasAcc = true;
      acc = accTok.text;
      if (match(Tok::Colon) && !parseType(accType)) {
        return nullptr;
      }
      if (!expect(Tok::Assign, "'='")) {
        return nullptr;
      }
      accInit = parseExpression(1, false);
      if (accInit == nullptr) {
        return nullptr;
      }
    }

    std::vector<ast::Stmt *> body;
    if (!parseBlock(body)) {
      return nullptr;
    }

    auto *fe = arena_.make<ast::ForExpr>(joinSpans(start.span, previous().span));
    fe->var = variable.text;
    fe->varType = variableType;
    fe->hasVarType = hasVariableType;
    fe->from = from;
    fe->to = to;
    fe->hasAcc = hasAcc;
    fe->acc = acc;
    fe->accType = accType;
    fe->accInit = accInit;
    fe->body = std::move(body);
    return fe;
  }

  ast::Expr *parseLoopExpr() {
    const Token start = advance();
    Token label;
    bool hasLabel = false;
    if (check(Tok::Ident)) {
      label = advance();
      hasLabel = true;
    }
    if (!expect(Tok::LParen, "'('")) return nullptr;
    std::vector<ast::LoopParam> params;
    if (!match(Tok::RParen)) {
      while (!atEnd()) {
        Token name;
        if (!takeIdent(name, "循环参数名") || !expect(Tok::Colon, "':'"))
          return nullptr;
        ast::TypeSyntax type;
        if (!parseType(type)) return nullptr;
        ast::Expr *init = nullptr;
        if (match(Tok::Assign)) {
          init = parseExpression();
          if (init == nullptr) return nullptr;
        }
        const Span end = init != nullptr ? init->span : type.span;
        params.push_back(
            ast::LoopParam{joinSpans(name.span, end), name.text, type, init});
        if (match(Tok::RParen)) break;
        if (!expect(Tok::Comma, "','")) return nullptr;
        if (match(Tok::RParen)) break;
      }
    }
    ast::Expr *condition = nullptr;
    if (match(Tok::KwWhile)) {
      condition = parseExpression(1, false);
      if (condition == nullptr) return nullptr;
    }
    std::vector<ast::Stmt *> body;
    if (!parseBlock(body)) return nullptr;
    auto *stmt = arena_.make<ast::LoopExpr>(joinSpans(start.span, previous().span));
    stmt->label = hasLabel ? label.text : std::string_view{};
    stmt->hasLabel = hasLabel;
    stmt->params = std::move(params);
    stmt->cond = condition;
    stmt->body = std::move(body);
    return stmt;
  }

  ast::Stmt *parseJumpStatement(bool isBreak) {
    const Token start = advance();
    Token label;
    if (!takeIdent(label, "循环标签") || !expect(Tok::LParen, "'('")) {
      return nullptr;
    }

    std::vector<ast::Expr *> args;
    if (!match(Tok::RParen)) {
      while (!atEnd()) {
        ast::Expr *arg = parseExpression();
        if (arg == nullptr) {
          return nullptr;
        }
        args.push_back(arg);
        if (match(Tok::RParen)) {
          break;
        }
        if (!expect(Tok::Comma, "','")) {
          return nullptr;
        }
        if (match(Tok::RParen)) {
          break;
        }
      }
    }
    if (!expect(Tok::Semi, "';'")) {
      return nullptr;
    }

    auto *stmt = arena_.make<ast::JumpStmt>(
        joinSpans(start.span, previous().span), isBreak);
    stmt->label = label.text;
    stmt->args = std::move(args);
    return stmt;
  }

  ast::Stmt *parseExpressionOrAssignmentStatement() {
    const Span start = current().span;
    ast::Expr *expression = parseExpression();
    if (expression == nullptr) {
      return nullptr;
    }

    if (match(Tok::Assign)) {
      ast::Expr *value = parseExpression();
      if (value == nullptr || !expect(Tok::Semi, "';'")) {
        return nullptr;
      }
      return arena_.make<ast::AssignStmt>(
          joinSpans(start, previous().span), expression, value);
    }

    if (!expect(Tok::Semi, "';'")) {
      return nullptr;
    }
    return arena_.make<ast::ExprStmt>(joinSpans(start, previous().span),
                                      expression);
  }

  ast::Expr *parseExpression(int minPrecedence, bool allowConstruct) {
    ast::Expr *left = parsePrefix();
    if (left == nullptr) {
      return nullptr;
    }
    left = parsePostfix(left, allowConstruct);
    if (left == nullptr) {
      return nullptr;
    }

    while (!atEnd()) {
      const int precedence = binaryPrecedence(current().kind);
      ast::BinOp op;
      if (precedence < minPrecedence || precedence == 0 ||
          !binaryOp(current().kind, op)) {
        break;
      }
      advance();
      const int rightPrecedence =
          isRightAssociative(previous().kind) ? precedence : precedence + 1;
      ast::Expr *right = parseExpression(rightPrecedence, allowConstruct);
      if (right == nullptr) {
        return nullptr;
      }
      left = arena_.make<ast::BinaryExpr>(joinSpans(left->span, right->span), op,
                                          left, right);
    }
    return left;
  }

  ast::Expr *parsePrefix() {
    if (isQuoted(current())) {
      const Token token = advance();
      return arena_.make<ast::StrLitExpr>(
          token.span, token.kind == Tok::StrLit ? token.text : unquote(token.text));
    }

    switch (current().kind) {
    case Tok::IntLit: {
      const Token token = advance();
      return arena_.make<ast::IntLitExpr>(token.span, token.intValue);
    }
    case Tok::KwTrue:
    case Tok::KwFalse: {
      const Token token = advance();
      return arena_.make<ast::BoolLitExpr>(token.span,
                                           token.kind == Tok::KwTrue);
    }
    case Tok::Ident: {
      const Token token = advance();
      return arena_.make<ast::NameExpr>(token.span, token.text);
    }
    case Tok::Minus: {
      const Token minus = advance();
      if (!check(Tok::IntLit)) {
        expected("负号后的整数字面量");
        return nullptr;
      }
      const Token literal = advance();
      auto *operand =
          arena_.make<ast::IntLitExpr>(literal.span, literal.intValue);
      return arena_.make<ast::UnaryExpr>(
          joinSpans(minus.span, literal.span), ast::UnOp::Neg, operand);
    }
    case Tok::LParen: {
      const Token open = advance();
      ast::Expr *expression = parseExpression(1, true);
      if (expression == nullptr || !expect(Tok::RParen, "')'")) {
        return nullptr;
      }
      expression->span = joinSpans(open.span, previous().span);
      return expression;
    }
    case Tok::LBracket:
      return parseTableExpression();
    case Tok::KwIf:
      return parseValueIfExpression();
    case Tok::StrLit: {
      auto *s = arena_.make<ast::StrLitExpr>(current().span, current().text);
      advance();
      return s;
    }
    case Tok::KwLoop:
      return parseLoopExpr();
    case Tok::KwFor:
      return parseForExpr();
    case Tok::KwImport:
      diags_.error(current().span, D::ParseReserved,
                   "import 只用于顶层模块绑定");
      return nullptr;
    case Tok::At:
      // `@root` 只允许出现在顶层声明前（parseModule 的顶层循环里消费）。
      // 落到这里说明写在函数体 / 表达式等非顶层位置。
      diags_.error(current().span, D::ParseUnexpectedToken,
                   "声明属性 @root 只能写在顶层声明上");
      advance();
      return nullptr;
    default:
      diags_.error(current().span, D::ParseNotAnExpression,
                   "此处需要表达式");
      return nullptr;
    }
  }

  ast::Expr *parsePostfix(ast::Expr *expression, bool allowConstruct) {
    while (!atEnd()) {
      if (match(Tok::LParen)) {
        std::vector<ast::Expr *> args;
        if (!match(Tok::RParen)) {
          while (!atEnd()) {
            ast::Expr *arg = parseExpression();
            if (arg == nullptr) {
              return nullptr;
            }
            args.push_back(arg);
            if (match(Tok::RParen)) {
              break;
            }
            if (!expect(Tok::Comma, "','")) {
              return nullptr;
            }
            if (match(Tok::RParen)) {
              break;
            }
          }
        }
        expression = arena_.make<ast::CallExpr>(
            joinSpans(expression->span, previous().span), expression,
            std::move(args));
        continue;
      }

      if (match(Tok::LBracket)) {
        ast::Expr *index = parseExpression();
        if (index == nullptr || !expect(Tok::RBracket, "']'")) {
          return nullptr;
        }
        expression = arena_.make<ast::IndexExpr>(
            joinSpans(expression->span, previous().span), expression, index);
        continue;
      }

      if (check(Tok::Dot) && peek(1).kind == Tok::Ident) {
        advance();
        const Token member = advance();
        expression = arena_.make<ast::DottedExpr>(
            joinSpans(expression->span, member.span), expression, member.text);
        continue;
      }

      if (match(Tok::KwAs)) {
        ast::TypeSyntax target;
        if (!parseType(target)) {
          return nullptr;
        }
        expression = arena_.make<ast::CastExpr>(
            joinSpans(expression->span, target.span), expression, target);
        continue;
      }

      ast::TypeSyntax target;
      if (allowConstruct && check(Tok::LBrace) &&
          expressionType(expression, target)) {
        expression = parseConstructExpression(expression, std::move(target));
        if (expression == nullptr) {
          return nullptr;
        }
        continue;
      }
      break;
    }
    return expression;
  }

  bool expressionType(ast::Expr *expression, ast::TypeSyntax &type) const {
    type = {};
    if (expression->kind == ast::ExprKind::Name) {
      auto *name = static_cast<ast::NameExpr *>(expression);
      type.span = expression->span;
      type.name = name->name;
      return true;
    }
    if (expression->kind != ast::ExprKind::Dotted) {
      return false;
    }
    auto *dotted = static_cast<ast::DottedExpr *>(expression);
    if (dotted->object == nullptr ||
        dotted->object->kind != ast::ExprKind::Name) {
      return false;
    }
    auto *qualifier = static_cast<ast::NameExpr *>(dotted->object);
    type.span = expression->span;
    type.hasQualifier = true;
    type.qualifier = qualifier->name;
    type.name = dotted->member;
    return true;
  }

  ast::Expr *parseConstructExpression(ast::Expr *base,
                                      ast::TypeSyntax target) {
    advance();
    std::vector<ast::FieldInit> fields;
    while (!atEnd() && !check(Tok::RBrace)) {
      ast::FieldInit field;
      const Span start = current().span;
      if (check(Tok::Ident) && peek(1).kind == Tok::Colon) {
        field.name = advance().text;
        advance();
      }
      field.value = parseExpression();
      if (field.value == nullptr) {
        return nullptr;
      }
      field.span = joinSpans(start, field.value->span);
      fields.push_back(field);

      if (match(Tok::Comma)) {
        continue;
      }
      if (check(Tok::RBrace)) {
        break;
      }
      if (!(check(Tok::Ident) && peek(1).kind == Tok::Colon)) {
        expected("构造字段或 '}'");
        return nullptr;
      }
    }
    if (!expect(Tok::RBrace, "'}'")) {
      return nullptr;
    }
    return arena_.make<ast::ConstructExpr>(
        joinSpans(base->span, previous().span), target, std::move(fields));
  }

  ast::Expr *parseTableExpression() {
    const Token start = advance();
    std::vector<ast::Expr *> elements;
    if (!match(Tok::RBracket)) {
      while (!atEnd()) {
        ast::Expr *element = parseExpression();
        if (element == nullptr) {
          return nullptr;
        }
        elements.push_back(element);
        if (match(Tok::RBracket)) {
          break;
        }
        if (!expect(Tok::Comma, "','")) {
          return nullptr;
        }
        if (match(Tok::RBracket)) {
          break;
        }
      }
    }
    return arena_.make<ast::TableExpr>(
        joinSpans(start.span, previous().span), std::move(elements));
  }

  ast::Expr *parseValueIfExpression() {
    const Token start = advance();
    ast::Expr *condition = parseExpression(1, false);
    if (condition == nullptr) {
      return nullptr;
    }

    std::vector<ast::Stmt *> thenBody;
    if (!parseValueBlock(thenBody)) {
      return nullptr;
    }
    std::vector<ast::Stmt *> elseBody;
    bool hasElse = false;
    if (match(Tok::KwElse)) {
      hasElse = true;
      if (!parseValueBlock(elseBody)) {
        return nullptr;
      }
    }

    auto *expression = arena_.make<ast::ValueIfExpr>(
        joinSpans(start.span, previous().span), condition, std::move(thenBody),
        std::move(elseBody));
    expression->hasElse = hasElse;
    return expression;
  }

  bool parseValueBlock(std::vector<ast::Stmt *> &body) {
    if (!expect(Tok::LBrace, "'{'") ) {
      return false;
    }
    while (!atEnd() && !check(Tok::RBrace)) {
      const size_t startPos = pos_;
      if (current().kind == Tok::KwLet ||
          current().kind == Tok::KwReturn ||
          current().kind == Tok::KwLoop ||
          current().kind == Tok::KwFor ||
          current().kind == Tok::KwBreak ||
          current().kind == Tok::KwContinue) {
        ast::Stmt *stmt = parseStatement();
        if (stmt != nullptr) {
          body.push_back(stmt);
        } else {
          synchronizeStatement(startPos);
        }
        continue;
      }

      const Span start = current().span;
      ast::Expr *value = parseExpression();
      if (value == nullptr) {
        synchronizeStatement(startPos);
        continue;
      }
      if (match(Tok::Assign)) {
        ast::Expr *assigned = parseExpression();
        if (assigned == nullptr || !expect(Tok::Semi, "';'")) {
          synchronizeStatement(startPos);
          continue;
        }
        body.push_back(arena_.make<ast::AssignStmt>(
            joinSpans(start, previous().span), value, assigned));
        continue;
      }
      if (match(Tok::Semi)) {
        body.push_back(arena_.make<ast::ExprStmt>(
            joinSpans(start, previous().span), value));
        continue;
      }
      if (check(Tok::RBrace)) {
        body.push_back(arena_.make<ast::ExprStmt>(
            joinSpans(start, value->span), value));
        break;
      }
      expected("';' 或 '}'");
      synchronizeStatement(startPos);
    }
    if (!expect(Tok::RBrace, "'}'")) {
      return false;
    }
    if (body.empty() || body.back()->kind != ast::StmtKind::ExprStmt) {
      diags_.error(previous().span, D::ParseExpected,
                   "带值 if 的分支必须以值收尾");
    }
    return true;
  }

  void synchronizeDeclaration(size_t start) {
    if (pos_ == start && !atEnd()) {
      advance();
    }
    while (!atEnd()) {
      if (isDeclarationStart(current().kind)) {
        return;
      }
      advance();
    }
  }

  void synchronizeStatement(size_t start) {
    if (pos_ == start && !atEnd()) {
      advance();
    }
    while (!atEnd() && !check(Tok::RBrace)) {
      if (previous().kind == Tok::Semi ||
          isStatementKeyword(current().kind)) {
        return;
      }
      advance();
    }
  }

  const std::vector<Token> &tokens_;
  size_t pos_ = 0;
  ast::AstArena &arena_;
  DiagSink &diags_;
};

} // namespace

ast::Module *parseModule(const std::vector<Token> &toks, ast::AstArena &arena,
                         DiagSink &diags) {
  Parser parser(toks, 0, arena, diags);
  return parser.parseModule();
}

ast::Expr *parseExpressionAt(const std::vector<Token> &toks, size_t &pos,
                             ast::AstArena &arena, DiagSink &diags) {
  Parser parser(toks, pos, arena, diags);
  ast::Expr *expression = parser.parseExpression();
  pos = parser.position();
  return expression;
}

} // namespace ln
