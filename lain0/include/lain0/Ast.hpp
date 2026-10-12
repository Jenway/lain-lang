#ifndef LAIN0_AST_HPP
#define LAIN0_AST_HPP

// AST：纯语法形状，不含任何语义结论。语义结论由 Sema 挂在旁边（见 Sema.hpp）。
//
// 所有节点都从 arena 分配，parser 结束后整棵树的寿命由 AstArena 掌管。

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "lain0/Source.hpp"

namespace ln {
namespace ast {

struct Node {
  virtual ~Node() = default;
};

// ---------------- 语法层面的类型引用 ----------------
// 表面语言写在源码里的类型标注。降级前由 Sema 解析成 TypeId。
// 覆盖：`i32`、`addr`、`Point`、`M.Point`、`[u64]`、`ro [u8]`。
struct TypeSyntax {
  Span span{};
  std::string_view name{};       // 最后一个标识符（`M.Point` 只存 `Point`）
  bool hasQualifier = false;     // 是否写了限定名
  std::string_view qualifier{};  // `M`
  bool isArray = false;          // `[T]`
  bool isRo = false;             // `ro [T]`
  TypeSyntax *element = nullptr; // 数组元素类型；非数组时为 nullptr
  bool valid() const { return !name.empty(); }
};

// ---------------- 表达式 ----------------

// 二元算子。词算子 `and` / `or` 与符号同算子。
enum class BinOp {
  Add, Sub, Mul, Div, Rem,
  Shl, Shr, BitAnd, BitOr, BitXor,
  Eq, Ne, Lt, Le, Gt, Ge,
  LogicalAnd, LogicalOr,
};

enum class UnOp { Neg };

enum class ExprKind {
  IntLit,   // 整数字面量（含十六进制，已规范化为数值）
  StrLit,   // 字节串字面量："hi"
  BoolLit,
  Name,     // 裸名字
  Dotted,   // M.answer / p.x / Shape.Circle
  Call,     // f(...)、宏调用、以及全部内建（load/store/lea/ptr_of/addr_of/len）
  Index,    // t[i]
  Unary,    // -<字面量>（仅字面量，文档约定）
  Binary,
  Cast,     // x as T
  Construct,// Point { x: 1 y: 2 } / Shape.Circle { k }
  Table,    // [10, 20, 30]
  ValueIf,  // if c { a } else { b } 当值用
};

struct Expr : Node {
  explicit Expr(ExprKind k, Span s) : kind(k), span(s) {}
  ExprKind kind;
  Span span;
};

struct IntLitExpr : Expr {
  IntLitExpr(Span s, uint64_t v) : Expr(ExprKind::IntLit, s), value(v) {}
  uint64_t value = 0;
};

struct StrLitExpr : Expr {
  StrLitExpr(Span s, std::string_view raw) : Expr(ExprKind::StrLit, s), rawText(raw) {}
  std::string_view rawText{}; // 不含引号的原文；转义与字节化在 Sema/Lower 做
};

struct BoolLitExpr : Expr {
  BoolLitExpr(Span s, bool v) : Expr(ExprKind::BoolLit, s), value(v) {}
  bool value = false;
};

struct NameExpr : Expr {
  NameExpr(Span s, std::string_view n) : Expr(ExprKind::Name, s), name(n) {}
  std::string_view name{};
};

struct DottedExpr : Expr {
  DottedExpr(Span s, Expr *o, std::string_view m)
      : Expr(ExprKind::Dotted, s), object(o), member(m) {}
  Expr *object = nullptr;
  std::string_view member{};
};

struct CallExpr : Expr {
  CallExpr(Span s, Expr *c, std::vector<Expr *> a)
      : Expr(ExprKind::Call, s), callee(c), args(std::move(a)) {}
  Expr *callee = nullptr; // 通常是 NameExpr 或 DottedExpr
  std::vector<Expr *> args{};
};

struct IndexExpr : Expr {
  IndexExpr(Span s, Expr *b, Expr *i) : Expr(ExprKind::Index, s), base(b), index(i) {}
  Expr *base = nullptr;
  Expr *index = nullptr;
};

struct UnaryExpr : Expr {
  UnaryExpr(Span s, UnOp o, Expr *v) : Expr(ExprKind::Unary, s), op(o), operand(v) {}
  UnOp op = UnOp::Neg;
  Expr *operand = nullptr;
};

struct BinaryExpr : Expr {
  BinaryExpr(Span s, BinOp o, Expr *l, Expr *r)
      : Expr(ExprKind::Binary, s), op(o), lhs(l), rhs(r) {}
  BinOp op = BinOp::Add;
  Expr *lhs = nullptr;
  Expr *rhs = nullptr;
};

struct CastExpr : Expr {
  CastExpr(Span s, Expr *v, TypeSyntax t)
      : Expr(ExprKind::Cast, s), operand(v), target(t) {}
  Expr *operand = nullptr;
  TypeSyntax target{};
};

struct FieldInit {
  Span span{};
  std::string_view name{};
  Expr *value = nullptr;
};

struct ConstructExpr : Expr {
  ConstructExpr(Span s, TypeSyntax t, std::vector<FieldInit> f)
      : Expr(ExprKind::Construct, s), target(t), fields(std::move(f)) {}
  TypeSyntax target{};
  std::vector<FieldInit> fields{};
};

struct TableExpr : Expr {
  TableExpr(Span s, std::vector<Expr *> e) : Expr(ExprKind::Table, s), elements(std::move(e)) {}
  std::vector<Expr *> elements{};
};

struct Stmt;

struct ValueIfExpr : Expr {
  ValueIfExpr(Span s, Expr *c, std::vector<Stmt *> t, std::vector<Stmt *> f)
      : Expr(ExprKind::ValueIf, s), cond(c), thenBody(std::move(t)), elseBody(std::move(f)) {}
  Expr *cond = nullptr;
  std::vector<Stmt *> thenBody{};
  std::vector<Stmt *> elseBody{}; // 可空表示省略 else
  bool hasElse = false;
};

// ---------------- 语句 ----------------

enum class StmtKind { Let, Return, Assign, ExprStmt, If, Loop, For, Jump };

struct Stmt : Node {
  explicit Stmt(StmtKind k, Span s) : kind(k), span(s) {}
  StmtKind kind;
  Span span;
};

struct LetStmt : Stmt {
  LetStmt(Span s, std::string_view n) : Stmt(StmtKind::Let, s), name(n) {}
  std::string_view name{};
  TypeSyntax type{};
  bool hasType = false;
  bool isRo = false;
  Expr *init = nullptr;
};

struct ReturnStmt : Stmt {
  explicit ReturnStmt(Span s) : Stmt(StmtKind::Return, s) {}
  Expr *value = nullptr; // 可空：裸 return
};

struct AssignStmt : Stmt {
  AssignStmt(Span s, Expr *t, Expr *v) : Stmt(StmtKind::Assign, s), target(t), value(v) {}
  Expr *target = nullptr;
  Expr *value = nullptr;
};

struct ExprStmt : Stmt {
  ExprStmt(Span s, Expr *e) : Stmt(StmtKind::ExprStmt, s), expr(e) {}
  Expr *expr = nullptr;
};

struct IfStmt : Stmt {
  explicit IfStmt(Span s) : Stmt(StmtKind::If, s) {}
  Expr *cond = nullptr;
  std::vector<Stmt *> thenBody{};
  std::vector<Stmt *> elseBody{};
  bool hasElse = false;
};

struct LoopParam {
  Span span{};
  std::string_view name{};
  TypeSyntax type{};
  Expr *init = nullptr; // 可空（acc 可省）
};

struct LoopStmt : Stmt {
  explicit LoopStmt(Span s) : Stmt(StmtKind::Loop, s) {}
  std::string_view label{};
  bool hasLabel = false;
  std::vector<LoopParam> params{};
  Expr *cond = nullptr; // nullptr => 无条件（靠 break 退出）
  std::vector<Stmt *> body{};
};

struct ForStmt : Stmt {
  explicit ForStmt(Span s) : Stmt(StmtKind::For, s) {}
  std::string_view var{};
  TypeSyntax varType{};
  bool hasVarType = false;
  Expr *from = nullptr;
  Expr *to = nullptr; // 半开区间 [from, to)
  bool hasAcc = false;
  std::string_view acc{};
  TypeSyntax accType{};
  Expr *accInit = nullptr;
  std::vector<Stmt *> body{};
};

struct JumpStmt : Stmt {
  JumpStmt(Span s, bool brk) : Stmt(StmtKind::Jump, s), isBreak(brk) {}
  bool isBreak = true;
  std::string_view label{};
  std::vector<Expr *> args{};
};

// ---------------- 声明 ----------------

enum class DeclKind { Func, Extern, Struct, Enum, Import, StaticLet };

struct Decl : Node {
  explicit Decl(DeclKind k, Span s) : kind(k), span(s) {}
  DeclKind kind;
  Span span;
  std::string_view name{};
};

struct ParamDecl {
  Span span{};
  std::string_view name{};
  TypeSyntax type{};
};

struct FieldDecl {
  Span span{};
  std::string_view name{};
  TypeSyntax type{};
};

struct VariantDecl {
  Span span{};
  std::string_view name{};
  std::vector<TypeSyntax> payload{}; // 0 或 1 个
};

struct FuncDecl : Decl {
  explicit FuncDecl(Span s) : Decl(DeclKind::Func, s) {}
  std::vector<ParamDecl> params{};
  TypeSyntax ret{};
  bool hasRet = false;
  std::vector<Stmt *> body{};
};

struct ExternDecl : Decl {
  explicit ExternDecl(Span s) : Decl(DeclKind::Extern, s) {}
  std::vector<ParamDecl> params{};
  TypeSyntax ret{};
  bool hasRet = false;
  std::string_view linkName{};
};

struct StructDecl : Decl {
  explicit StructDecl(Span s) : Decl(DeclKind::Struct, s) {}
  std::vector<FieldDecl> fields{}; // 声明体里空格分隔
};

struct EnumDecl : Decl {
  explicit EnumDecl(Span s) : Decl(DeclKind::Enum, s) {}
  std::vector<VariantDecl> variants{};
};

struct ImportDecl : Decl {
  explicit ImportDecl(Span s) : Decl(DeclKind::Import, s) {}
  std::string_view path{}; // import("examples::modules_lib") 里的字符串原文
};

// 顶层 `let NAME: T = <值>;` —— 字节串、定长表、顶层聚合构造都落这里。
struct StaticLetDecl : Decl {
  explicit StaticLetDecl(Span s) : Decl(DeclKind::StaticLet, s) {}
  TypeSyntax type{};
  bool hasType = false;
  bool isRo = false;
  Expr *init = nullptr;
};

struct Module : Node {
  std::string_view name{}; // 由文件名/驱动给出
  std::vector<Decl *> decls{};
};

// ---------------- Arena ----------------

class AstArena {
public:
  template <class T, class... Args> T *make(Args &&...args) {
    static_assert(std::is_base_of_v<Node, T>, "AST nodes must derive from Node");
    auto owned = std::make_unique<T>(std::forward<Args>(args)...);
    T *raw = owned.get();
    nodes_.push_back(std::move(owned));
    return raw;
  }

  template <class T> T *makeSpan(Span s, auto &&...args) {
    return make<T>(s, std::forward<decltype(args)>(args)...);
  }

private:
  std::vector<std::unique_ptr<Node>> nodes_;
};

} // namespace ast
} // namespace ln

#endif
