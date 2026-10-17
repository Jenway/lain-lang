#ifndef LAIN0_LOWER_HPP
#define LAIN0_LOWER_HPP

// 降级：AST -> L1Builder 里的内存 IR。
//
// 硬性纪律（对应 docs/04《判据》：源码里不出现 `#` 形态、验证器不面对用户输入）：
//   * 一切 IR 只能经 Seed.hpp 的构造 API 产生，不许拼含 `#` 的文本；
//   * Lowerer 跑完之后，Driver **必须**先 lainir_verify，通过了才打印；
// 每条 IR 都要写 line/column（L1Inst 自带这两个字段），诊断才有位置。
//
// 模块命名空间（docs/plan-modules.md §3）：run 按 Program 的模块顺序依次降级，
// 共用一个 L1Builder。每进一个模块换一次 ModuleContext 与 SemaResult，发出的
// 符号名一律是 `<模块键>__<名字>`（宿主 extern 例外，保持平坦）。

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
  // 模块集合：run 按它的模块顺序降级，import 的加载已经在 Program 里做完了。
  const Program *program = nullptr;
  // 临时值名字的前缀。注意打印器会自己加 `%`，所以这里**不带** `%`，
  // 否则产物里会出现 `%%r1`。
  const char *tempPrefix = "r";
};

// 顶层静态块 / 字符串字面量共用的取址机制：data 符号本身即地址，
// 用 #bitcast[#addr] 包住符号引用即得 #addr；buildAccessor 再包一层
// `#proc NAME() -> #addr { %base = #bitcast[#addr](%SYMBOL); #return %base }`。
// 这两个是 LowerDecl.cpp 里的自由函数，LowerExpr 降级内联字符串时也要用，故在此声明。
const L1Inst *emitDataAddr(L1Builder *builder, const char *result,
                           const char *symbol, uint32_t line, uint32_t column);
const L1Subroutine *buildAccessor(L1Builder *builder, const char *procName,
                                 const char *symbol, const char *baseName,
                                 const std::vector<const L1Inst *> &extra,
                                 uint32_t line, uint32_t column);

class Lowerer {
public:
  Lowerer(L1Builder *builder, const TypeStore &types, const SourceMgr &src,
          DiagSink &diags, const LowerOptions &opts);

  // 按 Program 的模块顺序把全部模块降级进同一个 L1Builder，
  // 末尾建一个 lainir_module（模块名取根模块键）。
  // 返回 nullptr 表示失败（diags 已填）。
  const L1Module *run(const Program &prog);

  // 拆分产出（`--split`）用的单模块入口：只降级 u 这一个模块，末尾建一个
  // 只含**该模块**数据块与过程的 lainir_module（模块名取该模块键）。
  // 调用方为每份单元新建一个 Lowerer（新 builder），所以 data_/subs_ 天然
  // 只装一个模块；临时值编号也从 0 起 —— 每份单元的文本于是只由它自己的
  // 源码决定，与降级顺序无关。
  const L1Module *runModule(const ModuleUnit *u);

  // --- 以下供内部分工使用，三个 .cpp 各实现一组 ---

  // LowerDecl.cpp：顶层
  bool lowerDecl(const ast::Decl *d);
  bool lowerFunc(const ast::FuncDecl *f, L1Subroutine *&outSub);
  bool lowerExtern(const ast::ExternDecl *e, L1Subroutine *&outSub);
  bool lowerStruct(const ast::StructDecl *s);
  bool lowerEnum(const ast::EnumDecl *e);
  bool lowerStaticLet(const ast::StaticLetDecl *d, std::vector<L1Data> &dataOut,
                      std::vector<L1Subroutine> &subsOut);

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
  bool lowerValueIfExpr(const ast::ValueIfExpr *e, TypeId expectedTy,
                        std::vector<const L1Inst *> &out, L1Operand &value);
  bool lowerIndexExpr(const ast::IndexExpr *e, TypeId expectedTy,
                      std::vector<const L1Inst *> &out, L1Operand &value);
  bool lowerDottedExpr(const ast::DottedExpr *e, TypeId expectedTy,
                       std::vector<const L1Inst *> &out, L1Operand &value);
  bool lowerConstructExpr(const ast::ConstructExpr *e, TypeId expectedTy,
                          std::vector<const L1Inst *> &out, L1Operand &value);
  bool lowerProjection(const ast::CallExpr *e, std::string_view variantName,
                       const AggregateLayout *lay, TypeId expectedTy,
                       std::vector<const L1Inst *> &out, L1Operand &value);
  bool lowerAddressArithmetic(const ast::BinaryExpr *e, TypeId expectedTy,
                              std::vector<const L1Inst *> &out, L1Operand &value);

private:
  // 把一个模块的顶层声明降级进当前 builder（run 与 runModule 共用）。
  // 换模块上下文、清绑定表；import 的加载与解析是 Program 的事。
  bool lowerOne(const ModuleUnit *u);

  // 临时值编号器：编号来自 Lowerer 自己的计数器，不复用 bootstrap 的 scratch +144。
  const char *nextTemp();
  const char *intern(std::string_view s) const;
  uint32_t lineOf(Span s) const;
  uint32_t columnOf(Span s) const;
  void fail(Span s, D code, std::string msg);

  // --- 模块上下文（run 的模块循环每进一个模块换一次）---
  // 源码里的裸名字 -> 当前模块的限定键。
  std::string qualify(std::string_view name) const { return ctx_.qualify(name); }
  // 源码里的顶层名字 -> 发出的符号名（func 与静态 let 带前缀，宿主 extern 平坦）。
  const char *symbolFor(std::string_view srcName) const;
  // 类型标注按当前模块上下文解析。
  TypeId resolveTy(const ast::TypeSyntax &ts) const;
  // 按当前模块上下文查类型名（enum/struct）。
  TypeId lookupTy(std::string_view name) const;

  L1Builder *builder_ = nullptr;
  const TypeStore *types_ = nullptr;
  const SourceMgr *src_ = nullptr;
  DiagSink *diags_ = nullptr;
  LowerOptions opts_{};
  // 当前正在降级的模块。run 每进一个模块就换一次。
  const SemaResult *sema_ = nullptr;
  ModuleContext ctx_{};
  // 名字 -> 当前代表该值的操作数。局部 let、赋值、循环参数的重新绑定都走这里。
  // 这是 SSA 形状的记账：名字只是值的引用，重绑定等价于换一个引用。
  std::unordered_map<std::string, L1Operand> bindings_{};
  // 当前正在降级的循环的参数（名 + 类型）。循环体里的语句 if 若给这些参数赋了值，
  // 那个新值必须靠 #if 带结果合并出来——分支内定义的值在外层是看不见的。
  std::vector<std::pair<std::string, const L1Type *>> loopParams_{};
  uint32_t tempCounter_ = 0;
  uint32_t strlitCounter_ = 0;  // 内联字符串字面量的唯一存储名计数器
  std::vector<L1Data> data_{};
  std::vector<L1Subroutine> subs_{};
};

} // namespace ln

#endif
