// 语句与控制流降级。
//
// 形状上的三条规矩（都来自 docs/04）：
//   * 带值 #if 的两支都必须以 #yield 收尾；
//   * #loop 只有显式 #continue 才形成回边，落到区域末尾即顺序离开；
//   * 以 return / break / continue 收尾的分支不再要求 #yield，
//     但它们之后不许再降级任何语句。

#include <string>
#include <vector>

#include "lain0/Lower.hpp"

namespace ln {

bool Lowerer::lowerBody(const std::vector<ast::Stmt *> &body,
                        std::vector<const L1Inst *> &out, TypeId returnType,
                        bool &terminated) {
  terminated = false;
  for (const ast::Stmt *s : body) {
    if (terminated) {
      fail(s->span, D::LowerUnsupported, "终结指令之后还有语句");
      return false;
    }
    if (!lowerStmt(s, out, returnType, terminated)) return false;
  }
  return true;
}

bool Lowerer::lowerStmt(const ast::Stmt *s, std::vector<const L1Inst *> &out,
                        TypeId returnType, bool &terminated) {
  if (!s) return true;
  switch (s->kind) {
  case ast::StmtKind::Let: {
    const auto *l = static_cast<const ast::LetStmt *>(s);
    TypeId declared = l->hasType ? types_->resolve(l->type, *diags_) : kInvalidType;
    TypeId ty = declared;
    if (ty == kInvalidType) {
      auto it = sema_->exprTypes.find(l->init);
      if (it != sema_->exprTypes.end()) ty = it->second;
    }
    if (!l->init) {
      fail(l->span, D::LowerUnsupported, "let 必须有初值");
      return false;
    }
    L1Operand value{};
    if (!lowerExpr(l->init, ty, out, value)) return false;
    bindings_[std::string(l->name)] = value;
    return true;
  }
  case ast::StmtKind::Return: {
    const auto *r = static_cast<const ast::ReturnStmt *>(s);
    if (!r->value) {
      out.push_back(lainir_inst_jump(builder_, INST_RETURN, nullptr, nullptr, 0));
      terminated = true;
      return true;
    }
    L1Operand value{};
    const ast::Expr *val = r->value;
    if (val->kind == ast::ExprKind::Call) {
      const auto *ce = static_cast<const ast::CallExpr *>(val);
      const ast::Expr *callee = ce->callee;
      bool builtinCall =
          callee && callee->kind == ast::ExprKind::Name &&
          (static_cast<const ast::NameExpr *>(callee)->name == "load" ||
           static_cast<const ast::NameExpr *>(callee)->name == "store" ||
           static_cast<const ast::NameExpr *>(callee)->name == "lea" ||
           static_cast<const ast::NameExpr *>(callee)->name == "ptr_of" ||
           static_cast<const ast::NameExpr *>(callee)->name == "addr_of" ||
           static_cast<const ast::NameExpr *>(callee)->name == "len");
      // 投影 Shape.Circle(s) 长得像调用，但它不是过程调用 —— 不能走尾调用。
      bool projectionCall = false;
      if (callee != nullptr && callee->kind == ast::ExprKind::Dotted) {
        const auto *d = static_cast<const ast::DottedExpr *>(callee);
        if (d->object != nullptr && d->object->kind == ast::ExprKind::Name) {
          const auto *en = static_cast<const ast::NameExpr *>(d->object);
          const TypeId enumTy = types_->lookup(en->name);
          const AggregateLayout *elay =
              enumTy != kInvalidType ? types_->layout(enumTy) : nullptr;
          projectionCall = elay != nullptr && !elay->variants.empty();
        }
      }
      if (!builtinCall && !projectionCall &&
          lowerCall(ce, returnType, out, value, /*tail=*/true)) {
        out.push_back(lainir_inst_jump(builder_, INST_RETURN, nullptr, &value, 1));
        terminated = true;
        return true;
      }
    }
    if (!lowerExpr(val, returnType, out, value)) return false;
    out.push_back(lainir_inst_jump(builder_, INST_RETURN, nullptr, &value, 1));
    terminated = true;
    return true;
  }
  case ast::StmtKind::Assign: {
    const auto *a = static_cast<const ast::AssignStmt *>(s);
    std::string targetName;
    if (a->target && a->target->kind == ast::ExprKind::Name) {
      targetName = std::string(static_cast<const ast::NameExpr *>(a->target)->name);
    } else {
      fail(a->span, D::LowerUnsupported, "赋值目标只能是名字");
      return false;
    }
    TypeId ty = kInvalidType;
    auto it = sema_->exprTypes.find(a->value);
    if (it != sema_->exprTypes.end()) ty = it->second;
    L1Operand value{};
    if (!lowerExpr(a->value, ty, out, value)) return false;
    bindings_[targetName] = value;
    return true;
  }
  case ast::StmtKind::ExprStmt: {
    const auto *e = static_cast<const ast::ExprStmt *>(s);
    L1Operand ignored{};
    return lowerExpr(e->expr, kInvalidType, out, ignored);
  }
  case ast::StmtKind::If:
    return lowerIfStmt(static_cast<const ast::IfStmt *>(s), out, returnType,
                       terminated);
  case ast::StmtKind::Loop:
    return lowerLoopStmt(static_cast<const ast::LoopStmt *>(s), out, returnType);
  case ast::StmtKind::For:
    return lowerForStmt(static_cast<const ast::ForStmt *>(s), out, returnType);
  case ast::StmtKind::Jump: {
    // break/continue 是终止语句：它之后不能再发指令（否则验证器报 2003）。
    const bool ok = lowerJumpStmt(static_cast<const ast::JumpStmt *>(s), out);
    if (ok) terminated = true;
    return ok;
  }
  }
  return true;
}

namespace {

// 这段语句里有没有给 name 赋过值（嵌套 if 也要看）。
bool assignsNameInStmt(const ast::Stmt *s, const std::string &name);

bool assignsNameInBody(const std::vector<ast::Stmt *> &body,
                       const std::string &name) {
  for (const ast::Stmt *s : body)
    if (assignsNameInStmt(s, name)) return true;
  return false;
}

bool assignsNameInStmt(const ast::Stmt *s, const std::string &name) {
  if (!s) return false;
  if (s->kind == ast::StmtKind::Assign) {
    const auto *a = static_cast<const ast::AssignStmt *>(s);
    if (a->target && a->target->kind == ast::ExprKind::Name) {
      const auto *n = static_cast<const ast::NameExpr *>(a->target);
      return n->name == name;
    }
    return false;
  }
  if (s->kind == ast::StmtKind::If) {
    const auto *i = static_cast<const ast::IfStmt *>(s);
    if (assignsNameInBody(i->thenBody, name)) return true;
    if (i->hasElse && assignsNameInBody(i->elseBody, name)) return true;
  }
  return false;
}

} // namespace

bool Lowerer::lowerIfStmt(const ast::IfStmt *s,
                          std::vector<const L1Inst *> &out, TypeId returnType,
                          bool &terminated) {
  L1Operand cond{};
  if (!lowerExpr(s->cond, types_->builtin("bool"), out, cond)) return false;

  // 循环体里的语句 if：若某一支给循环参数赋了值，那个新值必须合并出来
  // （分支内定义的值在外层看不见），办法是让 #if 带一个结果。
  std::string mergeParam;
  const L1Type *mergeTy = nullptr;
  for (const auto &p : loopParams_) {
    if (assignsNameInBody(s->thenBody, p.first) ||
        (s->hasElse && assignsNameInBody(s->elseBody, p.first))) {
      mergeParam = p.first;
      mergeTy = p.second;
      break; // #if 只有单个结果名，一次合并一个参数
    }
  }
  // if 之前的绑定快照：else 必须从这个状态开始降级。
  const auto bindingsAtEntry = bindings_;
  const L1Operand entryVal = mergeTy ? bindings_[mergeParam] : L1Operand{};

  // 语句形态的 if 不产出值：两支各自是一个不带 results 的区域（需要合并时除外）。
  std::vector<const L1Inst *> thenInsts;
  bool thenTerminated = false;
  if (!lowerBody(s->thenBody, thenInsts, returnType, thenTerminated)) return false;
  // 必须在降级 else 之前捕获：之后 bindings_ 会被 else 分支覆盖。
  const L1Operand thenVal = mergeTy ? bindings_[mergeParam] : L1Operand{};

  std::vector<const L1Inst *> elseInsts;
  bool elseTerminated = false;
  // else 从「if 之前」的状态开始：否则会读到 then 分支里定义的临时值，
  // 而那个值按区域可见性在 else 里根本不存在（验证器报 2002）。
  if (mergeTy) bindings_ = bindingsAtEntry;
  if (s->hasElse && !lowerBody(s->elseBody, elseInsts, returnType, elseTerminated))
    return false;
  const L1Operand elseVal =
      mergeTy ? (s->hasElse ? bindings_[mergeParam] : entryVal) : L1Operand{};
  if (mergeTy) bindings_ = bindingsAtEntry;

  if (mergeTy) {
    // 已终结的分支不必 yield：break/continue/return 在验证器眼里等同于这条路走完了。
    if (!thenTerminated) {
      L1Operand y[1] = {thenVal};
      thenInsts.push_back(lainir_inst_jump(builder_, INST_YIELD, nullptr, y, 1));
    }
    if (!elseTerminated) {
      L1Operand y[1] = {elseVal};
      elseInsts.push_back(lainir_inst_jump(builder_, INST_YIELD, nullptr, y, 1));
    }
  }

  const L1Type *const results[1] = {mergeTy};
  const L1Type *const *resPtr = mergeTy ? results : nullptr;
  const uint32_t resCount = mergeTy ? 1u : 0u;

  const L1Region *thenRegion = lainir_region(
      builder_, nullptr, 0, resPtr, resCount,
      thenInsts.empty() ? nullptr : thenInsts.data(),
      static_cast<uint32_t>(thenInsts.size()));
  const L1Region *elseRegion =
      (s->hasElse || mergeTy)
          ? lainir_region(builder_, nullptr, 0, resPtr, resCount,
                          elseInsts.empty() ? nullptr : elseInsts.data(),
                          static_cast<uint32_t>(elseInsts.size()))
          : nullptr;

  if (mergeTy) {
    const char *res = nextTemp();
    out.push_back(lainir_inst_if(builder_, res, cond, thenRegion, elseRegion));
    bindings_[mergeParam] = lainir_ref(res); // 之后读到的就是合并值
  } else {
    out.push_back(lainir_inst_if(builder_, nullptr, cond, thenRegion, elseRegion));
  }

  // 只有两支都终结，这个 if 之后才算不可达。
  terminated = thenTerminated && s->hasElse && elseTerminated;
  return true;
}

bool Lowerer::lowerLoopStmt(const ast::LoopStmt *s,
                            std::vector<const L1Inst *> &out,
                            TypeId returnType) {
  // 循环参数既是区域入口参数，也是回边携带的状态。
  std::vector<L1RegionParam> params;
  std::vector<const L1Type *> resultTypes;
  params.reserve(s->params.size());

  for (const ast::LoopParam &p : s->params) {
    TypeId ty = types_->resolve(p.type, *diags_);
    if (ty == kInvalidType) return false;
    L1Operand init{};
    std::vector<const L1Inst *> initOut;
    if (p.init) {
      if (!lowerExpr(p.init, ty, initOut, init)) return false;
    } else {
      init = lainir_int(0);
    }
    if (!initOut.empty()) {
      fail(p.span, D::LowerUnsupported, "循环参数的初值必须是常量或已定义的值");
      return false;
    }
    params.push_back(lainir_param(intern(p.name), types_->lain(ty), init));
    bindings_[std::string(p.name)] = lainir_ref(intern(p.name));
  }

  std::vector<const L1Inst *> bodyInsts;
  if (s->cond) {
    L1Operand cond{};
    if (!lowerExpr(s->cond, types_->builtin("bool"), bodyInsts, cond)) return false;
    // 条件为真才继续：用带值 if，真分支落空（顺序继续循环体），假分支 break。
    const L1Region *empty = lainir_region(builder_, nullptr, 0, nullptr, 0, nullptr, 0);
    std::vector<const L1Inst *> breakInsts;
    breakInsts.push_back(lainir_inst_jump(builder_, INST_BREAK,
                                          s->hasLabel ? intern(s->label) : nullptr,
                                          nullptr, 0));
    const L1Region *breakRegion = lainir_region(
        builder_, nullptr, 0, nullptr, 0, breakInsts.data(),
        static_cast<uint32_t>(breakInsts.size()));
    bodyInsts.push_back(lainir_inst_if(builder_, nullptr, cond, empty, breakRegion));
  }

  bool bodyTerminated = false;
  if (!lowerBody(s->body, bodyInsts, returnType, bodyTerminated)) return false;

  // 值型循环需要在区域末尾留下一个 #break（结果类型取第一个参数）。
  if (!params.empty()) {
    const L1Type *resultTy = params.front().param.ty;
    const L1Type *const results[1] = {resultTy};
    L1Operand breakArg[1] = {lainir_ref(params.front().param.name)};
    bodyInsts.push_back(lainir_inst_jump(
        builder_, INST_BREAK, s->hasLabel ? intern(s->label) : nullptr, breakArg, 1));
    const L1Region *region = lainir_region(
        builder_, params.empty() ? nullptr : params.data(),
        static_cast<uint32_t>(params.size()), results, 1,
        bodyInsts.empty() ? nullptr : bodyInsts.data(),
        static_cast<uint32_t>(bodyInsts.size()));
    const char *res = nextTemp();
    out.push_back(lainir_inst_loop(builder_, res,
                                   s->hasLabel ? intern(s->label) : nullptr, region));
    bindings_[res] = lainir_ref(res);
    (void)resultTypes;
    return true;
  }

  const L1Region *region = lainir_region(
      builder_, nullptr, 0, nullptr, 0,
      bodyInsts.empty() ? nullptr : bodyInsts.data(),
      static_cast<uint32_t>(bodyInsts.size()));
  out.push_back(lainir_inst_loop(builder_, nullptr,
                                 s->hasLabel ? intern(s->label) : nullptr, region));
  return true;
}

bool Lowerer::lowerForStmt(const ast::ForStmt *s,
                           std::vector<const L1Inst *> &out, TypeId returnType) {
  // for i in from..to acc = init { … }
  // 降级成等价的 loop：区域参数是 (i, acc)，条件取反后 break，末尾 continue。
  TypeId varTy = s->hasVarType ? types_->resolve(s->varType, *diags_)
                               : types_->builtin("i32");
  if (varTy == kInvalidType) return false;
  TypeId accTy = s->hasAcc ? types_->resolve(s->accType, *diags_) : varTy;

  std::vector<L1RegionParam> params;
  L1Operand fromInit{};
  std::vector<const L1Inst *> pre;
  if (s->from) {
    if (!lowerExpr(s->from, varTy, pre, fromInit)) return false;
  } else {
    fromInit = lainir_int(0);
  }
  if (!pre.empty()) {
    fail(s->span, D::LowerUnsupported, "for 的下界必须是常量或已定义的值");
    return false;
  }
  params.push_back(lainir_param(intern(s->var), types_->lain(varTy), fromInit));

  if (s->hasAcc) {
    L1Operand accInit{};
    std::vector<const L1Inst *> preAcc;
    if (s->accInit && !lowerExpr(s->accInit, accTy, preAcc, accInit)) return false;
    if (!preAcc.empty()) {
      fail(s->span, D::LowerUnsupported, "for 的累加器初值必须是常量或已定义的值");
      return false;
    }
    if (!s->accInit) accInit = lainir_int(0);
    params.push_back(lainir_param(intern(s->acc), types_->lain(accTy), accInit));
  }

  std::vector<const L1Inst *> bodyInsts;
  // 条件：!(i < to) -> break。
  if (s->to) {
    L1Operand toOp{};
    if (!lowerExpr(s->to, varTy, bodyInsts, toOp)) return false;
    L1Operand cmpOps[2] = {lainir_ref(intern(s->var)), toOp};
    OperatorChoice choice{};
    if (!selectBinary(ast::BinOp::Lt, varTy, *types_, choice)) {
      fail(s->span, D::SemaNoSuchOperator, "for 的边界比较没有可用算子");
      return false;
    }
    const char *cmp = nextTemp();
    bodyInsts.push_back(lainir_inst(builder_, choice.kind, cmp, choice.operandTy,
                                    cmpOps, 2));
    const L1Region *empty = lainir_region(builder_, nullptr, 0, nullptr, 0, nullptr, 0);
    std::vector<const L1Inst *> breakInsts;
    breakInsts.push_back(lainir_inst_jump(builder_, INST_BREAK, nullptr, nullptr, 0));
    const L1Region *breakRegion = lainir_region(
        builder_, nullptr, 0, nullptr, 0, breakInsts.data(),
        static_cast<uint32_t>(breakInsts.size()));
    bodyInsts.push_back(lainir_inst_if(builder_, nullptr, lainir_ref(cmp), empty,
                                       breakRegion));
  }

  bool bodyTerminated = false;
  (void)bodyTerminated;
  for (const ast::Stmt *st : s->body) {
    bool term = false;
    if (!lowerStmt(st, bodyInsts, returnType, term)) return false;
  }

  // 末尾：i 自增后 continue。
  OperatorChoice incChoice{};
  if (selectBinary(ast::BinOp::Add, varTy, *types_, incChoice)) {
    L1Operand incOps[2] = {lainir_ref(intern(s->var)), lainir_int(1)};
    const char *next = nextTemp();
    bodyInsts.push_back(lainir_inst(builder_, incChoice.kind, next,
                                    incChoice.operandTy, incOps, 2));
    L1Operand contArgs[2] = {lainir_ref(next),
                             s->hasAcc ? lainir_ref(intern(s->acc)) : lainir_int(0)};
    bodyInsts.push_back(lainir_inst_jump(builder_, INST_CONTINUE, nullptr, contArgs,
                                         s->hasAcc ? 2u : 1u));
  }

  const L1Type *const results[1] = {types_->lain(s->hasAcc ? accTy : varTy)};
  const L1Region *region = lainir_region(
      builder_, params.data(), static_cast<uint32_t>(params.size()), results, 1,
      bodyInsts.empty() ? nullptr : bodyInsts.data(),
      static_cast<uint32_t>(bodyInsts.size()));
  const char *res = nextTemp();
  out.push_back(lainir_inst_loop(builder_, res, nullptr, region));
  return true;
}

bool Lowerer::lowerJumpStmt(const ast::JumpStmt *s,
                            std::vector<const L1Inst *> &out) {
  std::vector<L1Operand> args;
  args.reserve(s->args.size());
  for (ast::Expr *a : s->args) {
    L1Operand op{};
    if (!lowerExpr(a, kInvalidType, out, op)) return false;
    args.push_back(op);
  }
  if (s->isBreak && args.size() != 1) {
    fail(s->span, D::SemaTypeMismatch, "break 必须恰好带一个值");
    return false;
  }
  out.push_back(lainir_inst_jump(builder_, s->isBreak ? INST_BREAK : INST_CONTINUE,
                                 s->label.empty() ? nullptr : intern(s->label),
                                 args.empty() ? nullptr : args.data(),
                                 static_cast<uint32_t>(args.size())));
  return true;
}

} // namespace ln
