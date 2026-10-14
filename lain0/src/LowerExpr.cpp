// 表达式降级。
//
// 一条铁律：宽度只能来自 IR 自己（TypeStore::width / lainir_inst_result_type），
// 不许传递什么「期望宽度」——stage 0 那一整类 bug 就是那么来的。

#include <cstdio>
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
    // 顶层静态绑定（表 / 字符串）：它是 data 块 + `#proc NAME() -> #addr`，
    // 取值要调它的访问器过程，不能直接引用那个名字。
    if (sema_->topLevelTypes.count(std::string(n->name)) != 0) {
      const char *res = nextTemp();
      out.push_back(lainir_inst_call(builder_, res, intern(n->name), nullptr, 0));
      value = lainir_ref(res);
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
          n->name == "ptr_of" || n->name == "addr_of" || n->name == "len" ||
          n->name == "ref_of" || n->name == "mut_ref_of" ||
          n->name == "slice_of" || n->name == "symbol_addr") {
        return lowerBuiltin(c, n->name, expectedTy, out, value);
      }
    }
    // Shape.Circle(s) 这种「enum 变体投影」：callee 是限定名，不是过程。
    // 判别 tag，命中才读载荷，不命中回 0。
    if (c->callee && c->callee->kind == ast::ExprKind::Dotted) {
      const auto *d = static_cast<const ast::DottedExpr *>(c->callee);
      if (d->object && d->object->kind == ast::ExprKind::Name) {
        const auto *en = static_cast<const ast::NameExpr *>(d->object);
        const TypeId enumTy = types_->lookup(en->name);
        const AggregateLayout *elay =
            enumTy != kInvalidType ? types_->layout(enumTy) : nullptr;
        if (elay != nullptr && !elay->variants.empty())
          return lowerProjection(c, d->member, elay, expectedTy, out, value);
      }
    }
    return lowerCall(c, expectedTy, out, value);
  }
  case ast::ExprKind::Loop:
    return lowerLoopExpr(static_cast<const ast::LoopExpr *>(e), expectedTy, out,
                         value);
  case ast::ExprKind::Construct:
    return lowerConstructExpr(static_cast<const ast::ConstructExpr *>(e),
                              expectedTy, out, value);
  case ast::ExprKind::Dotted:
    return lowerDottedExpr(static_cast<const ast::DottedExpr *>(e), expectedTy,
                           out, value);
  case ast::ExprKind::Index:
    return lowerIndexExpr(static_cast<const ast::IndexExpr *>(e), expectedTy,
                          out, value);
  case ast::ExprKind::ValueIf:
    return lowerValueIfExpr(static_cast<const ast::ValueIfExpr *>(e), expectedTy,
                            out, value);
  case ast::ExprKind::For:
    return lowerForExpr(static_cast<const ast::ForExpr *>(e), expectedTy, out,
                        value);
  case ast::ExprKind::StrLit: {
    const auto *sl = static_cast<const ast::StrLitExpr *>(e);
    std::vector<uint8_t> bytes(sl->rawText.begin(), sl->rawText.end());
    // 内联字符串字面量：在模块级发一个只读 data 块 + 取址过程，调用它拿 #addr。
    // （顶层 `let x = "..."` 走 lowerStaticLet 的同款机制；这里是表达式位置的内联形态。）
    const std::string storageName =
        std::string("strlit_") + std::to_string(strlitCounter_++);
    const char *symbol = intern(storageName);
    const char *procName = intern(storageName);  // 取址过程名 = 存储名
    const L1Data *block = lainir_data(
        builder_, symbol, bytes.empty() ? nullptr : bytes.data(),
        static_cast<uint32_t>(bytes.size()), false);
    if (block == nullptr) {
      fail(e->span, D::LowerUnsupported, "字符串常量块构造失败");
      return false;
    }
    data_.push_back(*block);
    const L1Subroutine *accessor =
        buildAccessor(builder_, procName, symbol, intern("base"), {},
                     lineOf(e->span), columnOf(e->span));
    if (accessor == nullptr) {
      fail(e->span, D::LowerUnsupported, "字符串取址过程构造失败");
      return false;
    }
    subs_.push_back(*accessor);
    const char *res = nextTemp();
    out.push_back(lainir_inst_call(builder_, res, procName, nullptr, 0));
    value = lainir_ref(res);
    return true;
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

bool Lowerer::lowerProjection(const ast::CallExpr *e,
                              std::string_view variantName,
                              const AggregateLayout *lay, TypeId expectedTy,
                              std::vector<const L1Inst *> &out,
                              L1Operand &value) {
  const VariantLayout *vl = nullptr;
  for (const VariantLayout &v : lay->variants) {
    if (v.name == variantName) {
      vl = &v;
      break;
    }
  }
  if (vl == nullptr) {
    fail(e->span, D::LowerUnsupported,
         std::string("enum 没有变体 ") + std::string(variantName));
    return false;
  }
  if (e->args.size() != 1 || e->args[0] == nullptr) {
    fail(e->span, D::LowerUnsupported, "投影需要一个实参（被判别的值）");
    return false;
  }

  const TypeId payloadTy =
      vl->payload != kInvalidType ? vl->payload : expectedTy;
  if (payloadTy == kInvalidType) {
    fail(e->span, D::SemaWidthUnknown, "投影的载荷类型取不到");
    return false;
  }
  const L1Type *resTy = types_->lain(payloadTy);
  const L1Type *const results[1] = {resTy};
  const L1Type *addrTy = types_->lain(types_->builtin("addr"));
  const L1Type *tagTy = types_->lain(types_->builtin("u8"));

  // 被判别的对象：enum 按地址传
  L1Operand obj{};
  if (!lowerExpr(e->args[0], types_->builtin("addr"), out, obj)) return false;

  // tag = #load[#bits<8>](%obj)（tag 在 offset 0）
  L1Operand objOp = obj;
  const char *tagName = nextTemp();
  out.push_back(lainir_inst_mem(builder_, INST_LOAD, tagName, tagTy,
                                ORDER_RELAXED, false, &objOp, 1));

  // hit = tag == 变体的 tag（tag 是 #bits<8>，比较指令的 ty 取操作数宽度）
  L1Operand cmpOps[2] = {lainir_ref(tagName), lainir_int(vl->tag)};
  const char *hitName = nextTemp();
  out.push_back(lainir_inst(builder_, INST_EQ, hitName, tagTy, cmpOps, 2));

  // 命中：读载荷；不命中：0
  std::vector<const L1Inst *> thenInsts;
  if (vl->payload != kInvalidType) {
    L1Operand leaOps[4] = {obj, lainir_int(0), lainir_int(1),
                           lainir_int(vl->payloadOffset)};
    const char *payloadAddr = nextTemp();
    thenInsts.push_back(
        lainir_inst(builder_, INST_LEA, payloadAddr, addrTy, leaOps, 4));
    L1Operand ptr = lainir_ref(payloadAddr);
    const char *pv = nextTemp();
    thenInsts.push_back(lainir_inst_mem(builder_, INST_LOAD, pv, resTy,
                                        ORDER_RELAXED, false, &ptr, 1));
    L1Operand y[1] = {lainir_ref(pv)};
    thenInsts.push_back(lainir_inst_jump(builder_, INST_YIELD, nullptr, y, 1));
  } else {
    L1Operand y[1] = {lainir_int(0)};
    thenInsts.push_back(lainir_inst_jump(builder_, INST_YIELD, nullptr, y, 1));
  }

  std::vector<const L1Inst *> elseInsts;
  L1Operand zero[1] = {lainir_int(0)};
  elseInsts.push_back(lainir_inst_jump(builder_, INST_YIELD, nullptr, zero, 1));

  const L1Region *thenRegion =
      lainir_region(builder_, nullptr, 0, results, 1, thenInsts.data(),
                    static_cast<uint32_t>(thenInsts.size()));
  const L1Region *elseRegion =
      lainir_region(builder_, nullptr, 0, results, 1, elseInsts.data(),
                    static_cast<uint32_t>(elseInsts.size()));

  const char *res = nextTemp();
  out.push_back(lainir_inst_if(builder_, res, lainir_ref(hitName), thenRegion,
                               elseRegion));
  value = lainir_ref(res);
  return true;
}

bool Lowerer::lowerConstructExpr(const ast::ConstructExpr *e, TypeId expectedTy,
                                 std::vector<const L1Inst *> &out,
                                 L1Operand &value) {
  // --- 变体构造：Shape.Circle { 载荷 } ---
  // 布局是「tag 在前（1 字节，offset 0），载荷在 payloadOffset」。
  if (e->target.hasQualifier) {
    const TypeId enumTy = types_->resolve(e->target, *diags_);
    if (enumTy == kInvalidType) {
      fail(e->span, D::LowerUnsupported, "变体构造的 enum 类型取不到");
      return false;
    }
    const AggregateLayout *elay = types_->layout(enumTy);
    if (elay == nullptr) {
      fail(e->span, D::LowerUnsupported, "变体构造的目标不是 enum");
      return false;
    }
    const VariantLayout *vl = nullptr;
    for (const VariantLayout &v : elay->variants) {
      if (v.name == e->target.name) {
        vl = &v;
        break;
      }
    }
    if (vl == nullptr) {
      fail(e->span, D::LowerUnsupported,
           std::string("enum 没有变体 ") + std::string(e->target.name));
      return false;
    }

    const L1Type *addrTy = types_->lain(types_->builtin("addr"));
    const L1Type *tagTy = types_->lain(types_->builtin("u8"));
    L1Operand count = lainir_int(elay->size);
    const char *objName = nextTemp();
    out.push_back(lainir_inst_mem(builder_, INST_ALLOCA, objName, tagTy,
                                  ORDER_RELAXED, false, &count, 1));

    // tag：store 到 offset 0（#store 是「值, 地址」）
    L1Operand tagOps[2] = {lainir_int(vl->tag), lainir_ref(objName)};
    out.push_back(lainir_inst_mem(builder_, INST_STORE, nullptr, tagTy,
                                  ORDER_RELAXED, false, tagOps, 2));

    if (vl->payload != kInvalidType && !e->fields.empty()) {
      L1Operand leaOps[4] = {lainir_ref(objName), lainir_int(0), lainir_int(1),
                             lainir_int(vl->payloadOffset)};
      const char *payloadAddr = nextTemp();
      out.push_back(
          lainir_inst(builder_, INST_LEA, payloadAddr, addrTy, leaOps, 4));
      L1Operand val{};
      if (!lowerExpr(e->fields[0].value, vl->payload, out, val)) return false;
      L1Operand storeOps[2] = {val, lainir_ref(payloadAddr)};
      out.push_back(lainir_inst_mem(builder_, INST_STORE, nullptr,
                                    types_->lain(vl->payload), ORDER_RELAXED,
                                    false, storeOps, 2));
    }

    value = lainir_ref(objName);
    return true;
  }

  TypeId structTy = expectedTy;
  if (structTy == kInvalidType)
    structTy = types_->resolve(e->target, *diags_);
  if (structTy == kInvalidType) {
    fail(e->span, D::LowerUnsupported, "构造的目标类型取不到");
    return false;
  }
  const AggregateLayout *lay = types_->layout(structTy);
  if (lay == nullptr) {
    fail(e->span, D::LowerUnsupported, "构造的目标不是聚合");
    return false;
  }

  // 函数体里的构造从栈上划一块：#alloca[#bits<8>](size)
  // （docs/04 —— 构造出来的地址只在本帧内有效）
  const L1Type *sizeTy = types_->lain(types_->builtin("u8"));
  L1Operand count = lainir_int(lay->size);
  const char *objName = nextTemp();
  out.push_back(lainir_inst_mem(builder_, INST_ALLOCA, objName, sizeTy,
                                ORDER_RELAXED, false, &count, 1));

  const L1Type *addrTy = types_->lain(types_->builtin("addr"));
  for (const ast::FieldInit &f : e->fields) {
    uint32_t offset = 0;
    TypeId fieldTy = kInvalidType;
    for (const FieldLayout &fl : lay->fields) {
      if (fl.name == f.name) {
        offset = fl.offset;
        fieldTy = fl.type;
        break;
      }
    }
    if (fieldTy == kInvalidType) {
      fail(f.span, D::LowerUnsupported,
           std::string("聚合没有成员 ") + std::string(f.name));
      return false;
    }
    L1Operand leaOps[4] = {lainir_ref(objName), lainir_int(0), lainir_int(1),
                           lainir_int(offset)};
    const char *fieldAddr = nextTemp();
    out.push_back(lainir_inst(builder_, INST_LEA, fieldAddr, addrTy, leaOps, 4));

    L1Operand val{};
    if (!lowerExpr(f.value, fieldTy, out, val)) return false;
    // #store takes value, destination —— 值在前、地址在后（顺序反了验证器报 2005）
    L1Operand storeOps[2] = {val, lainir_ref(fieldAddr)};
    out.push_back(lainir_inst_mem(builder_, INST_STORE, nullptr,
                                  types_->lain(fieldTy), ORDER_RELAXED, false,
                                  storeOps, 2));
  }

  value = lainir_ref(objName);
  return true;
}

bool Lowerer::lowerDottedExpr(const ast::DottedExpr *e, TypeId expectedTy,
                              std::vector<const L1Inst *> &out,
                              L1Operand &value) {
  // 模块成员值访问：M.answer（object 是模块别名，按 <alias>.<member> 查 moduleMembers）。
  // 降级成对被导入模块访问器过程的调用：#call @answer()。
  if (e->object && e->object->kind == ast::ExprKind::Name) {
    const auto *en = static_cast<const ast::NameExpr *>(e->object);
    auto memIt = sema_->moduleMembers.find(std::string(en->name) + "." +
                                           std::string(e->member));
    if (memIt != sema_->moduleMembers.end() &&
        memIt->second.kind == ModuleMember::Kind::Value) {
      const char *res = nextTemp();
      out.push_back(lainir_inst_call(builder_, res,
                                     intern(memIt->second.mangled.c_str()), nullptr, 0));
      value = lainir_ref(res);
      return true;
    }
  }
  // 聚合按地址传（docs/04：「聚合是参数，地址由调用方给」），
  // 所以字段 = load(obj + 字段偏移)。
  const TypeId objTy = typeOfExpr(*sema_, e->object, kInvalidType);
  const AggregateLayout *lay =
      objTy != kInvalidType ? types_->layout(objTy) : nullptr;
  if (lay == nullptr) {
    fail(e->span, D::LowerUnsupported, "字段访问的对象不是聚合");
    return false;
  }
  uint32_t offset = 0;
  TypeId fieldTy = kInvalidType;
  for (const FieldLayout &f : lay->fields) {
    if (f.name == e->member) {
      offset = f.offset;
      fieldTy = f.type;
      break;
    }
  }
  if (fieldTy == kInvalidType) {
    fail(e->span, D::LowerUnsupported,
         std::string("聚合没有成员 ") + std::string(e->member));
    return false;
  }

  L1Operand obj{};
  if (!lowerExpr(e->object, types_->builtin("addr"), out, obj)) return false;

  const L1Type *addrTy = types_->lain(types_->builtin("addr"));
  L1Operand leaOps[4] = {obj, lainir_int(0), lainir_int(1), lainir_int(offset)};
  const char *addrName = nextTemp();
  out.push_back(lainir_inst(builder_, INST_LEA, addrName, addrTy, leaOps, 4));

  L1Operand ptr = lainir_ref(addrName);
  const char *res = nextTemp();
  out.push_back(lainir_inst_mem(builder_, INST_LOAD, res,
                                types_->lain(fieldTy), ORDER_RELAXED, false,
                                &ptr, 1));
  value = lainir_ref(res);
  return true;
}

bool Lowerer::lowerIndexExpr(const ast::IndexExpr *e, TypeId expectedTy,
                             std::vector<const L1Inst *> &out,
                             L1Operand &value) {
  // 表元素取址：#lea(%base, %index, 元素字节数, 0)
  L1Operand base{};
  if (!lowerExpr(e->base, types_->builtin("addr"), out, base)) return false;
  L1Operand index{};
  if (!lowerExpr(e->index, types_->builtin("u64"), out, index)) return false;

  // 元素字节数：base 是顶层表时，从它的数组类型取元素宽度。
  // 样例里 [u8] 与 [u64] 并存，scale 取错地址就全错。
  uint64_t scale = 1;
  if (e->base && e->base->kind == ast::ExprKind::Name) {
    const auto *n = static_cast<const ast::NameExpr *>(e->base);
    auto it = sema_->topLevelTypes.find(std::string(n->name));
    if (it != sema_->topLevelTypes.end()) {
      const TypeId elem = types_->elementOf(it->second);
      if (elem != kInvalidType) {
        const uint32_t w = types_->width(elem);
        if (w >= 8) scale = w / 8u;
      }
    }
  }

  const L1Type *addrTy = types_->lain(types_->builtin("addr"));
  L1Operand ops[4] = {base, index, lainir_int(scale), lainir_int(0)};
  const char *res = nextTemp();
  out.push_back(lainir_inst(builder_, INST_LEA, res, addrTy, ops, 4));
  value = lainir_ref(res);
  return true;
}

bool Lowerer::lowerValueIfExpr(const ast::ValueIfExpr *e, TypeId expectedTy,
                               std::vector<const L1Inst *> &out,
                               L1Operand &value) {
  if (expectedTy == kInvalidType) {
    fail(e->span, D::SemaWidthUnknown,
         "带值 if 的类型必须由期望类型给出（let x: T = if …）");
    return false;
  }
  const L1Type *resultTy = types_->lain(expectedTy);
  const L1Type *const results[1] = {resultTy};

  L1Operand cond{};
  if (!lowerExpr(e->cond, types_->builtin("bool"), out, cond)) return false;

  // 分支体：逐条降级；最后一条若是裸表达式，它的值就是这一支的结果，
  // 用 #yield 带出（区域声明了 1 个结果，每条路径都必须 yield）。
  auto lowerBranch = [&](const std::vector<ast::Stmt *> &body,
                         std::vector<const L1Inst *> &insts) -> bool {
    for (size_t i = 0; i < body.size(); ++i) {
      ast::Stmt *s = body[i];
      if (!s) continue;
      const bool last = (i + 1 == body.size());
      if (last && s->kind == ast::StmtKind::ExprStmt) {
        const auto *es = static_cast<const ast::ExprStmt *>(s);
        L1Operand v{};
        if (!lowerExpr(es->expr, expectedTy, insts, v)) return false;
        L1Operand y[1] = {v};
        insts.push_back(lainir_inst_jump(builder_, INST_YIELD, nullptr, y, 1));
        return true;
      }
      bool term = false;
      if (!lowerStmt(s, insts, expectedTy, term)) return false;
    }
    // 分支里没有可用的值：补一个该类型的零值。
    L1Operand z[1] = {lainir_int(0)};
    insts.push_back(lainir_inst_jump(builder_, INST_YIELD, nullptr, z, 1));
    return true;
  };

  std::vector<const L1Inst *> thenInsts;
  std::vector<const L1Inst *> elseInsts;
  if (!lowerBranch(e->thenBody, thenInsts)) return false;
  if (e->hasElse) {
    if (!lowerBranch(e->elseBody, elseInsts)) return false;
  } else {
    L1Operand z[1] = {lainir_int(0)};
    elseInsts.push_back(lainir_inst_jump(builder_, INST_YIELD, nullptr, z, 1));
  }

  const L1Region *thenRegion = lainir_region(
      builder_, nullptr, 0, results, 1,
      thenInsts.empty() ? nullptr : thenInsts.data(),
      static_cast<uint32_t>(thenInsts.size()));
  const L1Region *elseRegion = lainir_region(
      builder_, nullptr, 0, results, 1,
      elseInsts.empty() ? nullptr : elseInsts.data(),
      static_cast<uint32_t>(elseInsts.size()));

  const char *res = nextTemp();
  out.push_back(lainir_inst_if(builder_, res, cond, thenRegion, elseRegion));
  value = lainir_ref(res);
  return true;
}

bool Lowerer::lowerForExpr(const ast::ForExpr *e, TypeId expectedTy,
                           std::vector<const L1Inst *> &out, L1Operand &value) {
  if (expectedTy == kInvalidType) {
    fail(e->span, D::SemaWidthUnknown,
         "for 的值类型必须由期望类型给出（let x: T = for …）");
    return false;
  }
  const L1Type *resultTy = types_->lain(expectedTy);

  // 循环变量的类型：显式标注优先，否则从 from 推导。
  TypeId varTy = kInvalidType;
  if (e->hasVarType) varTy = types_->resolve(e->varType, *diags_);
  if (varTy == kInvalidType && e->from)
    varTy = typeOfExpr(*sema_, e->from, kInvalidType);
  if (varTy == kInvalidType) {
    fail(e->span, D::SemaWidthUnknown, "for 的循环变量类型无法推导");
    return false;
  }
  const L1Type *varL1 = types_->lain(varTy);

  std::vector<L1RegionParam> params;
  std::vector<L1Operand> paramRefs;

  L1Operand fromInit{};
  if (e->from) {
    if (!lowerExpr(e->from, varTy, out, fromInit)) return false;
  } else {
    fromInit = lainir_int(0);
  }
  const char *iname = intern(e->var);
  params.push_back(lainir_param(iname, varL1, fromInit));
  paramRefs.push_back(lainir_ref(iname));
  bindings_[std::string(e->var)] = lainir_ref(iname);

  // 累加器（可省）：没有它时结果取循环变量，靠源码自己的 break 带出。
  const char *aname = nullptr;
  if (e->hasAcc) {
    TypeId accTy = kInvalidType;
    if (!e->accType.name.empty()) accTy = types_->resolve(e->accType, *diags_);
    if (accTy == kInvalidType && e->accInit)
      accTy = typeOfExpr(*sema_, e->accInit, kInvalidType);
    if (accTy == kInvalidType) accTy = expectedTy;
    L1Operand accInit{};
    if (e->accInit) {
      if (!lowerExpr(e->accInit, accTy, out, accInit)) return false;
    } else {
      accInit = lainir_int(0);
    }
    aname = intern(e->acc);
    params.push_back(lainir_param(aname, types_->lain(accTy), accInit));
    paramRefs.push_back(lainir_ref(aname));
    bindings_[std::string(e->acc)] = lainir_ref(aname);
  }
  // 登记当前循环参数：循环体里的语句 if 靠它判断要不要做分支合并。
  const std::vector<std::pair<std::string, const L1Type *>> savedLoopParams =
      loopParams_;
  loopParams_.clear();
  for (const auto &pr : params)
    loopParams_.emplace_back(pr.param.name, pr.param.ty);

  // #loop 必须带标签；for 语法没有标签，生成一个内部唯一名。
  const std::string labelText = "L" + std::string(nextTemp());
  const char *label = intern(labelText);

  std::vector<const L1Inst *> bodyInsts;

  // 上界在循环外求值；条件是 i < to。
  L1Operand toOp{};
  if (!lowerExpr(e->to, varTy, out, toOp)) return false;

  OperatorChoice cmpChoice{};
  if (!selectBinary(ast::BinOp::Lt, varTy, *types_, cmpChoice)) {
    fail(e->span, D::LowerUnsupported, "for 的条件运算符不受支持");
    return false;
  }
  L1Operand cmpOps[2] = {paramRefs[0], toOp};
  const char *cname = nextTemp();
  bodyInsts.push_back(lainir_inst(builder_, cmpChoice.kind, cname,
                                  cmpChoice.operandTy, cmpOps, 2));
  L1Operand cond = lainir_ref(cname);

  std::vector<const L1Inst *> inner;
  bool term = false;
  if (!lowerBody(e->body, inner, expectedTy, term)) return false;

  // 步进：i + 1
  OperatorChoice incChoice{};
  if (!selectBinary(ast::BinOp::Add, varTy, *types_, incChoice)) {
    fail(e->span, D::LowerUnsupported, "for 的步进运算符不受支持");
    return false;
  }
  L1Operand incOps[2] = {bindings_[std::string(e->var)], lainir_int(1)};
  const char *stepName = nextTemp();
  inner.push_back(lainir_inst(builder_, incChoice.kind, stepName,
                              incChoice.operandTy, incOps, 2));

  // 体已经以 break/continue 结束时不再合成 continue（终止指令后不能再有指令）。
  if (!term) {
    std::vector<L1Operand> contArgs;
    contArgs.push_back(lainir_ref(stepName));
    if (aname) contArgs.push_back(bindings_[std::string(e->acc)]);
    inner.push_back(lainir_inst_jump(builder_, INST_CONTINUE, label,
                                     contArgs.data(),
                                     static_cast<uint32_t>(contArgs.size())));
  }

  const L1Region *thenRegion =
      lainir_region(builder_, nullptr, 0, nullptr, 0,
                    inner.empty() ? nullptr : inner.data(),
                    static_cast<uint32_t>(inner.size()));
  bodyInsts.push_back(lainir_inst_if(builder_, nullptr, cond, thenRegion, nullptr));

  // 收尾：#break(结果参数)。必须用入口参数本身——body 更新后的临时值定义在
  // then 区域内部，在循环体这一层是看不见的（loop 同理）。
  L1Operand breakArg[1] = {paramRefs.back()};
  bodyInsts.push_back(lainir_inst_jump(builder_, INST_BREAK, label, breakArg, 1));

  const L1Type *const results[1] = {resultTy};
  const L1Region *region = lainir_region(
      builder_, params.data(), static_cast<uint32_t>(params.size()), results, 1,
      bodyInsts.data(), static_cast<uint32_t>(bodyInsts.size()));

  const char *res = nextTemp();
  out.push_back(lainir_inst_loop(builder_, res, label, region));
  loopParams_ = savedLoopParams;
  value = lainir_ref(res);
  return true;
}

bool Lowerer::lowerCall(const ast::CallExpr *e, TypeId expectedTy,
                        std::vector<const L1Inst *> &out, L1Operand &value,
                        bool tail) {
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

  // 聚合（struct/enum）按地址返回：lain() 对它返回 nullptr，这里必须退回
  // #addr，否则 #call 就没有结果（验证器报 2020 defines 0 result(s)）。
  // 与 signatureTypeOf 同一条规则。
  const L1Type *retTy = nullptr;
  if (expectedTy != kInvalidType) {
    retTy = types_->lain(expectedTy);
    if (retTy == nullptr) retTy = lainir_type(builder_, TY_ADDR, 0);
  }
  const char *res = retTy ? nextTemp() : nullptr;
  const L1Inst *call =
      tail ? lainir_inst_call_ex(builder_, res, intern(n->name),
                                 args.empty() ? nullptr : args.data(),
                                 static_cast<uint32_t>(args.size()), true)
           : lainir_inst_call(builder_, res, intern(n->name),
                              args.empty() ? nullptr : args.data(),
                              static_cast<uint32_t>(args.size()));
  out.push_back(call);
  value = res ? lainir_ref(res) : L1Operand{};
  return true;
}

bool Lowerer::lowerLoopExpr(const ast::LoopExpr *e, TypeId expectedTy,
                           std::vector<const L1Inst *> &out, L1Operand &value) {
  if (expectedTy == kInvalidType) {
    fail(e->span, D::SemaWidthUnknown,
         "loop 的值类型必须由期望类型给出（let x: T = loop …）");
    return false;
  }
  const L1Type *resultTy = types_->lain(expectedTy);

  std::vector<L1RegionParam> params;
  std::vector<L1Operand> paramRefs;
  params.reserve(e->params.size());
  paramRefs.reserve(e->params.size());
  for (const ast::LoopParam &p : e->params) {
    TypeId ty = types_->resolve(p.type, *diags_);
    if (ty == kInvalidType) return false;
    L1Operand init{};
    if (p.init) {
      if (!lowerExpr(p.init, ty, out, init)) return false;
    } else {
      init = lainir_int(0);
    }
    const char *pname = intern(p.name);
    params.push_back(lainir_param(pname, types_->lain(ty), init));
    paramRefs.push_back(lainir_ref(pname));
    bindings_[std::string(p.name)] = lainir_ref(pname);
  }
  // 登记当前循环参数：循环体里的语句 if 靠它判断要不要做分支合并。
  const std::vector<std::pair<std::string, const L1Type *>> savedLoopParams =
      loopParams_;
  loopParams_.clear();
  for (const auto &pr : params)
    loopParams_.emplace_back(pr.param.name, pr.param.ty);

  std::vector<const L1Inst *> bodyInsts;

  if (e->cond) {
    L1Operand cond{};
    if (!lowerExpr(e->cond, types_->builtin("bool"), bodyInsts, cond))
      return false;

    std::vector<const L1Inst *> inner;
    bool term = false;
    if (!lowerBody(e->body, inner, expectedTy, term)) return false;

    std::vector<const L1Inst *> thenInsts;
    for (auto *i : inner) thenInsts.push_back(i);
    // 体已经以 break/continue 结束时不再合成 continue：终止指令之后不能再有指令。
    if (!term) {
      std::vector<L1Operand> contArgs;
      contArgs.reserve(params.size());
      for (const auto &pr : params)
        contArgs.push_back(bindings_[std::string(pr.param.name)]);
      thenInsts.push_back(lainir_inst_jump(
          builder_, INST_CONTINUE, e->hasLabel ? intern(e->label) : nullptr,
          contArgs.data(), static_cast<uint32_t>(contArgs.size())));
    }

    const L1Region *thenRegion =
        lainir_region(builder_, nullptr, 0, nullptr, 0, thenInsts.data(),
                      static_cast<uint32_t>(thenInsts.size()));
    bodyInsts.push_back(lainir_inst_if(builder_, nullptr, cond, thenRegion, nullptr));
  } else {
    bool term = false;
    if (!lowerBody(e->body, bodyInsts, expectedTy, term)) return false;
  }

  // 值型循环末尾必须有一个 #break（结果取最后一个参数在循环入口的值）。
  if (!params.empty()) {
    L1Operand breakArg[1] = {paramRefs.back()};
    bodyInsts.push_back(lainir_inst_jump(builder_, INST_BREAK,
                                        e->hasLabel ? intern(e->label) : nullptr,
                                        breakArg, 1));
  }

  const L1Type *const results[1] = {resultTy};
  const L1Region *region = lainir_region(
      builder_, params.empty() ? nullptr : params.data(),
      static_cast<uint32_t>(params.size()), results, 1,
      bodyInsts.empty() ? nullptr : bodyInsts.data(),
      static_cast<uint32_t>(bodyInsts.size()));

  const char *res = nextTemp();
  out.push_back(lainir_inst_loop(builder_, res,
                                 e->hasLabel ? intern(e->label) : nullptr, region));
  loopParams_ = savedLoopParams;
  value = lainir_ref(res);
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
    out.push_back(lainir_inst(builder_, INST_BITCAST, res,
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
    out.push_back(lainir_inst(builder_, INST_BITCAST, res,
                              lainir_type(builder_, TY_ADDR, 0), &num, 1));
    value = lainir_ref(res);
    return true;
  }
  if (name == "symbol_addr") {
    if (e->args.size() != 1 || e->args[0] == nullptr ||
        e->args[0]->kind != ast::ExprKind::StrLit) {
      fail(e->span, D::SemaTypeMismatch,
           "symbol_addr 需要一个字符串字面量符号名");
      return false;
    }
    const auto *symbol = static_cast<const ast::StrLitExpr *>(e->args[0]);
    const char *res = nextTemp();
    L1Operand sym = lainir_ref(intern(symbol->rawText));
    out.push_back(lainir_inst(builder_, INST_BITCAST, res,
                              lainir_type(builder_, TY_ADDR, 0), &sym, 1));
    value = lainir_ref(res);
    return true;
  }
  if (name == "ref_of" || name == "mut_ref_of") {
    if (e->args.size() != 1) {
      fail(e->span, D::SemaTypeMismatch, "ref_of/mut_ref_of 需要一个实参");
      return false;
    }
    L1Operand val{};
    if (!lowerExpr(e->args[0], kInvalidType, out, val)) return false;
    // ref / mut ref 的 LAINIR 形态就是 #addr：把值 bitcast 成地址。
    const char *res = nextTemp();
    out.push_back(lainir_inst(builder_, INST_BITCAST, res,
                              lainir_type(builder_, TY_ADDR, 0), &val, 1));
    value = lainir_ref(res);
    return true;
  }
  if (name == "slice_of") {
    if (e->args.size() != 2) {
      fail(e->span, D::SemaTypeMismatch, "slice_of 需要两个实参 (base, len)");
      return false;
    }
    L1Operand base{}, len{};
    if (!lowerExpr(e->args[0], types_->builtin("addr"), out, base)) return false;
    if (!lowerExpr(e->args[1], types_->builtin("u64"), out, len)) return false;
    // 栈上划 16 字节：ptr@0（addr），len@8（u64）。
    const L1Type *sizeTy = types_->lain(types_->builtin("u8"));
    L1Operand count = lainir_int(16);
    const char *slot = nextTemp();
    out.push_back(lainir_inst_mem(builder_, INST_ALLOCA, slot, sizeTy,
                                  ORDER_RELAXED, false, &count, 1));
    const L1Type *addrTy = types_->lain(types_->builtin("addr"));
    // store base @0
    {
      L1Operand leaOps[4] = {lainir_ref(slot), lainir_int(0), lainir_int(1),
                             lainir_int(0)};
      const char *fa = nextTemp();
      out.push_back(lainir_inst(builder_, INST_LEA, fa, addrTy, leaOps, 4));
      L1Operand so[2] = {base, lainir_ref(fa)};
      out.push_back(lainir_inst_mem(builder_, INST_STORE, nullptr, addrTy,
                                    ORDER_RELAXED, false, so, 2));
    }
    // store len @8
    {
      L1Operand leaOps[4] = {lainir_ref(slot), lainir_int(0), lainir_int(1),
                             lainir_int(8)};
      const char *fa = nextTemp();
      out.push_back(lainir_inst(builder_, INST_LEA, fa, addrTy, leaOps, 4));
      L1Operand so[2] = {len, lainir_ref(fa)};
      out.push_back(lainir_inst_mem(builder_, INST_STORE, nullptr,
                                    types_->lain(types_->builtin("u64")),
                                    ORDER_RELAXED, false, so, 2));
    }
    value = lainir_ref(slot);
    return true;
  }
  if (name == "len") {
    // len(t) 发元素个数：顶层表的长度编译期已知，由 Sema 登记在这里。
    if (e->args.size() != 1 || e->args[0] == nullptr ||
        e->args[0]->kind != ast::ExprKind::Name) {
      fail(e->span, D::LowerUnsupported, "len 只作用于顶层的表/串名字");
      return false;
    }
    const auto *n = static_cast<const ast::NameExpr *>(e->args[0]);
    auto it = sema_->topLevelLength.find(std::string(n->name));
    if (it == sema_->topLevelLength.end()) {
      fail(e->span, D::LowerUnsupported,
           std::string("len 取不到 ") + std::string(n->name) + " 的长度");
      return false;
    }
    value = lainir_int(it->second);
    return true;
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

  // D-002：addr ↔ 整数统一走 #bitcast。不能按宽度判断——#addr 的宽度无意义
  // （为 0），会被当成「变宽」而错发成 #zext。
  const TypeId addrTy = types_->builtin("addr");
  if (src == addrTy || target == addrTy) {
    const char *res = nextTemp();
    out.push_back(lainir_inst(builder_, INST_BITCAST, res,
                              types_->lain(target), &operand, 1));
    value = lainir_ref(res);
    return true;
  }

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
