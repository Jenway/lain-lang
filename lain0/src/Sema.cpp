#include "lain0/Sema.hpp"

// 语义层实现。
//
// 这一层不产出 IR，只产出「降级需要的结论」：每个表达式的类型、聚合类型的布局、
// 以及给定操作数类型时该发哪一条 LAINIR 指令。
//
// 内建标量是固定的九种，所以它们的 L1Type 直接是静态对象，不需要 L1Builder。

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

#include "lain0/Ast.hpp"
#include "lain0/Diag.hpp"

namespace ln {
namespace {

// 顺序固定，索引即内建编号。
constexpr uint32_t kBuiltinCount = 9;

struct BuiltinDef {
  const char *name;
  L1TypeKind kind;
  uint32_t width;
  bool signedInt;
  bool isBool;
};

const BuiltinDef kBuiltinDefs[kBuiltinCount] = {
    {"i32", TY_BITS, 32, true, false},   {"u32", TY_BITS, 32, false, false},
    {"i64", TY_BITS, 64, true, false},   {"u64", TY_BITS, 64, false, false},
    {"usize", TY_BITS, 64, false, false},{"i8", TY_BITS, 8, true, false},
    {"u8", TY_BITS, 8, false, false},    {"bool", TY_BITS, 1, false, true},
    {"addr", TY_ADDR, 0, false, false},
};

// 内建标量的物理类型对象。静态存储期，指针全程稳定。
L1Type g_builtinTypes[kBuiltinCount]{};

uint32_t alignOf(uint32_t width) {
  // 对齐取字段宽度与 8 的较小者；宽度按字节算。
  uint32_t bytes = (width + 7) / 8;
  if (bytes == 0) return 1;
  uint32_t a = 1;
  while (a < bytes && a < 8) a <<= 1;
  return a > 8 ? 8 : a;
}

uint32_t roundUp(uint32_t value, uint32_t align) {
  if (align == 0) return value;
  return (value + align - 1) / align * align;
}

} // namespace

// ---------------- TypeStore ----------------

void TypeStore::initBuiltins() {
  if (!entries_.empty()) return;
  for (uint32_t i = 0; i < kBuiltinCount; ++i) {
    const BuiltinDef &def = kBuiltinDefs[i];
    g_builtinTypes[i].kind = def.kind;
    g_builtinTypes[i].width = def.width;

    Entry e{};
    e.name = def.name;
    e.lain = &g_builtinTypes[i];
    e.k = def.isBool ? Entry::K::Bool : (def.kind == TY_ADDR ? Entry::K::Addr : Entry::K::Bits);
    e.signedInt = def.signedInt;
    entries_.push_back(e);
    byName_[def.name] = static_cast<TypeId>(entries_.size()); // id = 下标+1
  }
}

TypeId TypeStore::builtin(std::string_view name) const {
  auto it = byName_.find(std::string(name));
  return it == byName_.end() ? kInvalidType : it->second;
}

TypeId TypeStore::addStruct(std::string_view name, AggregateLayout layout) {
  Entry e{};
  e.name = std::string(name);
  e.lain = nullptr;
  e.k = Entry::K::Struct;
  e.layout = std::move(layout);
  entries_.push_back(e);
  TypeId id = static_cast<TypeId>(entries_.size());
  byName_[e.name] = id;
  return id;
}

TypeId TypeStore::addEnum(std::string_view name, AggregateLayout layout) {
  Entry e{};
  e.name = std::string(name);
  e.lain = nullptr;
  e.k = Entry::K::Enum;
  e.layout = std::move(layout);
  entries_.push_back(e);
  TypeId id = static_cast<TypeId>(entries_.size());
  byName_[e.name] = id;
  return id;
}

TypeId TypeStore::elementOf(TypeId id) const {
  if (id == kInvalidType || id > entries_.size()) return kInvalidType;
  return entries_[id - 1].element;
}

TypeId TypeStore::addArrayOf(TypeId element, bool isRo) const {
  uint64_t key = (static_cast<uint64_t>(element) << 1) | (isRo ? 1u : 0u);
  auto it = arrays_.find(key);
  if (it != arrays_.end()) return it->second;

  Entry e{};
  e.name = std::string(isRo ? "ro [" : "[") + std::string(name(element)) + "]";
  e.lain = nullptr;
  e.k = Entry::K::Array;
  e.roArray = isRo;
  e.element = element;
  entries_.push_back(e);
  TypeId id = static_cast<TypeId>(entries_.size());
  arrays_[key] = id;
  return id;
}

TypeId TypeStore::addModuleType(std::string_view moduleAlias) {
  Entry e{};
  e.name = std::string(moduleAlias);
  e.lain = nullptr;
  e.k = Entry::K::Module;
  entries_.push_back(e);
  TypeId id = static_cast<TypeId>(entries_.size());
  byName_[e.name] = id;
  return id;
}

TypeId TypeStore::resolve(const ast::TypeSyntax &ts, DiagSink &diags) const {
  if (!ts.valid()) {
    diags.error(ts.span, D::SemaUnknownType, "类型标注缺失");
    return kInvalidType;
  }

  if (ts.hasQualifier) {
    // 限定名（M.Point）目前只支持「已登记的模块成员类型」，其余报未知。
    std::string key = std::string(ts.qualifier) + "." + std::string(ts.name);
    auto it = byName_.find(key);
    if (it == byName_.end()) {
      diags.error(ts.span, D::SemaUnknownType, "未知的限定类型: " + key);
      return kInvalidType;
    }
    return it->second;
  }

  std::string key(ts.name);
  if (ts.isArray) {
    TypeId elem = resolve(*ts.element, diags);
    if (elem == kInvalidType) return kInvalidType;
    return addArrayOf(elem, ts.isRo);
  }

  auto it = byName_.find(key);
  if (it == byName_.end()) {
    diags.error(ts.span, D::SemaUnknownType, "未知的类型名: " + key);
    return kInvalidType;
  }
  return it->second;
}

const L1Type *TypeStore::lain(TypeId id) const {
  if (id == kInvalidType || id > entries_.size()) return nullptr;
  return entries_[id - 1].lain;
}

bool TypeStore::isAggregate(TypeId id) const {
  if (id == kInvalidType || id > entries_.size()) return false;
  const Entry &e = entries_[id - 1];
  return e.k == Entry::K::Struct || e.k == Entry::K::Enum;
}

bool TypeStore::isModuleMemberType(TypeId id) const {
  if (id == kInvalidType || id > entries_.size()) return false;
  return entries_[id - 1].k == Entry::K::Module;
}

bool TypeStore::isAddr(TypeId id) const {
  if (id == kInvalidType || id > entries_.size()) return false;
  return entries_[id - 1].k == Entry::K::Addr;
}

bool TypeStore::isBits(TypeId id) const {
  if (id == kInvalidType || id > entries_.size()) return false;
  const Entry &e = entries_[id - 1];
  return e.k == Entry::K::Bits || e.k == Entry::K::Bool;
}

uint32_t TypeStore::width(TypeId id) const {
  const L1Type *t = lain(id);
  return t ? t->width : 0;
}

bool TypeStore::signedness(TypeId id) const {
  if (id == kInvalidType || id > entries_.size()) return false;
  return entries_[id - 1].signedInt;
}

std::string_view TypeStore::name(TypeId id) const {
  if (id == kInvalidType || id > entries_.size()) return std::string_view{};
  return std::string_view{entries_[id - 1].name};
}

const AggregateLayout *TypeStore::layout(TypeId id) const {
  if (id == kInvalidType || id > entries_.size()) return nullptr;
  return &entries_[id - 1].layout;
}

// ---------------- 算子选择 ----------------

bool selectBinary(ast::BinOp op, TypeId operandTy, const TypeStore &ts,
                  OperatorChoice &out) {
  out.kind = INST_COUNT;
  out.operandTy = nullptr;
  out.resultTy = nullptr;

  if (!ts.isBits(operandTy)) return false; // addr / 聚合 / 数组没有算子

  const uint32_t width = ts.width(operandTy);
  // i8 / u8 只作 load/store 宽度，不绑定任何算子。
  if (width == 8) return false;

  const bool signedOp = ts.signedness(operandTy);
  const L1Type *operand = ts.lain(operandTy);
  const L1Type *bits1 = ts.lain(ts.builtin("bool"));

  bool isCompare = false;
  switch (op) {
  case ast::BinOp::Add: out.kind = INST_ADD; break;
  case ast::BinOp::Sub: out.kind = INST_SUB; break;
  case ast::BinOp::Mul: out.kind = INST_MUL; break;
  case ast::BinOp::Div: out.kind = signedOp ? INST_SDIV : INST_UDIV; break;
  case ast::BinOp::Rem: out.kind = signedOp ? INST_SREM : INST_UREM; break;
  case ast::BinOp::Shl: out.kind = INST_SHL; break;
  // 右移按操作数的符号性选：有符号走算术右移（符号扩展），无符号走逻辑右移。
  case ast::BinOp::Shr: out.kind = signedOp ? INST_ASHR : INST_LSHR; break;
  case ast::BinOp::BitAnd: out.kind = INST_AND; break;
  case ast::BinOp::BitOr: out.kind = INST_OR; break;
  case ast::BinOp::BitXor: out.kind = INST_XOR; break;
  case ast::BinOp::LogicalAnd: out.kind = INST_AND; break;
  case ast::BinOp::LogicalOr: out.kind = INST_OR; break;
  case ast::BinOp::Eq: out.kind = INST_EQ; isCompare = true; break;
  case ast::BinOp::Ne: out.kind = INST_NE; isCompare = true; break;
  case ast::BinOp::Lt: out.kind = signedOp ? INST_SLT : INST_ULT; isCompare = true; break;
  case ast::BinOp::Le: out.kind = signedOp ? INST_SLE : INST_ULE; isCompare = true; break;
  case ast::BinOp::Gt: out.kind = signedOp ? INST_SGT : INST_UGT; isCompare = true; break;
  case ast::BinOp::Ge: out.kind = signedOp ? INST_SGE : INST_UGE; isCompare = true; break;
  }
  if (out.kind == INST_COUNT) return false;

  // 比较的结果恒为 #bits<1>，其余跟随操作数宽度。
  out.operandTy = operand;
  out.resultTy = isCompare ? (bits1 ? bits1 : operand) : operand;
  return true;
}

// ---------------- 作用域遍历 ----------------

namespace {

class Walker {
public:
  Walker(TypeStore &ts, SemaResult &out, DiagSink &diags)
      : ts_(ts), out_(out), diags_(diags) {}

  void run(const ast::Module &mod) {
    // 第一遍：登记所有声明（布局与签名），因为使用可以出现在定义之前。
    for (const ast::Decl *d : mod.decls) declare(d);
    // 顶层静态绑定既是数据块、也是一个「取值过程」的名字，在表达式里按 #addr
    // 使用。必须在走函数体之前绑好，否则函数体里查不到这个名字。
    for (const ast::Decl *d : mod.decls) {
      if (d->kind != ast::DeclKind::StaticLet) continue;
      const auto *s = static_cast<const ast::StaticLetDecl *>(d);
      TypeId t = s->hasType ? ts_.resolve(s->type, diags_) : kInvalidType;
      if (s->init && t == kInvalidType) t = typeOf(s->init, kInvalidType);
      if (t != kInvalidType) {
        out_.topLevelTypes[std::string(s->name)] = t;
        // 产物是 `data NAME_storage …` + `#proc NAME() -> #addr`。
        bind(s->name, ts_.builtin("addr"));
      }
      // 长度：表按元素个数，字符串按字节数。
      if (s->init && s->init->kind == ast::ExprKind::Table) {
        out_.topLevelLength[std::string(s->name)] =
            static_cast<const ast::TableExpr *>(s->init)->elements.size();
      } else if (s->init && s->init->kind == ast::ExprKind::StrLit) {
        out_.topLevelLength[std::string(s->name)] =
            static_cast<const ast::StrLitExpr *>(s->init)->rawText.size();
      }
    }
    // 第二遍：走函数体，定出每个表达式与局部绑定的类型。
    for (const ast::Decl *d : mod.decls) {
      if (d->kind != ast::DeclKind::Func) continue;
      walkFunc(static_cast<const ast::FuncDecl *>(d));
    }
  }

private:
  TypeStore &ts_;
  SemaResult &out_;
  DiagSink &diags_;
  std::vector<std::unordered_map<std::string, TypeId>> scopes_;
  TypeId funcRet_ = kInvalidType;

  void pushScope() { scopes_.emplace_back(); }
  void popScope() { scopes_.pop_back(); }

  void bind(std::string_view name, TypeId ty) {
    if (scopes_.empty()) scopes_.emplace_back();
    scopes_.back()[std::string(name)] = ty;
  }

  TypeId lookup(std::string_view name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      auto found = it->find(std::string(name));
      if (found != it->end()) return found->second;
    }
    return kInvalidType;
  }

  void declare(const ast::Decl *d) {
    switch (d->kind) {
    case ast::DeclKind::Struct: {
      const auto *s = static_cast<const ast::StructDecl *>(d);
      AggregateLayout layout{};
      uint32_t offset = 0;
      uint32_t maxAlign = 1;
      for (const ast::FieldDecl &f : s->fields) {
        TypeId ft = ts_.resolve(f.type, diags_);
        if (ft == kInvalidType) continue;
        uint32_t fw = ts_.width(ft);
        uint32_t align = ts_.isAggregate(ft) ? 8 : alignOf(fw);
        if (align > maxAlign) maxAlign = align;
        offset = roundUp(offset, align);
        layout.fields.push_back(FieldLayout{f.name, ft, offset});
        offset += (fw + 7) / 8;
      }
      layout.size = roundUp(offset, maxAlign);
      layout.align = maxAlign;
      ts_.addStruct(s->name, std::move(layout));
      break;
    }
    case ast::DeclKind::Enum: {
      const auto *e = static_cast<const ast::EnumDecl *>(d);
      AggregateLayout layout{};
      uint32_t maxAlign = 1;
      const uint32_t tagSize = 1;
      uint32_t offset = tagSize;
      uint64_t tag = 0;
      for (const ast::VariantDecl &v : e->variants) {
        VariantLayout vl{};
        vl.name = v.name;
        vl.tag = tag++;
        vl.payload = kInvalidType;
        vl.payloadOffset = 0;
        if (!v.payload.empty()) {
          TypeId pt = ts_.resolve(v.payload[0], diags_);
          vl.payload = pt;
          uint32_t pw = pt == kInvalidType ? 0 : ts_.width(pt);
          uint32_t align = ts_.isAggregate(pt) ? 8 : alignOf(pw);
          if (align > maxAlign) maxAlign = align;
          offset = roundUp(offset, align);
          vl.payloadOffset = offset;
          offset += (pw + 7) / 8;
        }
        layout.variants.push_back(vl);
      }
      layout.size = roundUp(offset, maxAlign);
      layout.align = maxAlign;
      ts_.addEnum(e->name, std::move(layout));
      break;
    }
    case ast::DeclKind::Func:
    case ast::DeclKind::Extern: {
      const ast::FuncDecl *f = nullptr;
      const ast::ExternDecl *x = nullptr;
      std::string_view fname;
      TypeId ret = kInvalidType;
      bool hasRet = false;
      if (d->kind == ast::DeclKind::Func) {
        f = static_cast<const ast::FuncDecl *>(d);
        fname = f->name;
        hasRet = f->hasRet;
        if (hasRet) ret = ts_.resolve(f->ret, diags_);
      } else {
        x = static_cast<const ast::ExternDecl *>(d);
        fname = x->name;
        hasRet = x->hasRet;
        if (hasRet) ret = ts_.resolve(x->ret, diags_);
      }
      if (hasRet && ret != kInvalidType)
        out_.functionReturns[std::string(fname)] = ret;
      break;
    }
    case ast::DeclKind::Import: {
      const auto *im = static_cast<const ast::ImportDecl *>(d);
      ts_.addModuleType(im->name);
      out_.moduleMembers[std::string(im->name)] = ModuleMember{
          ModuleMember::Kind::Type, im->name, ts_.builtin(im->name), im->path};
      break;
    }
    case ast::DeclKind::StaticLet:
      break;
    }
  }

  void walkFunc(const ast::FuncDecl *f) {
    funcRet_ = kInvalidType;
    auto it = out_.functionReturns.find(std::string(f->name));
    if (it != out_.functionReturns.end()) funcRet_ = it->second;

    pushScope();
    for (const ast::ParamDecl &p : f->params) {
      TypeId pt = ts_.resolve(p.type, diags_);
      if (pt != kInvalidType) bind(p.name, pt);
    }
    walkBody(f->body);
    popScope();
  }

  void walkBody(const std::vector<ast::Stmt *> &body) {
    for (const ast::Stmt *s : body) walkStmt(s);
  }

  void walkStmt(const ast::Stmt *s) {
    if (!s) return;
    switch (s->kind) {
    case ast::StmtKind::Let: {
      const auto *l = static_cast<const ast::LetStmt *>(s);
      TypeId declared = l->hasType ? ts_.resolve(l->type, diags_) : kInvalidType;
      TypeId initTy = l->init ? typeOf(l->init, declared) : kInvalidType;
      TypeId finalTy = declared != kInvalidType ? declared : initTy;
      if (finalTy == kInvalidType) {
        diags_.error(l->span, D::SemaWidthUnknown,
                     "绑定 " + std::string(l->name) + " 的类型无法推导");
      } else {
        bind(l->name, finalTy);
      }
      break;
    }
    case ast::StmtKind::Return: {
      const auto *r = static_cast<const ast::ReturnStmt *>(s);
      if (r->value) (void)typeOf(r->value, funcRet_);
      break;
    }
    case ast::StmtKind::Assign: {
      const auto *a = static_cast<const ast::AssignStmt *>(s);
      TypeId targetTy = typeOf(a->target, kInvalidType);
      if (a->value) (void)typeOf(a->value, targetTy);
      break;
    }
    case ast::StmtKind::ExprStmt: {
      const auto *e = static_cast<const ast::ExprStmt *>(s);
      if (e->expr) (void)typeOf(e->expr, kInvalidType);
      break;
    }
    case ast::StmtKind::If: {
      const auto *i = static_cast<const ast::IfStmt *>(s);
      if (i->cond) (void)typeOf(i->cond, kInvalidType);
      walkBody(i->thenBody);
      if (i->hasElse) walkBody(i->elseBody);
      break;
    }
    case ast::StmtKind::Loop: {
      const auto *l = static_cast<const ast::LoopStmt *>(s);
      pushScope();
      for (const ast::LoopParam &p : l->params) {
        TypeId pt = ts_.resolve(p.type, diags_);
        if (pt != kInvalidType) bind(p.name, pt);
      }
      if (l->cond) (void)typeOf(l->cond, kInvalidType);
      walkBody(l->body);
      popScope();
      break;
    }
    case ast::StmtKind::For: {
      const auto *f = static_cast<const ast::ForStmt *>(s);
      pushScope();
      TypeId vt = f->hasVarType ? ts_.resolve(f->varType, diags_)
                                : ts_.builtin("i32");
      bind(f->var, vt);
      if (f->hasAcc) {
        TypeId at = ts_.resolve(f->accType, diags_);
        if (at == kInvalidType) at = vt;
        bind(f->acc, at);
      }
      if (f->from) (void)typeOf(f->from, vt);
      if (f->to) (void)typeOf(f->to, vt);
      walkBody(f->body);
      popScope();
      break;
    }
    case ast::StmtKind::Jump: {
      const auto *j = static_cast<const ast::JumpStmt *>(s);
      for (ast::Expr *a : j->args) (void)typeOf(a, kInvalidType);
      break;
    }
    }
  }

  TypeId typeOf(const ast::Expr *e, TypeId expected) {
    if (!e) return kInvalidType;
    TypeId result = compute(e, expected);
    if (result == kInvalidType && expected != kInvalidType) result = expected;
    out_.exprTypes[e] = result;
    return result;
  }

  TypeId compute(const ast::Expr *e, TypeId expected) {
    switch (e->kind) {
    case ast::ExprKind::IntLit:
      // 字面量自身没有类型，取上下文；没有上下文时退回 i32。
      return expected != kInvalidType ? expected : ts_.builtin("i32");
    case ast::ExprKind::BoolLit:
      return ts_.builtin("bool");
    case ast::ExprKind::StrLit:
      return ts_.builtin("addr");
    case ast::ExprKind::Name: {
      const auto *n = static_cast<const ast::NameExpr *>(e);
      TypeId t = lookup(n->name);
      if (t != kInvalidType) return t;
      diags_.error(n->span, D::SemaUnknownName,
                   "未绑定的名字: " + std::string(n->name));
      return kInvalidType;
    }
    case ast::ExprKind::Call: {
      const auto *c = static_cast<const ast::CallExpr *>(e);
      for (ast::Expr *a : c->args) (void)typeOf(a, kInvalidType);
      if (c->callee && c->callee->kind == ast::ExprKind::Name) {
        const auto *n = static_cast<const ast::NameExpr *>(c->callee);
        auto it = out_.functionReturns.find(std::string(n->name));
        if (it != out_.functionReturns.end()) return it->second;
      }
      return kInvalidType;
    }
    case ast::ExprKind::Cast: {
      const auto *c = static_cast<const ast::CastExpr *>(e);
      (void)typeOf(c->operand, kInvalidType);
      return ts_.resolve(c->target, diags_);
    }
    case ast::ExprKind::Construct: {
      const auto *c = static_cast<const ast::ConstructExpr *>(e);
      for (const ast::FieldInit &f : c->fields) (void)typeOf(f.value, kInvalidType);
      return ts_.resolve(c->target, diags_);
    }
    case ast::ExprKind::Table: {
      const auto *t = static_cast<const ast::TableExpr *>(e);
      TypeId elem = ts_.builtin("u64");
      for (ast::Expr *x : t->elements) {
        TypeId et = typeOf(x, elem);
        if (et != kInvalidType) elem = et;
      }
      return ts_.addArrayOf(elem, false);
    }
    case ast::ExprKind::Binary: {
      const auto *b = static_cast<const ast::BinaryExpr *>(e);
      // 先按左操作数定类型，再用它当上下文去定两侧，避免字面量取不到宽度。
      TypeId lhs = typeOf(b->lhs, expected);
      TypeId rhs = typeOf(b->rhs, lhs);
      (void)rhs;
      switch (b->op) {
      case ast::BinOp::Eq:
      case ast::BinOp::Ne:
      case ast::BinOp::Lt:
      case ast::BinOp::Le:
      case ast::BinOp::Gt:
      case ast::BinOp::Ge:
        return ts_.builtin("bool");
      default:
        return lhs;
      }
    }
    case ast::ExprKind::Unary: {
      const auto *u = static_cast<const ast::UnaryExpr *>(e);
      return typeOf(u->operand, expected);
    }
    case ast::ExprKind::Index: {
      const auto *ix = static_cast<const ast::IndexExpr *>(e);
      TypeId baseTy = typeOf(ix->base, kInvalidType);
      (void)typeOf(ix->index, ts_.builtin("u64"));
      if (baseTy == kInvalidType || baseTy > 0) {
        // 数组元素类型：TypeStore 的 Entry 里存了 element，这里取不到就退回 u64。
        // （数组元素类型的公开访问不在当前契约里，Slice A 只用到 u64 表。）
        return ts_.builtin("u64");
      }
      return kInvalidType;
    }
    case ast::ExprKind::ValueIf: {
      const auto *v = static_cast<const ast::ValueIfExpr *>(e);
      (void)typeOf(v->cond, kInvalidType);
      TypeId thenTy = kInvalidType;
      for (ast::Stmt *s : v->thenBody) {
        if (s->kind == ast::StmtKind::ExprStmt) {
          thenTy = typeOf(static_cast<const ast::ExprStmt *>(s)->expr, expected);
        } else {
          walkStmt(s);
        }
      }
      for (ast::Stmt *s : v->elseBody) {
        if (s->kind == ast::StmtKind::ExprStmt) {
          (void)typeOf(static_cast<const ast::ExprStmt *>(s)->expr, thenTy);
        } else {
          walkStmt(s);
        }
      }
      return thenTy != kInvalidType ? thenTy : expected;
    }
    case ast::ExprKind::Loop: {
      const auto *le = static_cast<const ast::LoopExpr *>(e);
      // 无论是否带标注，都把体跑一遍（绑定参数、检查体语句、登记各表达式类型）
      // —— 否则 Lower 在体里找不到操作数类型。返回的「结果类型」才用标注决定。
      pushScope();
      for (const ast::LoopParam &p : le->params) {
        TypeId pt = ts_.resolve(p.type, diags_);
        if (pt != kInvalidType) bind(p.name, pt);
      }
      if (le->cond) (void)typeOf(le->cond, kInvalidType); // 条件里的参数也要登记类型
      walkBody(le->body);
      popScope();
      if (expected != kInvalidType) return expected;
      // 无标注：结果类型取最后一个参数（累加器）的声明类型。
      if (!le->params.empty()) {
        TypeId t = ts_.resolve(le->params.back().type, diags_);
        if (t != kInvalidType) return t;
      }
      return kInvalidType;
    }
    case ast::ExprKind::For: {
      const auto *fe = static_cast<const ast::ForExpr *>(e);
      pushScope();
      // 循环变量的类型：显式标注优先，否则从 from 推导。
      TypeId varTy = kInvalidType;
      if (fe->hasVarType) varTy = ts_.resolve(fe->varType, diags_);
      if (varTy == kInvalidType && fe->from) varTy = typeOf(fe->from, kInvalidType);
      if (varTy != kInvalidType) bind(fe->var, varTy);
      // 累加器：显式标注优先，否则从初值推导。
      TypeId accTy = kInvalidType;
      if (fe->hasAcc) {
        if (!fe->accType.name.empty()) accTy = ts_.resolve(fe->accType, diags_);
        if (accTy == kInvalidType && fe->accInit)
          accTy = typeOf(fe->accInit, kInvalidType);
        if (accTy != kInvalidType) bind(fe->acc, accTy);
      }
      if (fe->to) (void)typeOf(fe->to, varTy);
      walkBody(fe->body);
      popScope();
      if (expected != kInvalidType) return expected;
      // 无标注：有 acc 取 acc 类型，否则取循环变量类型（值由 break 带出）。
      if (accTy != kInvalidType) return accTy;
      return varTy;
    }
    case ast::ExprKind::Dotted:
    default:
      return kInvalidType;
    }
  }
};

} // namespace

bool analyze(const ast::Module &mod, TypeStore &ts, SemaResult &out,
             DiagSink &diags) {
  ts.initBuiltins();
  Walker w(ts, out, diags);
  w.run(mod);
  return !diags.hasErrors();
}

} // namespace ln
