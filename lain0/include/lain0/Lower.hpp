#ifndef LAIN0_LOWER_HPP
#define LAIN0_LOWER_HPP

// 降级：AST -> L1Builder 里的内存 IR。
//
// 硬性纪律（对应 docs/04《判据》：源码里不出现 `#` 形态、验证器不面对用户输入）：
//   * 一切 IR 只能经 Seed.hpp 的构造 API 产生，不许拼含 `#` 的文本；
//   * Lowerer 跑完之后，Driver **必须**先 lainir_verify，通过了才打印；
// 每条 IR 都要写 line/column（L1Inst 自带这两个字段），诊断才有位置。

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "lain0/Ast.hpp"
#include "lain0/Diag.hpp"
#include "lain0/Seed.hpp"
#include "lain0/Sema.hpp"
#include "lain0/Source.hpp"

namespace ln {

struct LowerOptions {
  const char *moduleName = "lain";
  // 临时值名字的前缀。注意打印器会自己加 `%`，所以这里**不带** `%`，
  // 否则产物里会出现 `%%r1`。
  const char *tempPrefix = "r";
};

class Lowerer {
public:
  Lowerer(L1Builder *builder, const TypeStore &types, const SemaResult &sema,
          const SourceMgr &src, DiagSink &diags, const LowerOptions &opts);

  // 返回 nullptr 表示失败（diags 已填）。
  const L1Module *run(const ast::Module &mod);

  // --- 以下供内部分工使用，三个 .cpp 各实现一组 ---

  // LowerDecl.cpp：顶层
  bool lowerDecl(const ast::Decl *d);
  bool lowerFunc(const ast::FuncDecl *f, L1Subroutine *&outSub);
  bool lowerExtern(const ast::ExternDecl *e, L1Subroutine *&outSub);
  bool lowerStruct(const ast::StructDecl *s);
  bool lowerEnum(const ast::EnumDecl *e);
  bool lowerStaticLet(const ast::StaticLetDecl *d, std::vector<L1Data> &dataOut,
                      std::vector<L1Subroutine> &subsOut);
  bool lowerImport(const ast::ImportDecl *d, std::vector<L1Subroutine> &subsOut);

  // LowerStmt.cpp：语句与控制流
  bool lowerBody(const std::vector<ast::Stmt *> &body,
                 std::vector<const L1Inst *> &out, TypeId returnType,
                 bool &terminated);
  bool lowerStmt(const ast::Stmt *s, std::vector<const L1Inst *> &out,
                 TypeId returnType, bool &terminated);
  bool lowerIfStmt(const ast::IfStmt *s, std::vector<const L1Inst *> &out,
                   TypeId returnType, bool &terminated);
  bool lowerLoopStmt(const ast::LoopStmt *s, std::vector<const L1Inst *> &out,
                     TypeId returnType);
  bool lowerForStmt(const ast::ForStmt *s, std::vector<const L1Inst *> &out,
                    TypeId returnType);
  bool lowerJumpStmt(const ast::JumpStmt *s, std::vector<const L1Inst *> &out);

  // LowerExpr.cpp：表达式
  // 返回 true 并给出代表该值的操作数；临时值会追加进 out。
  bool lowerExpr(const ast::Expr *e, TypeId expectedTy,
                 std::vector<const L1Inst *> &out, L1Operand &value);
  bool lowerBinary(const ast::BinaryExpr *e, TypeId expectedTy,
                   std::vector<const L1Inst *> &out, L1Operand &value);
  bool lowerCall(const ast::CallExpr *e, TypeId expectedTy,
                 std::vector<const L1Inst *> &out, L1Operand &value,
                 bool tail = false);
  bool lowerBuiltin(const ast::CallExpr *e, std::string_view name,
                    TypeId expectedTy, std::vector<const L1Inst *> &out,
                    L1Operand &value);
  bool lowerCast(const ast::CastExpr *e, std::vector<const L1Inst *> &out,
                 L1Operand &value);
  bool lowerLoopExpr(const ast::LoopExpr *e, TypeId expectedTy,
                    std::vector<const L1Inst *> &out, L1Operand &value);
  bool lowerForExpr(const ast::ForExpr *e, TypeId expectedTy,
                    std::vector<const L1Inst *> &out, L1Operand &value);
  bool lowerAddressArithmetic(const ast::BinaryExpr *e, TypeId expectedTy,
                              std::vector<const L1Inst *> &out,
                              L1Operand &value);

private:
  // 临时值编号器：编号来自 Lowerer 自己的计数器，不复用 bootstrap 的 scratch +144。
  const char *nextTemp();
  const char *intern(std::string_view s);
  uint32_t lineOf(Span s) const;
  uint32_t columnOf(Span s) const;
  void fail(Span s, D code, std::string msg);

  L1Builder *builder_ = nullptr;
  const TypeStore *types_ = nullptr;
  const SemaResult *sema_ = nullptr;
  const SourceMgr *src_ = nullptr;
  DiagSink *diags_ = nullptr;
  LowerOptions opts_{};
  // 名字 -> 当前代表该值的操作数。局部 let、赋值、循环参数的重新绑定都走这里。
  // 这是 SSA 形状的记账：名字只是值的引用，重绑定等价于换一个引用。
  std::unordered_map<std::string, L1Operand> bindings_{};
  uint32_t tempCounter_ = 0;
  std::vector<L1Data> data_{};
  std::vector<L1Subroutine> subs_{};
};

} // namespace ln

#endif
