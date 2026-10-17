#include "lain0/Sema.hpp"

// 语义层实现。
//
// 这一层不产出 IR，只产出「降级需要的结论」：每个表达式的类型、聚合类型的布局、
// 以及给定操作数类型时该发哪一条 LAINIR 指令。
//
// 内建标量是固定的九种，所以它们的 L1Type 直接是静态对象，不需要 L1Builder。
//
// 模块命名空间（docs/plan-modules.md §3）：Program 先把整个 import 依赖图加载完
// （同键只加载一次），再分轮分析 —— 先给所有模块的 struct/enum 建**名字条目**，
// 再算布局（字段类型可以指向另一个模块的聚合），然后登记顶层声明与 import 成员表，
// 最后走函数体。类型表与符号表的键一律是 `<模块键>__<名字>`。

#include <cstddef>
#include <deque>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "lain0/Ast.hpp"
#include "lain0/Diag.hpp"
#include "lain0/Lexer.hpp"
#include "lain0/Parser.hpp"

namespace ln {
namespace {

// ---------------- 路径与模块键 ----------------
// §3.1：源根是入口文件（或清单）所在目录；模块键是「规范化后的文件路径相对源根、
// 去掉 .lain、把 / 换成 _」。规范化只做词法处理（. / .. / //），跑出源根报错。

std::string normalizePath(const std::string &in) {
  std::string p;
  p.reserve(in.size());
  for (char c : in) {
    p.push_back(c == '\\' ? '/' : c);
  }
  const bool absolute = !p.empty() && p[0] == '/';

  std::vector<std::string> parts{};
  std::string cur{};
  auto flush = [&]() {
    if (cur.empty() || cur == ".") {
      cur.clear();
      return;
    }
    if (cur == "..") {
      // 只有上一段也是普通目录时才真正弹掉；否则保留（它会在 keyOf 里被判越界）。
      if (!parts.empty() && parts.back() != "..") parts.pop_back();
      else parts.push_back("..");
    } else {
      parts.push_back(cur);
    }
    cur.clear();
  };
  for (char c : p) {
    if (c == '/') flush();
    else cur.push_back(c);
  }
  flush();

  std::string out;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i != 0) out += '/';
    out += parts[i];
  }
  if (absolute) return "/" + out;
  return out.empty() ? std::string(".") : out;
}

std::string directoryOf(const std::string &path) {
  const size_t pos = path.find_last_of('/');
  return pos == std::string::npos ? "." : path.substr(0, pos);
}

std::string resolveModulePath(const std::string &dir,
                              const std::string &spec) {
  // "examples::modules_lib" —— 取 :: 之后最后一段作模块名，逐层目录试开。
  const size_t pos = spec.rfind("::");
  const std::string base = pos == std::string::npos ? "" : spec.substr(0, pos);
  const std::string mod = pos == std::string::npos ? spec : spec.substr(pos + 2);
  const std::vector<std::string> cands = {
      dir + "/" + mod + ".lain",
      dir + "/" + base + "/" + mod + ".lain",
  };
  for (const auto &c : cands) {
    std::ifstream f(c);
    if (f) return c;
  }
  return cands.empty() ? spec : cands.front();
}

std::string readFileText(const std::string &filePath, DiagSink &diags, Span sp) {
  std::ifstream f(filePath, std::ios::binary);
  if (!f) {
    diags.error(sp, D::IoOpenFailed, "打不开源文件: " + filePath);
    return "";
  }
  return std::string((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
}

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
    {"i32", TY_BITS, 32, true, false},    {"u32", TY_BITS, 32, false, false},
    {"i64", TY_BITS, 64, true, false},    {"u64", TY_BITS, 64, false, false},
    {"usize", TY_BITS, 64, false, false}, {"i8", TY_BITS, 8, true, false},
    {"u8", TY_BITS, 8, false, false},     {"bool", TY_BITS, 1, false, true},
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

// ---------------- 聚合布局（第二轮） ----------------
// §3.5：字段类型指向尚未算布局的聚合时递归计算，递归回到自身 →
// 「按值包含自身的聚合」。

bool layoutOf(TypeStore &ts, TypeId id, LayoutSites &sites,
              std::vector<TypeId> &stack, DiagSink &diags);

bool computeStructLayout(TypeStore &ts, const ast::StructDecl *s,
                         const ModuleContext &ctx,
                         LayoutSites &sites,
                         std::vector<TypeId> &stack, DiagSink &diags) {
  AggregateLayout layout{};
  uint32_t offset = 0;
  uint32_t maxAlign = 1;
  for (const ast::FieldDecl &f : s->fields) {
    TypeId ft = ts.resolve(f.type, ctx, diags);
    if (ft == kInvalidType) continue;
    if (ts.isAggregate(ft) && !ts.hasLayout(ft)) {
      if (!layoutOf(ts, ft, sites, stack, diags)) return false;
    }
    uint32_t fw = ts.width(ft);
    uint32_t align = ts.isAggregate(ft) ? 8 : alignOf(fw);
    if (align > maxAlign) maxAlign = align;
    offset = roundUp(offset, align);
    layout.fields.push_back(FieldLayout{f.name, ft, offset});
    offset += (fw + 7) / 8;
  }
  layout.size = roundUp(offset, maxAlign);
  layout.align = maxAlign;
  ts.fillStructLayout(ts.lookup(ctx.qualify(s->name)), std::move(layout));
  return true;
}

bool computeEnumLayout(TypeStore &ts, const ast::EnumDecl *e,
                       const ModuleContext &ctx, LayoutSites &sites,
                       std::vector<TypeId> &stack, DiagSink &diags) {
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
      TypeId pt = ts.resolve(v.payload[0], ctx, diags);
      vl.payload = pt;
      if (ts.isAggregate(pt) && !ts.hasLayout(pt)) {
        if (!layoutOf(ts, pt, sites, stack, diags)) return false;
      }
      uint32_t pw = pt == kInvalidType ? 0 : ts.width(pt);
      uint32_t align = ts.isAggregate(pt) ? 8 : alignOf(pw);
      if (align > maxAlign) maxAlign = align;
      offset = roundUp(offset, align);
      vl.payloadOffset = offset;
      offset += (pw + 7) / 8;
    }
    layout.variants.push_back(vl);
  }
  layout.size = roundUp(offset, maxAlign);
  layout.align = maxAlign;
  ts.fillEnumLayout(ts.lookup(ctx.qualify(e->name)), std::move(layout));
  return true;
}

bool layoutOf(TypeStore &ts, TypeId id, LayoutSites &sites,
              std::vector<TypeId> &stack, DiagSink &diags) {
  auto it = sites.find(id);
  if (it == sites.end()) return true; // 内建 / 数组 / 引用：没有站点
  for (TypeId open : stack) {
    if (open == id) {
      diags.error(it->second.decl->span, D::SemaUnknownType,
                  "按值包含自身的聚合: " + std::string(ts.name(id)));
      return false;
    }
  }
  if (ts.hasLayout(id)) return true;
  stack.push_back(id);
  const bool ok =
      it->second.isEnum
          ? computeEnumLayout(
                ts, static_cast<const ast::EnumDecl *>(it->second.decl),
                *it->second.ctx, sites, stack, diags)
          : computeStructLayout(
                ts, static_cast<const ast::StructDecl *>(it->second.decl),
                *it->second.ctx, sites, stack, diags);
  stack.pop_back();
  return ok;
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

TypeId TypeStore::declareStruct(std::string_view name) {
  auto it = byName_.find(std::string(name));
  if (it != byName_.end()) return it->second; // 同名重复声明沿用原条目
  Entry e{};
  e.name = std::string(name);
  e.lain = nullptr;
  e.k = Entry::K::Struct;
  e.laidOut = false;
  entries_.push_back(e);
  TypeId id = static_cast<TypeId>(entries_.size());
  byName_[e.name] = id;
  return id;
}

TypeId TypeStore::declareEnum(std::string_view name) {
  auto it = byName_.find(std::string(name));
  if (it != byName_.end()) return it->second;
  Entry e{};
  e.name = std::string(name);
  e.lain = nullptr;
  e.k = Entry::K::Enum;
  e.laidOut = false;
  entries_.push_back(e);
  TypeId id = static_cast<TypeId>(entries_.size());
  byName_[e.name] = id;
  return id;
}

void TypeStore::fillStructLayout(TypeId id, AggregateLayout layout) {
  if (id == kInvalidType || id > entries_.size()) return;
  Entry &e = entries_[id - 1];
  e.k = Entry::K::Struct;
  e.layout = std::move(layout);
  e.laidOut = true;
}

void TypeStore::fillEnumLayout(TypeId id, AggregateLayout layout) {
  if (id == kInvalidType || id > entries_.size()) return;
  Entry &e = entries_[id - 1];
  e.k = Entry::K::Enum;
  e.layout = std::move(layout);
  e.laidOut = true;
}

bool TypeStore::hasLayout(TypeId id) const {
  if (id == kInvalidType || id > entries_.size()) return true;
  return entries_[id - 1].laidOut;
}

TypeId TypeStore::lookup(std::string_view name) const {
  auto it = byName_.find(std::string(name));
  return it == byName_.end() ? kInvalidType : it->second;
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
  auto it = byName_.find(std::string(moduleAlias));
  if (it != byName_.end()) return it->second;
  Entry e{};
  e.name = std::string(moduleAlias);
  e.lain = nullptr;
  e.k = Entry::K::Module;
  entries_.push_back(e);
  TypeId id = static_cast<TypeId>(entries_.size());
  byName_[e.name] = id;
  return id;
}

TypeId TypeStore::addRef(TypeId pointee, bool isMutable) const {
  // ref / mut ref 的 LAINIR 形态就是 #addr，与内建 addr 共用那条 L1Type。
  Entry e{};
  e.name = std::string(isMutable ? "mut ref " : "ref ") +
           std::string(name(pointee));
  TypeId addrId = builtin("addr");
  e.lain = addrId != kInvalidType ? entries_[addrId - 1].lain : nullptr;
  e.k = Entry::K::Ref;
  e.element = pointee;
  e.refMutable = isMutable;
  entries_.push_back(e);
  return static_cast<TypeId>(entries_.size());
}

TypeId TypeStore::addSlice(TypeId element) const {
  // slice T -> (addr base, u64 len) 的 2 字段聚合。
  Entry e{};
  e.name = std::string("slice ") + std::string(name(element));
  e.lain = nullptr;
  e.k = Entry::K::Slice;
  e.element = element;
  AggregateLayout lay{};
  FieldLayout base{"ptr", builtin("addr"), 0};
  FieldLayout len{"len", builtin("u64"), 8};
  lay.fields.push_back(base);
  lay.fields.push_back(len);
  lay.size = 16;
  lay.align = 8;
  e.layout = std::move(lay);
  entries_.push_back(e);
  return static_cast<TypeId>(entries_.size());
}

TypeId TypeStore::resolve(const ast::TypeSyntax &ts, const ModuleContext &ctx,
                          DiagSink &diags) const {
  if (!ts.valid()) {
    diags.error(ts.span, D::SemaUnknownType, "类型标注缺失");
    return kInvalidType;
  }

  if (ts.ptrKind != ast::TypeSyntax::PtrKind::None) {
    if (ts.inner == nullptr) {
      diags.error(ts.span, D::SemaUnknownType, "引用/切片类型缺少内层类型");
      return kInvalidType;
    }
    TypeId inner = resolve(*ts.inner, ctx, diags);
    if (inner == kInvalidType) return kInvalidType;
    if (ts.ptrKind == ast::TypeSyntax::PtrKind::Slice) return addSlice(inner);
    return addRef(inner, ts.ptrKind == ast::TypeSyntax::PtrKind::MutRef);
  }
  if (ts.hasQualifier) {
    // 限定名（M.Point）：别名表里查到键就查被导入模块的限定名；
    // 别名不在表里才退回「限定符本身是类型名」（Shape.Circle 形态）。
    auto aliasIt = ctx.aliases.find(std::string(ts.qualifier));
    if (aliasIt != ctx.aliases.end()) {
      const std::string key = aliasIt->second + "__" + std::string(ts.name);
      auto it = byName_.find(key);
      if (it != byName_.end()) return it->second;
      diags.error(ts.span, D::SemaUnknownType,
                  "未知的限定类型: " + std::string(ts.qualifier) + "." +
                      std::string(ts.name));
      return kInvalidType;
    }
    // `Shape.Circle` 这种「enum 名.变体名」：变体构造的目标类型就是那个 enum
    // 本身（Circle 是 Shape 的一个变体，不是独立类型）。
    auto enumIt = byName_.find(ctx.qualify(ts.qualifier));
    if (enumIt != byName_.end()) {
      const AggregateLayout *lay = layout(enumIt->second);
      if (lay != nullptr && !lay->variants.empty()) return enumIt->second;
    }
    diags.error(ts.span, D::SemaUnknownType,
                "未知的限定类型: " + std::string(ts.qualifier) + "." +
                    std::string(ts.name));
    return kInvalidType;
  }

  std::string key(ts.name);
  if (ts.isArray) {
    TypeId elem = resolve(*ts.element, ctx, diags);
    if (elem == kInvalidType) return kInvalidType;
    return addArrayOf(elem, ts.isRo);
  }

  // 裸名先按当前模块限定查，再退回内建标量表（内建名不限定、模块不导出标量）。
  auto it = byName_.find(ctx.qualify(ts.name));
  if (it != byName_.end()) return it->second;
  it = byName_.find(key);
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
  return e.k == Entry::K::Struct || e.k == Entry::K::Enum ||
         e.k == Entry::K::Slice;
}

bool TypeStore::isModuleMemberType(TypeId id) const {
  if (id == kInvalidType || id > entries_.size()) return false;
  return entries_[id - 1].k == Entry::K::Module;
}

bool TypeStore::isAddr(TypeId id) const {
  if (id == kInvalidType || id > entries_.size()) return false;
  const Entry::K k = entries_[id - 1].k;
  return k == Entry::K::Addr || k == Entry::K::Ref;
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
  // i8 / u8 现在也支持算术/比较：直接落到与操作数同宽度的 #add/#eq/… 指令上
  // （LAINIR 的算子只认物理宽度，8-bit 同宽运算和比较都是合法的）。

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

// 一个模块的语义结论。绑定模块身份（ctx），分三个阶段跑：
//   registerTopLevel -> registerImports -> walkBodies
class Walker {
public:
  Walker(TypeStore &ts, SemaResult &out, DiagSink &diags, const Program &prog,
         const ModuleUnit &unit)
      : ts_(ts), out_(out), diags_(diags), prog_(prog), unit_(unit),
        ctx_(unit.ctx) {}

  // 顶层 func / extern / 静态 let：登记返回类型、类型、长度与发出的符号名。
  // struct/enum 的名字条目已由 Program 的第一轮建好，这里不碰。
  void registerTopLevel() {
    for (const ast::Decl *d : unit_.ast->decls) {
      switch (d->kind) {
      case ast::DeclKind::Func:
      case ast::DeclKind::Extern: {
        std::string_view fname{};
        TypeId ret = kInvalidType;
        bool hasRet = false;
        if (d->kind == ast::DeclKind::Func) {
          const auto *f = static_cast<const ast::FuncDecl *>(d);
          fname = f->name;
          hasRet = f->hasRet;
          if (hasRet) ret = ts_.resolve(f->ret, ctx_, diags_);
          // func 走模块前缀。
          out_.topLevelSymbols[ctx_.qualify(fname)] = ctx_.qualify(fname);
        } else {
          const auto *x = static_cast<const ast::ExternDecl *>(d);
          fname = x->name;
          hasRet = x->hasRet;
          if (hasRet) ret = ts_.resolve(x->ret, ctx_, diags_);
          // 宿主 extern 保持平坦：宿主按 link_name 找它，同名 extern 跨模块
          // 重复是验证器明确放行的（双方都是 extern 且 link_name 相等）。
          out_.topLevelSymbols[ctx_.qualify(fname)] = std::string(fname);
        }
        if (hasRet && ret != kInvalidType)
          out_.functionReturns[ctx_.qualify(fname)] = ret;
        break;
      }
      case ast::DeclKind::StaticLet: {
        const auto *s = static_cast<const ast::StaticLetDecl *>(d);
        TypeId t =
            s->hasType ? ts_.resolve(s->type, ctx_, diags_) : kInvalidType;
        if (s->init && t == kInvalidType) t = typeOf(s->init, kInvalidType);
        const std::string q = ctx_.qualify(s->name);
        if (t != kInvalidType) {
          out_.topLevelTypes[q] = t;
          // 产物是 `data <键>__<名字>_storage` + `#proc <鍵>__<名字>() -> #addr`。
          out_.topLevelSymbols[q] = q;
          bind(s->name, ts_.builtin("addr"));
        }
        // 长度：表按元素个数，字符串按字节数。
        if (s->init && s->init->kind == ast::ExprKind::Table) {
          out_.topLevelLength[q] =
              static_cast<const ast::TableExpr *>(s->init)->elements.size();
          out_.topLevelData.insert(q);
        } else if (s->init && s->init->kind == ast::ExprKind::StrLit) {
          out_.topLevelLength[q] =
              static_cast<const ast::StrLitExpr *>(s->init)->rawText.size();
          out_.topLevelData.insert(q);
        } else if (s->init && s->init->kind == ast::ExprKind::Construct) {
          out_.topLevelData.insert(q);
        }
        break;
      }
      case ast::DeclKind::Struct:
      case ast::DeclKind::Enum:
      case ast::DeclKind::Import:
        break; // 前两者在第一/二轮；import 在 registerImports
      }
    }
  }

  // import 成员表：只引用被导入模块自己的顶层声明，所以一轮就能填满。
  // 成员分三种：值绑定（静态 let）、函数（func）、类型（struct/enum）。
  void registerImports() {
    for (const ast::Decl *d : unit_.ast->decls) {
      if (d->kind != ast::DeclKind::Import) continue;
      const auto *im = static_cast<const ast::ImportDecl *>(d);
      const std::string alias(im->name);
      ts_.addModuleType(ctx_.qualify(alias));
      out_.moduleMembers[alias] = ModuleMember{
          ModuleMember::Kind::Type, im->name, ts_.builtin(ctx_.qualify(alias)),
          std::string(im->path)};

      const ModuleUnit *dep = depUnitOf(alias);
      if (dep == nullptr) continue;
      // 成员表按被导入模块自己的顶层声明建，与它的源码顺序一致。
      for (const ast::Decl *dd : dep->ast->decls) {
        switch (dd->kind) {
        case ast::DeclKind::StaticLet: {
          const auto *s = static_cast<const ast::StaticLetDecl *>(dd);
          const std::string q = dep->ctx.qualify(s->name);
          auto t = dep->sema.topLevelTypes.find(q);
          if (t == dep->sema.topLevelTypes.end()) break;
          ModuleMember mm{};
          mm.kind = ModuleMember::Kind::Value;
          // §3.6：`M.NAME` 降级成对访问器过程的调用，所以成员的值类型必须是
          // 访问器真正返回的类型。发数据块的那三种初值（表 / 字节串 / 顶层聚合
          // 构造）走 buildAccessor，返回 #addr —— 声明成 addr 才能让
          // `M.NAME + i` 与 `store(M.NAME + i, v)` 落到既有地址算术上；
          // 其余初值发的是取值过程，返回声明类型（`let a: u64 = M.answer;`）。
          mm.type = dep->sema.topLevelData.count(q) != 0 ? ts_.builtin("addr")
                                                         : t->second;
          mm.declType = t->second;
          // 静态 let 的访问器过程名就是它发出的符号名。
          auto sym = dep->sema.topLevelSymbols.find(q);
          mm.mangled = sym == dep->sema.topLevelSymbols.end() ? q : sym->second;
          out_.moduleMembers[alias + "." + std::string(s->name)] = mm;
          break;
        }
        case ast::DeclKind::Func: {
          const auto *f = static_cast<const ast::FuncDecl *>(dd);
          const std::string q = dep->ctx.qualify(f->name);
          ModuleMember mm{};
          mm.kind = ModuleMember::Kind::Func;
          // 返回类型：无标注的 func 没有返回类型，kInvalidType 表示「调用不产值」。
          auto r = dep->sema.functionReturns.find(q);
          mm.type = r == dep->sema.functionReturns.end() ? kInvalidType : r->second;
          auto sym = dep->sema.topLevelSymbols.find(q);
          mm.mangled = sym == dep->sema.topLevelSymbols.end() ? q : sym->second;
          out_.moduleMembers[alias + "." + std::string(f->name)] = mm;
          break;
        }
        case ast::DeclKind::Struct:
        case ast::DeclKind::Enum: {
          // 类型成员：`M.Point` 的类型标注走 TypeStore::resolve 的限定分支，
          // 这里只登记，供「M.Type 当值用」之类的非法形态给出明确诊断。
          ModuleMember mm{};
          mm.kind = ModuleMember::Kind::Type;
          mm.type = ts_.lookup(dep->ctx.qualify(dd->name));
          out_.moduleMembers[alias + "." + std::string(dd->name)] = mm;
          break;
        }
        default:
          break;
        }
      }
    }
  }

  void walkBodies() {
    for (const ast::Decl *d : unit_.ast->decls) {
      if (d->kind != ast::DeclKind::Func) continue;
      walkFunc(static_cast<const ast::FuncDecl *>(d));
    }
  }

private:
  const ModuleUnit *depUnitOf(const std::string &alias) const {
    auto it = ctx_.aliases.find(alias);
    if (it == ctx_.aliases.end()) return nullptr;
    return prog_.find(it->second);
  }

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

  void walkFunc(const ast::FuncDecl *f) {
    funcRet_ = kInvalidType;
    auto it = out_.functionReturns.find(ctx_.qualify(f->name));
    if (it != out_.functionReturns.end()) funcRet_ = it->second;

    pushScope();
    for (const ast::ParamDecl &p : f->params) {
      TypeId pt = ts_.resolve(p.type, ctx_, diags_);
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
      TypeId declared = l->hasType ? ts_.resolve(l->type, ctx_, diags_)
                                   : kInvalidType;
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
        TypeId pt = ts_.resolve(p.type, ctx_, diags_);
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
      TypeId vt = f->hasVarType ? ts_.resolve(f->varType, ctx_, diags_)
                                : ts_.builtin("i32");
      bind(f->var, vt);
      if (f->hasAcc) {
        TypeId at = ts_.resolve(f->accType, ctx_, diags_);
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
        auto it = out_.functionReturns.find(ctx_.qualify(n->name));
        if (it != out_.functionReturns.end()) return it->second;
      }
      // M.f(x)：被调者是模块成员函数，返回类型就是成员表里那个函数的返回类型。
      if (c->callee && c->callee->kind == ast::ExprKind::Dotted) {
        const auto *d = static_cast<const ast::DottedExpr *>(c->callee);
        if (d->object && d->object->kind == ast::ExprKind::Name) {
          const auto *en = static_cast<const ast::NameExpr *>(d->object);
          auto it = out_.moduleMembers.find(std::string(en->name) + "." +
                                            std::string(d->member));
          if (it != out_.moduleMembers.end() &&
              it->second.kind == ModuleMember::Kind::Func) {
            return it->second.type; // 无返回标注时为 kInvalidType
          }
        }
      }
      return kInvalidType;
    }
    case ast::ExprKind::Cast: {
      const auto *c = static_cast<const ast::CastExpr *>(e);
      (void)typeOf(c->operand, kInvalidType);
      return ts_.resolve(c->target, ctx_, diags_);
    }
    case ast::ExprKind::Construct: {
      const auto *c = static_cast<const ast::ConstructExpr *>(e);
      for (const ast::FieldInit &f : c->fields) (void)typeOf(f.value, kInvalidType);
      return ts_.resolve(c->target, ctx_, diags_);
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
        TypeId pt = ts_.resolve(p.type, ctx_, diags_);
        if (pt != kInvalidType) bind(p.name, pt);
      }
      if (le->cond) (void)typeOf(le->cond, kInvalidType); // 条件里的参数也要登记类型
      walkBody(le->body);
      popScope();
      if (expected != kInvalidType) return expected;
      // 无标注：结果类型取最后一个参数（累加器）的声明类型。
      if (!le->params.empty()) {
        TypeId t = ts_.resolve(le->params.back().type, ctx_, diags_);
        if (t != kInvalidType) return t;
      }
      return kInvalidType;
    }
    case ast::ExprKind::For: {
      const auto *fe = static_cast<const ast::ForExpr *>(e);
      pushScope();
      // 循环变量的类型：显式标注优先，否则从 from 推导。
      TypeId varTy = kInvalidType;
      if (fe->hasVarType) varTy = ts_.resolve(fe->varType, ctx_, diags_);
      if (varTy == kInvalidType && fe->from) varTy = typeOf(fe->from, kInvalidType);
      if (varTy != kInvalidType) bind(fe->var, varTy);
      // 累加器：显式标注优先，否则从初值推导。
      TypeId accTy = kInvalidType;
      if (fe->hasAcc) {
        if (!fe->accType.name.empty()) accTy = ts_.resolve(fe->accType, ctx_, diags_);
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
    case ast::ExprKind::Dotted: {
      // p.x / M.answer：成员类型从聚合布局或模块成员表里查。
      const auto *d = static_cast<const ast::DottedExpr *>(e);
      // 模块成员值访问：M.answer —— object 是模块别名，不要当普通名字去 typeOf
      // （会报「未绑定的名字: M」），直接查 moduleMembers。
      if (d->object && d->object->kind == ast::ExprKind::Name) {
        const auto *en = static_cast<const ast::NameExpr *>(d->object);
        auto it = out_.moduleMembers.find(std::string(en->name) + "." +
                                          std::string(d->member));
        if (it != out_.moduleMembers.end()) return it->second.type;
      }
      (void)typeOf(d->object, kInvalidType);
      const TypeId objTy = typeOf(d->object, kInvalidType);
      if (objTy == kInvalidType) return kInvalidType;
      const AggregateLayout *lay = ts_.layout(objTy);
      if (lay != nullptr) {
        for (const FieldLayout &f : lay->fields) {
          if (f.name == d->member) return f.type;
        }
      }
      return kInvalidType;
    }
    }
    return kInvalidType;
  }

  TypeStore &ts_;
  SemaResult &out_;
  DiagSink &diags_;
  const Program &prog_;
  const ModuleUnit &unit_;
  ModuleContext ctx_{};
  std::vector<std::unordered_map<std::string, TypeId>> scopes_{};
  TypeId funcRet_ = kInvalidType;
};

} // namespace

// ---------------- Program ----------------

ModuleUnit *Program::addUnit(const std::string &path, SourceMgr &src,
                             DiagSink &diags) {
  const std::string norm = normalizePath(path);
  std::string key;
  if (!keyOf(norm, key, Span{}, diags)) return nullptr;
  auto known = index_.find(key);
  if (known != index_.end()) return &units_[known->second]; // 同键只加载一次

  // 文本由 ModuleUnit 自己持一份：token 与 AST 里的 string_view 都指向它。
  // SourceMgr 另存一份副本供诊断定位（它的 files_ 会扩容搬移）。
  const std::string text = readFileText(norm, diags, Span{});
  if (diags.hasErrors()) return nullptr;

  units_.emplace_back();
  ModuleUnit &u = units_.back();
  u.key = key;
  u.path = norm;
  u.dir = directoryOf(norm);
  u.text = text;
  u.fileId = src.addBuffer(norm, std::string(u.text));
  u.toks = lex(u.fileId, u.text, diags);
  if (diags.hasErrors()) return nullptr;
  u.arena = std::make_unique<ast::AstArena>();
  u.ast = parseModule(u.toks, *u.arena, diags);
  if (u.ast == nullptr || diags.hasErrors()) return nullptr;
  u.ctx.key = key;
  u.ctx.sourceRoot = sourceRoot_;
  // §四：`@root` 标注的顶层声明发出平坦名。导出名是**声明自己的属性**，
  // 所以这里从 AST 登记，不再从清单的根名列来。
  for (const ast::Decl *d : u.ast->decls) {
    if (d->isRoot && !d->name.empty()) u.ctx.rootNames.insert(std::string(d->name));
  }
  u.sema.sourcePath = norm;
  index_[key] = units_.size() - 1;
  return &u;
}

bool Program::keyOf(const std::string &normPath, std::string &key, Span sp,
                    DiagSink &diags) const {
  std::string rel;
  if (sourceRoot_.empty() || sourceRoot_ == ".") {
    rel = normPath;
  } else {
    const std::string prefix = sourceRoot_ + "/";
    if (normPath.rfind(prefix, 0) != 0) {
      diags.error(sp, D::SemaBadImport, "导入路径超出源根: " + normPath);
      return false;
    }
    rel = normPath.substr(prefix.size());
  }
  if (rel.rfind("../", 0) == 0 || rel == "..") {
    diags.error(sp, D::SemaBadImport, "导入路径超出源根: " + normPath);
    return false;
  }
  const std::string ext = ".lain";
  if (rel.size() > ext.size() &&
      rel.compare(rel.size() - ext.size(), ext.size(), ext) == 0) {
    rel.resize(rel.size() - ext.size());
  }
  for (char &c : rel) {
    if (c == '/') c = '_';
  }
  if (rel.empty()) {
    diags.error(sp, D::SemaBadImport, "模块键为空: " + normPath);
    return false;
  }
  if (rel.find("__") != std::string::npos) {
    diags.error(sp, D::SemaBadImport, "模块键不能含连续下划线: " + rel);
    return false;
  }
  key = rel;
  return true;
}

bool Program::loadImports(SourceMgr &src, DiagSink &diags) {
  // 宽度优先把整个 import 依赖图加载完；已加载的模块键直接复用。
  for (size_t i = 0; i < units_.size(); ++i) {
    ModuleUnit &u = units_[i];
    for (const ast::Decl *d : u.ast->decls) {
      if (d->kind != ast::DeclKind::Import) continue;
      const auto *im = static_cast<const ast::ImportDecl *>(d);
      const std::string spec =
          resolveModulePath(u.dir, std::string(im->path));
      const std::string depNorm = normalizePath(spec);
      std::string key;
      if (!keyOf(depNorm, key, im->span, diags)) return false;
      u.ctx.aliases[std::string(im->name)] = key;
      u.importOrder.push_back(key);
      if (addUnit(depNorm, src, diags) == nullptr) return false;
    }
  }
  return true;
}

bool Program::load(const std::string &entryPath, SourceMgr &src,
                   DiagSink &diags) {
  const std::string norm = normalizePath(entryPath);
  sourceRoot_ = directoryOf(norm);
  root_ = addUnit(norm, src, diags);
  if (root_ == nullptr) return false;
  return loadImports(src, diags);
}

// §3.3/§3.7：清单每行 `<相对路径> [<根名> …]`，路径相对清单所在目录、省略
// `.lain`，`#` 开头为注释。源根 = 清单所在目录。
// **根名列不再用**（proposal-modules-v2 §四）：导出名改由源码里的 `@root` 标注
// 决定，见 Program::addUnit。整形解析保留在这里，只是为了老清单不改也读得动
// —— 行尾多写几个名字不再有任何效果。
bool Program::loadUnit(const std::string &manifestPath, SourceMgr &src,
                       DiagSink &diags) {
  std::ifstream f(manifestPath, std::ios::binary);
  if (!f) {
    diags.error(Span{}, D::IoOpenFailed, "打不开清单: " + manifestPath);
    return false;
  }
  const std::string norm = normalizePath(manifestPath);
  sourceRoot_ = directoryOf(norm);

  std::string line;
  bool first = true;
  while (std::getline(f, line)) {
    // 去 \r（CRLF）与行内注释：`#` 之后整段丢掉。
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const size_t hash = line.find('#');
    if (hash != std::string::npos) line = line.substr(0, hash);
    // 按空白切：第一段是相对路径，其余是根名。
    std::vector<std::string> fields{};
    std::string cur{};
    auto flush = [&]() {
      if (!cur.empty()) {
        fields.push_back(cur);
        cur.clear();
      }
    };
    for (char c : line) {
      if (c == ' ' || c == '\t') {
        flush();
      } else {
        cur.push_back(c);
      }
    }
    flush();
    if (fields.empty()) continue;

    std::string rel = fields[0];
    const std::string ext = ".lain";
    if (rel.size() <= ext.size() ||
        rel.compare(rel.size() - ext.size(), ext.size(), ext) != 0) {
      rel += ext;
    }
    const std::string path =
        sourceRoot_.empty() || sourceRoot_ == "." ? rel
                                                  : sourceRoot_ + "/" + rel;
    ModuleUnit *u = addUnit(normalizePath(path), src, diags);
    if (u == nullptr) return false;
    // 行尾的字段（过去的根名）在这里被有意丢弃：解析保留，语义不再用。
    // 导出名从源码的 `@root` 标注来（addUnit 已登记）。
    manifestOrder_.push_back(u->key);
    if (first) {
      root_ = u;
      first = false;
    }
  }
  if (root_ == nullptr) {
    diags.error(Span{}, D::IoOpenFailed, "清单里没有模块: " + manifestPath);
    return false;
  }
  return loadImports(src, diags);
}

std::vector<const ModuleUnit *> Program::lowerOrder() const {
  // 清单顺序在前（产物确定性的来源），依赖图里新出现的模块按发现顺序接在后面。
  std::vector<const ModuleUnit *> out{};
  out.reserve(units_.size());
  std::unordered_set<std::string> seen{};
  for (const std::string &k : manifestOrder_) {
    auto it = index_.find(k);
    if (it == index_.end()) continue;
    if (!seen.insert(k).second) continue;
    out.push_back(&units_[it->second]);
  }
  for (const ModuleUnit &u : units_) {
    if (seen.insert(u.key).second) out.push_back(&u);
  }
  return out;
}

bool Program::analyze(TypeStore &ts, DiagSink &diags) {
  ts.initBuiltins();

  // 第一轮：所有模块的 struct/enum 只建名字条目（字段类型可能指向别的模块）。
  LayoutSites sites{};
  for (ModuleUnit &u : units_) {
    for (const ast::Decl *d : u.ast->decls) {
      if (d->kind == ast::DeclKind::Struct) {
        const auto *s = static_cast<const ast::StructDecl *>(d);
        const TypeId id = ts.declareStruct(u.ctx.qualify(s->name));
        sites[id] = LayoutSite{d, &u.ctx, false};
      } else if (d->kind == ast::DeclKind::Enum) {
        const auto *e = static_cast<const ast::EnumDecl *>(d);
        const TypeId id = ts.declareEnum(u.ctx.qualify(e->name));
        sites[id] = LayoutSite{d, &u.ctx, true};
      }
    }
  }

  // 第二轮：算布局（字段类型可跨模块，也可前向引用）。
  for (ModuleUnit &u : units_) {
    for (const ast::Decl *d : u.ast->decls) {
      if (d->kind != ast::DeclKind::Struct && d->kind != ast::DeclKind::Enum)
        continue;
      const TypeId id = ts.lookup(u.ctx.qualify(d->name));
      if (id == kInvalidType) continue;
      std::vector<TypeId> stack{};
      if (!layoutOf(ts, id, sites, stack, diags)) return false;
    }
  }
  if (diags.hasErrors()) return false;

  // 第三轮：先登记每个模块自己的顶层声明，再建 import 成员表 ——
  // 成员表只引用被导入模块的顶层声明，所以一轮就够（互相 import 也不会发散）。
  // Walker 每个模块只建一个：registerTopLevel 绑定的顶层名字必须活到第四轮
  // 走函数体的时候，中途重建 Walker 会把这些绑定丢掉。
  std::deque<Walker> walkers{};
  for (ModuleUnit &u : units_) {
    walkers.emplace_back(ts, u.sema, diags, *this, u);
  }
  for (Walker &w : walkers) {
    w.registerTopLevel();
  }
  if (diags.hasErrors()) return false;

  // §四 防撞：平坦导出名之间、平坦名与 `<键>__<名字>` 之间不许重名。
  // 放在 registerTopLevel 之后 —— 那一步才把每个模块发出的符号名登记齐
  // （含静态数据块的 `<名字>_storage`）。
  if (!checkRootExports(diags)) return false;

  for (Walker &w : walkers) {
    w.registerImports();
  }
  if (diags.hasErrors()) return false;

  // 第四轮：走函数体。
  for (Walker &w : walkers) {
    w.walkBodies();
  }
  return !diags.hasErrors();
}

// §四 防撞。三条不许发生的事，全部在编译期报出来（今天的静默版要跑到
// `start FAILED code 9102` 才炸）：
//   1. 两个模块各有一个同名 `@root`（平坦名撞车）；
//   2. `@root` 平坦名与任何 `<键>__<名字>` 相同（名字空间混叠）；
//   3. 数据型 `@root` 的 `<名字>_storage` 与别的符号同名。
// 判据就是「合并成一个镜像之后，每个名字只有一处定义」，所以在**合并视角**上比：
// 把每个声明发出的符号名收进同一张表，谁撞谁报。宿主 extern 例外 —— 它们本来就
// 发平坦名，且同名 extern 跨模块重复是验证器明确放行的，所以只拿来当「已占用的
// 平坦名」，不参与重复计数。
bool Program::checkRootExports(DiagSink &diags) const {
  // 一条 @root 声明发出的符号名 -> 出处。
  struct Claim {
    std::string moduleKey{};
    std::string rootName{};
    Span span{};
  };
  // 一条非根顶层声明发出的符号名 -> (模块键, 源码名字)。
  struct Mangled {
    std::string moduleKey{};
    std::string srcName{};
  };
  std::unordered_map<std::string, Claim> claimed{};   // 平坦导出名（含 _storage）
  std::unordered_map<std::string, Mangled> mangled{}; // <键>__<名字>（含 _storage）
  std::unordered_set<std::string> externFlat{};       // 宿主 extern 的平坦名
  bool bad = false;

  auto claim = [&](const std::string &symbol, const Claim &c) {
    auto ins = claimed.emplace(symbol, c);
    if (ins.second) return;
    const Claim &prev = ins.first->second;
    std::string msg = "重复的 @root 平坦名 '" + c.rootName + "'（发出符号 '" +
                      symbol + "'）";
    if (prev.moduleKey == c.moduleKey) {
      msg += "：模块 '" + c.moduleKey + "' 里出现了两次";
    } else {
      msg += "：模块 '" + prev.moduleKey + "' 与模块 '" + c.moduleKey +
             "' 各导出一个；平坦导出名与数据符号名都必须全局唯一";
    }
    diags.error(c.span, D::SemaDuplicateDeclaration, std::move(msg));
    bad = true;
  };

  for (const ModuleUnit &u : units_) {
    if (u.ast == nullptr) continue;
    for (const ast::Decl *d : u.ast->decls) {
      if (d->kind == ast::DeclKind::Import) continue;
      if (d->name.empty()) continue;
      if (d->kind == ast::DeclKind::Extern) {
        // 宿主 extern 保持平坦（link_name 决定它在宿主的落点）。
        externFlat.insert(std::string(d->name));
        continue;
      }
      const Span sp = d->rootSpan.valid() ? d->rootSpan : d->span;
      if (d->isRoot) {
        const std::string flat(d->name);
        const Claim c{u.key, flat, sp};
        claim(flat, c);
        // 数据型导出还发 `<名字>_storage`（symbol_addr 认这些）。
        if (u.sema.topLevelData.count(flat) != 0) claim(flat + "_storage", c);
        continue;
      }
      const std::string q = u.ctx.qualify(d->name);
      mangled.emplace(q, Mangled{u.key, std::string(d->name)});
      if (u.sema.topLevelData.count(q) != 0) {
        mangled.emplace(q + "_storage", Mangled{u.key, std::string(d->name)});
      }
    }
  }

  for (const auto &kv : claimed) {
    auto m = mangled.find(kv.first);
    if (m != mangled.end()) {
      diags.error(kv.second.span, D::SemaDuplicateDeclaration,
                  "'" + kv.first + "' 既是模块 '" + kv.second.moduleKey +
                      "' 的 @root 平坦导出名，又与模块 '" + m->second.moduleKey +
                      "' 的顶层声明 '" + m->second.srcName + "' 发出的 '" + kv.first +
                      "' 同名；平坦导出名不得与任何 '<键>__<名字>' 相同");
      bad = true;
    }
    if (externFlat.count(kv.first) != 0) {
      diags.error(kv.second.span, D::SemaDuplicateDeclaration,
                  "'" + kv.first + "' 是模块 '" + kv.second.moduleKey +
                      "' 的 @root 平坦导出名，又与一条宿主 extern 同名");
      bad = true;
    }
  }
  return !bad;
}

const ModuleUnit *Program::find(const std::string &key) const {
  auto it = index_.find(key);
  return it == index_.end() ? nullptr : &units_[it->second];
}

} // namespace ln
