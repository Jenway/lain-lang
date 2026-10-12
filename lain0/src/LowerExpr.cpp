// 表达式降级。
//
// 一条铁律：宽度只能来自 IR 自己（TypeStore::width / lainir_inst_result_type），
// 不许传递什么「期望宽度」——stage 0 那一整类 bug 就是那么来的。

#include <string>
#include <unordered_map>
#include <vector>

#include "lain0/Lower.hpp"

namespace ln {
namespace {

bool isComparison(ast::BinOp op) {
  switch (op) {
  case ast::BinOp::Eq:
  case ast::BinOp::Ne:
  case ast::BinOp::Lt:
  case ast::BinOp::Le:
  case ast::BinOp::Gt:
  case ast::BinOp::Ge:
    return true;
  default:
    return false;
  }
}

const char *builtinName(const ast::Expr *callee) {
  if (!callee || callee->kind != ast::ExprKind::Name) return nullptr;
  return nullptr; // 名字在 string_view 里，下面用直接比较，不走这个
}

} // namespace

// 取某个表达式在 Sema 里定下的类型；没定就退回期望类型。
static TypeId typeOfExpr(const SemaResult &sema, const ast::Expr *e, TypeId expected) {
  if (!e) return expected;
  auto it = sema.exprTypes.find(e);
  if (it != sema.exprTypes.end() && it->second != kInvalidType) return it->second;
  return expected;
}

bool Lowerer::lowerExpr(const ast::Expr *e, TypeId expectedTy,
                        std::vector<const L1Inst *> &out, L1Operand &value) {
  if (!e) {
    fail(Span{}, D::LowerUnsupported, "空表达式");
    return false;
  }

  switch (e->kind) {
  case ast::ExprKind::IntLit: {
    value = lainir_int(static_cast<const ast::IntLitExpr *>(e)->value);
    return true;
  }
  case ast::ExprKind::BoolLit: {
    value = lainir_int(static_cast<const ast::BoolLitExpr *>(e)->value ? 1u : 0u);
    return true;
  }
  case ast::ExprKind::Name: {
    const auto *n = static_cast<const ast::NameExpr *>(e);
    // 先查绑定表：let / 赋值 / 循环参数重绑定后，名字对应的是某个临时值。
    // 表里没有才当作过程参数（参数名与 IR 形参名一致）。
    auto it = bindings_.find(std::string(n->name));
    if (it != bindings_.end()) {
      value = it->second;
      return true;
    }
    value = lainir_ref(intern(n->name));
    return true;
  }
  case ast::ExprKind::Unary: {
    const auto *u = static_cast<const ast::UnaryExpr *>(e);
    TypeId ty = typeOfExpr(*sema_, u->operand, expectedTy);
    if (ty == kInvalidType) ty = expectedTy;
    L1Operand operand{};
    if (!lowerExpr(u->operand, ty, out, operand)) return false;
    // 负号只认字面量：发 `#sub[T](0, 字面量)`。
    if (u->operand->kind != ast::ExprKind::IntLit) {
      fail(u->span, D::LowerUnsupported, "一元负号只作用于字面量");
      return false;
    }
    const L1Type *t = types_->lain(ty);
    const char *res = nextTemp();
    L1Operand ops[2] = {lainir_int(0), operand};
    out.push_back(lainir_inst(builder_, INST_SUB, res, t, ops, 2));
    value = lainir_ref(res);
    return true;
  }
  case ast::ExprKind::Binary: {
    const auto *b = static_cast<const ast::BinaryExpr *>(e);
    // 地址算术（p + i / p + i*K）走 lea，不进算子表。
    TypeId lhsTy = typeOfExpr(*sema_, b->lhs, expectedTy);
    if (b->op == ast::BinOp::Add && lhsTy != kInvalidType && types_->isAddr(lhsTy)) {
      return lowerAddressArithmetic(b, expectedTy, out, value);
    }
    return lowerBinary(b, expectedTy, out, value);
  }
  case ast::ExprKind::Cast:
    return lowerCast(static_cast<const ast::CastExpr *>(e), out, value);
  case ast::ExprKind::Call: {
    const auto *c = static_cast<const ast::CallExpr *>(e);
    if (c->callee && c->callee->kind == ast::ExprKind::Name) {
      const auto *n = static_cast<const ast::NameExpr *>(c->callee);
      if (n->name == "load" || n->name == "store" || n->name == "lea" ||
          n->name == "ptr_of" || n->name == "addr_of" || n->name == "len") {
        return lowerBuiltin(c, n->name, expectedTy, out, value);
      }
    }
    return lowerCall(c, expectedTy, out, value);
  }
  default:
    fail(e->span, D::LowerUnsupported, "这个表达式形态还没实现降级");
    return false;
  }
}

bool Lowerer::lowerBinary(const ast::BinaryExpr *e, TypeId expectedTy,
                          std::vector<const L1Inst *> &out, L1Operand &value) {
  TypeId lhsTy = typeOfExpr(*sema_, e->lhs, expectedTy);
  TypeId rhsTy = typeOfExpr(*sema_, e->rhs, lhsTy);
  TypeId operandTy = lhsTy != kInvalidType ? lhsTy : rhsTy;
  if (operandTy == kInvalidType) {
    fail(e->span, D::SemaWidthUnknown, "二元运算的操作数类型无法推导");
    return false;
  }

  OperatorChoice choice{};
  if (!selectBinary(e->op, operandTy, *types_, choice)) {
    fail(e->span, D::SemaNoSuchOperator,
         std::string(types_->name(operandTy)) + " 上没有这个算子");
    return false;
  }

  L1Operand lhs{};
  L1Operand rhs{};
  if (!lowerExpr(e->lhs, operandTy, out, lhs)) return false;
  if (!lowerExpr(e->rhs, operandTy, out, rhs)) return false;

  const char *res = nextTemp();
  L1Operand ops[2] = {lhs, rhs};
  out.push_back(lainir_inst(builder_, choice.kind, res, choice.operandTy, ops, 2));
  value = lainir_ref(res);
  return true;
}

bool Lowerer::lowerCall(const ast::CallExpr *e, TypeId expectedTy,
                        std::vector<const L1Inst *> &out, L1Operand &value) {
  if (!e->callee || e->callee->kind != ast::ExprKind::Name) {
    fail(e->span, D::LowerUnsupported, "只支持直接调用具名过程");
    return false;
  }
  const auto *n = static_cast<const ast::NameExpr *>(e->callee);

  std::vector<L1Operand> args;
  args.reserve(e->args.size());
  for (ast::Expr *a : e->args) {
    L1Operand op{};
    if (!lowerExpr(a, kInvalidType, out, op)) return false;
    args.push_back(op);
  }

  const L1Type *retTy = expectedTy != kInvalidType ? types_->lain(expectedTy) : nullptr;
  const char *res = retTy ? nextTemp() : nullptr;
  const L1Inst *call = lainir_inst_call(builder_, res, intern(n->name),
                                        args.empty() ? nullptr : args.data(),
                                        static_cast<uint32_t>(args.size()));
  out.push_back(call);
  value = res ? lainir_ref(res) : L1Operand{};
  return true;
}

bool Lowerer::lowerBuiltin(const ast::CallExpr *e, std::string_view name,
                           TypeId expectedTy, std::vector<const L1Inst *> &out,
                           L1Operand &value) {
  if (name == "load") {
    if (expectedTy == kInvalidType) {
      fail(e->span, D::SemaWidthUnknown, "load 的宽度只能来自期望类型，不猜");
      return false;
    }
    if (e->args.size() != 1) {
      fail(e->span, D::SemaTypeMismatch, "load 需要一个实参");
      return false;
    }
    L1Operand ptr{};
    if (!lowerExpr(e->args[0], kInvalidType, out, ptr)) return false;
    const char *res = nextTemp();
    out.push_back(lainir_inst_mem(builder_, INST_LOAD, res, types_->lain(expectedTy),
                                  ORDER_RELAXED, false, &ptr, 1));
    value = lainir_ref(res);
    return true;
  }
  if (name == "store") {
    if (e->args.size() != 2) {
      fail(e->span, D::SemaTypeMismatch, "store 需要两个实参");
      return false;
    }
    L1Operand ptr{};
    L1Operand val{};
    if (!lowerExpr(e->args[0], kInvalidType, out, ptr)) return false;
    TypeId valTy = typeOfExpr(*sema_, e->args[1], kInvalidType);
    if (!lowerExpr(e->args[1], valTy, out, val)) return false;
    L1Operand ops[2] = {val, ptr}; // store 的操作数顺序固定为「值, 地址」
    out.push_back(lainir_inst_mem(builder_, INST_STORE, nullptr,
                                  types_->lain(valTy), ORDER_RELAXED, false, ops, 2));
    value = L1Operand{};
    return true;
  }
  if (name == "lea") {
    if (e->args.size() != 2) {
      fail(e->span, D::SemaTypeMismatch, "lea 需要两个实参");
      return false;
    }
    L1Operand base{};
    L1Operand index{};
    if (!lowerExpr(e->args[0], kInvalidType, out, base)) return false;
    if (!lowerExpr(e->args[1], types_->builtin("u64"), out, index)) return false;
    L1Operand ops[4] = {base, index, lainir_int(1), lainir_int(0)};
    const char *res = nextTemp();
    out.push_back(lainir_inst(builder_, INST_LEA, res, nullptr, ops, 4));
    value = lainir_ref(res);
    return true;
  }
  if (name == "ptr_of") {
    if (e->args.size() != 1) {
      fail(e->span, D::SemaTypeMismatch, "ptr_of 需要一个实参");
      return false;
    }
    L1Operand ptr{};
    if (!lowerExpr(e->args[0], types_->builtin("addr"), out, ptr)) return false;
    uint32_t width = expectedTy != kInvalidType ? types_->width(expectedTy) : 64;
    const char *res = nextTemp();
    out.push_back(lainir_inst(builder_, INST_PTR2INT, res,
                              lainir_type(builder_, TY_BITS, width), &ptr, 1));
    value = lainir_ref(res);
    return true;
  }
  if (name == "addr_of") {
    if (e->args.size() != 1) {
      fail(e->span, D::SemaTypeMismatch, "addr_of 需要一个实参");
      return false;
    }
    L1Operand num{};
    if (!lowerExpr(e->args[0], types_->builtin("u64"), out, num)) return false;
    const char *res = nextTemp();
    out.push_back(lainir_inst(builder_, INST_INT2PTR, res,
                              lainir_type(builder_, TY_ADDR, 0), &num, 1));
    value = lainir_ref(res);
    return true;
  }
  if (name == "len") {
    // len(t) 发元素个数；表的长度在 Sema 里没登记，Slice A 退回字面量 0 并报不支持。
    fail(e->span, D::LowerUnsupported, "len 需要表的元素个数，暂未接线");
    return false;
  }
  fail(e->span, D::LowerUnsupported, "未知的内建名");
  return false;
}

bool Lowerer::lowerCast(const ast::CastExpr *e, std::vector<const L1Inst *> &out,
                        L1Operand &value) {
  TypeId target = types_->resolve(e->target, *diags_);
  if (target == kInvalidType) return false;
  TypeId src = typeOfExpr(*sema_, e->operand, kInvalidType);
  if (src == kInvalidType) src = target;

  L1Operand operand{};
  if (!lowerExpr(e->operand, src, out, operand)) return false;

  const uint32_t toW = types_->width(target);
  const uint32_t fromW = types_->width(src);

  if (toW == fromW) {
    // 等宽：补一条加零复制，保持「值依然是新的临时值」这个形状。
    const char *res = nextTemp();
    L1Operand ops[2] = {operand, lainir_int(0)};
    out.push_back(lainir_inst(builder_, INST_ADD, res, types_->lain(target), ops, 2));
    value = lainir_ref(res);
    return true;
  }
  const char *res = nextTemp();
  out.push_back(lainir_inst(builder_, toW > fromW ? INST_ZEXT : INST_TRUNC, res,
                            types_->lain(target), &operand, 1));
  value = lainir_ref(res);
  return true;
}

bool Lowerer::lowerAddressArithmetic(const ast::BinaryExpr *e, TypeId expectedTy,
                                     std::vector<const L1Inst *> &out,
                                     L1Operand &value) {
  // p + i          -> #lea(p, i, 1, 0)
  // p + i * K      -> #lea(p, i, K, 0)（K 是无符号十进制字面量）
  // i + p          -> 拒绝：会拿地址当整数
  const ast::Expr *rhs = e->rhs;
  uint64_t scale = 1;
  const ast::Expr *indexExpr = rhs;

  if (rhs && rhs->kind == ast::ExprKind::Binary) {
    const auto *b = static_cast<const ast::BinaryExpr *>(rhs);
    if (b->op == ast::BinOp::Mul && b->rhs && b->rhs->kind == ast::ExprKind::IntLit) {
      scale = static_cast<const ast::IntLitExpr *>(b->rhs)->value;
      indexExpr = b->lhs;
    } else {
      fail(e->span, D::LowerUnsupported, "地址算术只允许 p + i 或 p + i * K");
      return false;
    }
  }

  L1Operand base{};
  L1Operand index{};
  if (!lowerExpr(e->lhs, types_->builtin("addr"), out, base)) return false;
  if (!lowerExpr(indexExpr, types_->builtin("u64"), out, index)) return false;

  L1Operand ops[4] = {base, index, lainir_int(scale), lainir_int(0)};
  const char *res = nextTemp();
  out.push_back(lainir_inst(builder_, INST_LEA, res, nullptr, ops, 4));
  value = lainir_ref(res);
  return true;
}

} // namespace ln
