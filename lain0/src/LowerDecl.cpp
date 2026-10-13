
// 顶层声明降级：func / extern / struct / enum / 顶层 let（字节串、定长表、聚合构造）。
//
// 三条约束决定了这里的形状：
//   * 一切 IR 只能经 Seed.hpp 的构造 API 产生，源码里不许出现 \`#\` 形态的文本；
//   * 静态数据块只能落顶层（发射是流式的），所以 data 块与「取址过程」成对出现 ——
//     取址过程是把块地址交给表达式层的唯一通道；
//   * 每条指令都要自带 line/column（构造 API 没有这个入口，只能建完就地补写），
//     否则验证器报出来的错全部落在 0:0。
//
// 临时值名字一律不带 \`%\`：文本层打印时自己补一个（print_operand 的 \`%%%s\`），
// 写进去的必须是裸标识符，否则产物里会出现 \`%%r1\` 这种读不回来的名字。

#include "lain0/Lower.hpp"

#include <fstream>
#include <sstream>
#include <vector>

#include "lain0/Ast.hpp"
#include "lain0/Diag.hpp"
#include "lain0/Lexer.hpp"
#include "lain0/Parser.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ln {
namespace {

// 静态块的最小可寻址单位是字节：布局给的是位宽，块尺寸要按字节算。
uint32_t bytesOf(uint32_t bits) { return bits == 0 ? 1u : (bits + 7u) / 8u; }

void stamp(const L1Inst *inst, uint32_t line, uint32_t column) {
  if (inst == nullptr) return;
  L1Inst *mutableInst = const_cast<L1Inst *>(inst);
  mutableInst->line = line;
  mutableInst->column = column;
}

const L1Inst *emit(L1Builder *builder, L1InstKind kind, const char *result,
                   const L1Type *ty, const L1Operand *ops, uint32_t count,
                   uint32_t line, uint32_t column) {
  const L1Inst *inst = lainir_inst(builder, kind, result, ty, ops, count);
  stamp(inst, line, column);
  return inst;
}

const L1Inst *emitMem(L1Builder *builder, L1InstKind kind, const char *result,
                      const L1Type *ty, const L1Operand *ops, uint32_t count,
                      uint32_t line, uint32_t column) {
  const L1Inst *inst = lainir_inst_mem(builder, kind, result, ty, ORDER_RELAXED,
                                       false, ops, count);
  stamp(inst, line, column);
  return inst;
}

const L1Inst *emitDataAddr(L1Builder *builder, const char *result,
                           const char *symbol, uint32_t line, uint32_t column) {
  // D-002：数据符号本身就是地址；用 #bitcast[#addr] 包住对符号的引用即得地址。
  L1Operand sym = lainir_ref(symbol);
  const L1Type *addrTy = lainir_type(builder, TY_ADDR, 0);
  const L1Inst *inst =
      lainir_inst(builder, INST_BITCAST, result, addrTy, &sym, 1);
  stamp(inst, line, column);
  return inst;
}

// 取址过程：\`#proc NAME() -> #addr { %base = #data_addr SYMBOL; <额外指令…>; #return %base }\`。
// 额外指令是「字段值不是字面量」时补的 store —— 它们写在同一个过程里，所以每个
// 构造点都要拿自己的临时名（装载器按名字取第一个，重名会静默指向前一个）。
const L1Subroutine *buildAccessor(L1Builder *builder, const char *procName,
                                  const char *symbol, const char *baseName,
                                  const std::vector<const L1Inst *> &extra,
                                  uint32_t line, uint32_t column) {
  const L1Type *addrTy = lainir_type(builder, TY_ADDR, 0);
  std::vector<const L1Inst *> insts{};
  insts.push_back(emitDataAddr(builder, baseName, symbol, line, column));
  for (const L1Inst *extra_inst : extra) {
    insts.push_back(extra_inst);
  }
  L1Operand base = lainir_ref(baseName);
  insts.push_back(emit(builder, INST_RETURN, nullptr, nullptr, &base, 1, line, column));

  const L1Region *body =
      lainir_region(builder, nullptr, 0, nullptr, 0, insts.data(),
                    static_cast<uint32_t>(insts.size()));
  const L1Type *results[1] = {addrTy};
  return lainir_subroutine(builder, procName, nullptr, 0, results, 1, body);
}

// 常量折叠：只认整数字面量、布尔字面量与「负号 + 字面量」。
// 顶层静态块的字节必须在降级时就定下来，算不出来的字段改走过程里的 store。
bool constInt(const ast::Expr *e, uint64_t &out) {
  if (e == nullptr) return false;
  switch (e->kind) {
  case ast::ExprKind::IntLit:
    out = static_cast<const ast::IntLitExpr *>(e)->value;
    return true;
  case ast::ExprKind::BoolLit:
    out = static_cast<const ast::BoolLitExpr *>(e)->value ? 1u : 0u;
    return true;
  case ast::ExprKind::Unary: {
    const auto *u = static_cast<const ast::UnaryExpr *>(e);
    if (u->op != ast::UnOp::Neg) return false;
    uint64_t magnitude = 0;
    if (!constInt(u->operand, magnitude)) return false;
    out = ~magnitude + 1u; // 块里存的就是二补的位模式
    return true;
  }
  default:
    return false;
  }
}

void putLittleEndian(std::vector<uint8_t> &bytes, uint32_t offset,
                     uint32_t count, uint64_t value) {
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t at = offset + i;
    if (at >= bytes.size()) return;
    bytes[at] = static_cast<uint8_t>((value >> (8u * i)) & 0xFFu);
  }
}

// #store / #load 的类型实参必须是字节整数倍（验证器的 byte_addressable），
// 1 位的 bool 与聚合（addr）都要在这里换掉。
const L1Type *storageTypeOf(L1Builder *builder, const TypeStore &ts, TypeId id) {
  if (id == kInvalidType) return lainir_type(builder, TY_BITS, 8);
  if (ts.isAddr(id) || ts.isAggregate(id) || ts.isModuleMemberType(id)) {
    return lainir_type(builder, TY_ADDR, 0);
  }
  uint32_t width = ts.width(id);
  if (width == 0) width = 8;
  if (width % 8u != 0u) width = bytesOf(width) * 8u;
  if (width > 64u) width = 64u;
  return lainir_type(builder, TY_BITS, width);
}

// 声明里的类型标注 -> IR 类型；聚合在 IR 里就是一个地址。
const L1Type *signatureTypeOf(L1Builder *builder, const TypeStore &ts, TypeId id) {
  const L1Type *ty = ts.lain(id);
  if (ty != nullptr) return ty;
  return lainir_type(builder, TY_ADDR, 0);
}

} // namespace

bool Lowerer::lowerDecl(const ast::Decl *d) {
  if (d == nullptr) return false;

  switch (d->kind) {
  case ast::DeclKind::Func: {
    // 名字绑定是「每个过程一份」：不清空的话上一个函数的局部 let 会漏进来，
    // 下一个函数里同名的参数就会引用到本过程根本不存在的临时值
    // （验证器报 2002 undefined value）。
    bindings_.clear();
    L1Subroutine *sub = nullptr;
    if (!lowerFunc(static_cast<const ast::FuncDecl *>(d), sub)) return false;
    if (sub != nullptr) subs_.push_back(*sub);
    return true;
  }
  case ast::DeclKind::Extern: {
    L1Subroutine *sub = nullptr;
    if (!lowerExtern(static_cast<const ast::ExternDecl *>(d), sub)) return false;
    if (sub != nullptr) subs_.push_back(*sub);
    return true;
  }
  case ast::DeclKind::Struct:
    return lowerStruct(static_cast<const ast::StructDecl *>(d));
  case ast::DeclKind::Enum:
    return lowerEnum(static_cast<const ast::EnumDecl *>(d));
  case ast::DeclKind::Import:
    return lowerImport(static_cast<const ast::ImportDecl *>(d), subs_);
  case ast::DeclKind::StaticLet:
    return lowerStaticLet(static_cast<const ast::StaticLetDecl *>(d), data_, subs_);
  }
  fail(d->span, D::LowerUnsupported, "未知的顶层声明形态");
  return false;
}

bool Lowerer::lowerFunc(const ast::FuncDecl *f, L1Subroutine *&outSub) {
  outSub = nullptr;
  if (f == nullptr) return false;

  std::vector<L1Param> params{};
  params.reserve(f->params.size());
  for (const ast::ParamDecl &p : f->params) {
    const TypeId pt = types_->resolve(p.type, *diags_);
    if (pt == kInvalidType) return false;
    params.push_back(lainir_proc_param(intern(p.name),
                                       signatureTypeOf(builder_, *types_, pt)));
  }

  TypeId retTy = kInvalidType;
  if (f->hasRet) {
    retTy = types_->resolve(f->ret, *diags_);
    if (retTy == kInvalidType) return false;
  }

  std::vector<const L1Inst *> insts{};
  bool terminated = false;
  if (!lowerBody(f->body, insts, retTy, terminated)) return false;

  const uint32_t line = lineOf(f->span);
  const uint32_t column = columnOf(f->span);
  if (!terminated) {
    // 落尾没有终结子：无返回值补一条裸 #return，有返回值就是「函数体没交值」。
    if (f->hasRet) {
      fail(f->span, D::LowerNoReturnValue,
           std::string("函数 ") + std::string(f->name) + " 的体没有以 return 收尾");
      return false;
    }
    insts.push_back(emit(builder_, INST_RETURN, nullptr, nullptr, nullptr, 0,
                         line, column));
  }

  const L1Region *body =
      lainir_region(builder_, nullptr, 0, nullptr, 0,
                    insts.empty() ? nullptr : insts.data(),
                    static_cast<uint32_t>(insts.size()));
  if (body == nullptr) {
    fail(f->span, D::LowerUnsupported, "函数体区域构造失败");
    return false;
  }

  const L1Type *results[1] = {nullptr};
  uint32_t resultCount = 0;
  if (f->hasRet) {
    results[0] = signatureTypeOf(builder_, *types_, retTy);
    resultCount = 1;
  }

  const L1Subroutine *sub =
      lainir_subroutine(builder_, intern(f->name),
                        params.empty() ? nullptr : params.data(),
                        static_cast<uint32_t>(params.size()), results,
                        resultCount, body);
  if (sub == nullptr) {
    fail(f->span, D::LowerUnsupported, "过程构造失败");
    return false;
  }
  outSub = const_cast<L1Subroutine *>(sub);
  return true;
}

bool Lowerer::lowerExtern(const ast::ExternDecl *e, L1Subroutine *&outSub) {
  outSub = nullptr;
  if (e == nullptr) return false;

  std::vector<L1Param> params{};
  params.reserve(e->params.size());
  for (const ast::ParamDecl &p : e->params) {
    const TypeId pt = types_->resolve(p.type, *diags_);
    if (pt == kInvalidType) return false;
    params.push_back(lainir_proc_param(intern(p.name),
                                       signatureTypeOf(builder_, *types_, pt)));
  }

  TypeId retTy = kInvalidType;
  if (e->hasRet) {
    retTy = types_->resolve(e->ret, *diags_);
    if (retTy == kInvalidType) return false;
  }

  const L1Type *results[1] = {nullptr};
  uint32_t resultCount = 0;
  if (e->hasRet) {
    results[0] = signatureTypeOf(builder_, *types_, retTy);
    resultCount = 1;
  }

  // 链接名原样搬：它既是能力名也是宿主那头的符号名，不许改写。
  const L1Subroutine *sub = lainir_subroutine_extern(
      builder_, intern(e->name), intern(e->linkName),
      params.empty() ? nullptr : params.data(),
      static_cast<uint32_t>(params.size()), results, resultCount);
  if (sub == nullptr) {
    fail(e->span, D::LowerUnsupported, "外部过程构造失败");
    return false;
  }
  outSub = const_cast<L1Subroutine *>(sub);
  return true;
}

bool Lowerer::lowerStruct(const ast::StructDecl *s) {
  // 布局是 Sema 的事（字段偏移与总尺寸），降级这里不产出任何 IR 项 ——
  // 只有「构造一个值」才需要发东西。
  (void)s;
  return true;
}

bool Lowerer::lowerEnum(const ast::EnumDecl *e) {
  (void)e;
  return true;
}

// 模块路径解析 + 加载（读文件、lex、parse）。与 Sema.cpp 里的版本同源，单发工具不释放 arena。
// 注意：parseModuleText 返回的 AST 里 token 文本是 string_view，指向传入的 text，
// 调用方必须让 text 活过 lower，否则类型名变悬垂指针（乱码）。
static std::string readFileText(const std::string &filePath, DiagSink &diags) {
  std::ifstream f(filePath);
  if (!f) {
    diags.error(Span{}, D::IoOpenFailed, "打不开导入模块: " + filePath);
    return "";
  }
  return std::string((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
}
static ast::Module *parseModuleText(const std::string &filePath,
                                    const std::string &text, DiagSink &diags) {
  ast::AstArena *arena = new ast::AstArena();
  std::vector<Token> toks = lex(0, text, diags);
  if (diags.hasErrors()) return nullptr;
  ast::Module *m = parseModule(toks, *arena, diags);
  if (diags.hasErrors()) return nullptr;
  return m;
}

static std::string directoryOf(const std::string &path) {
  const size_t pos = path.find_last_of('/');
  return pos == std::string::npos ? "." : path.substr(0, pos);
}
static std::string resolveModulePath(const std::string &dir,
                                    const std::string &spec) {
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

bool Lowerer::lowerImport(const ast::ImportDecl *d,
                          std::vector<L1Subroutine> &subsOut) {
  (void)subsOut;
  // 真正加载被导入的模块，把它的导出声明（顶层 let -> 访问器过程、struct/enum
  // -> 类型声明）降级进当前模块的 IR，于是 M.answer 发的 #call @answer 才有目标。
  // 被导入模块的类型（Point/Shape）在 Sema 的 import 分支里已经登记进同一套
  // TypeStore，这里不复用 analyze —— 直接 lower 声明即可。
  // 读进来的源文本要活在 lower 期间（token 文本是 string_view，指向它）。
  const std::string dir = directoryOf(srcPath_);
  const std::string filePath = resolveModulePath(dir, std::string(d->path));
  const std::string imodText = readFileText(filePath, *diags_);
  ast::Module *imod = parseModuleText(filePath, imodText, *diags_);
  if (imod == nullptr) return false;
  for (const ast::Decl *decl : imod->decls) {
    if (decl->kind == ast::DeclKind::StaticLet ||
        decl->kind == ast::DeclKind::Struct ||
        decl->kind == ast::DeclKind::Enum) {
      if (!lowerDecl(decl)) return false;
    }
  }
  return true;
}

bool Lowerer::lowerStaticLet(const ast::StaticLetDecl *d,
                             std::vector<L1Data> &dataOut,
                             std::vector<L1Subroutine> &subsOut) {
  if (d == nullptr) return false;

  const uint32_t line = lineOf(d->span);
  const uint32_t column = columnOf(d->span);
  const ast::Expr *init = d->init;
  if (init == nullptr) {
    fail(d->span, D::LowerUnsupported,
         std::string("顶层绑定 ") + std::string(d->name) + " 没有初值");
    return false;
  }

  const std::string storageName = std::string(d->name) + "_storage";
  const char *symbol = intern(storageName);
  const char *procName = intern(d->name);

  // 三种静态块共用的收尾：发 data 块 + 取址过程。
  auto publish = [&](std::vector<uint8_t> &bytes, bool writable,
                     const std::vector<const L1Inst *> &extra) {
    const L1Data *block =
        lainir_data(builder_, symbol, bytes.empty() ? nullptr : bytes.data(),
                    static_cast<uint32_t>(bytes.size()), writable);
    if (block == nullptr) {
      fail(d->span, D::LowerUnsupported, "静态数据块构造失败");
      return false;
    }
    dataOut.push_back(*block);
    const L1Subroutine *accessor = buildAccessor(
        builder_, procName, symbol, intern("base"), extra, line, column);
    if (accessor == nullptr) {
      fail(d->span, D::LowerUnsupported, "取址过程构造失败");
      return false;
    }
    subsOut.push_back(*accessor);
    return true;
  };

  // --- 字节串：字节按源码原样搬，不补末尾 0 ---
  if (init->kind == ast::ExprKind::StrLit) {
    const std::string_view raw =
        static_cast<const ast::StrLitExpr *>(init)->rawText;
    std::vector<uint8_t> bytes(raw.begin(), raw.end());
    return publish(bytes, false, {});
  }

  // --- 定长同质表：元素按声明宽度小端拆字节 ---
  if (init->kind == ast::ExprKind::Table) {
    const auto *table = static_cast<const ast::TableExpr *>(init);
    TypeId elemTy = kInvalidType;
    if (d->hasType && d->type.isArray && d->type.element != nullptr) {
      elemTy = types_->resolve(*d->type.element, *diags_);
    }
    if (elemTy == kInvalidType) {
      elemTy = types_->builtin("u64"); // 缺标注按 u64（docs/04 的口径）
    }
    const uint32_t width = types_->width(elemTy);
    if (width == 0 || width > 64) {
      fail(d->span, D::SemaWidthUnknown,
           std::string("定长表的元素宽度取不到: ") +
               std::string(types_->name(elemTy)));
      return false;
    }
    const uint32_t elemBytes = bytesOf(width);

    std::vector<uint8_t> bytes{};
    bytes.resize(static_cast<size_t>(elemBytes) * table->elements.size());
    for (size_t i = 0; i < table->elements.size(); ++i) {
      uint64_t value = 0;
      if (!constInt(table->elements[i], value)) {
        fail(table->elements[i] ? table->elements[i]->span : d->span,
             D::LowerUnsupported, "定长表的元素必须是整数字面量");
        return false;
      }
      if (width < 64u && (value >> width) != 0u) {
        fail(table->elements[i]->span, D::LowerUnsupported,
             "定长表的元素装不进声明宽度 " + std::to_string(width));
        return false;
      }
      putLittleEndian(bytes, static_cast<uint32_t>(i * elemBytes), elemBytes,
                      value);
    }
    // ro [T] 出只读块，其余可写（docs/04：表默认 rw）。
    return publish(bytes, !d->isRo, {});
  }

  // --- 顶层聚合构造：一块静态 storage + 一个取址过程 ---
  if (init->kind == ast::ExprKind::Construct) {
    const auto *ctor = static_cast<const ast::ConstructExpr *>(init);
    TypeId ty = d->hasType ? types_->resolve(d->type, *diags_)
                           : types_->resolve(ctor->target, *diags_);
    if (ty == kInvalidType) return false;
    const AggregateLayout *layout = types_->layout(ty);
    if (layout == nullptr || layout->size == 0) {
      fail(d->span, D::SemaUnknownType,
           std::string("顶层聚合构造拿不到布局: ") +
               std::string(types_->name(ty)));
      return false;
    }

    std::vector<uint8_t> bytes(layout->size, 0);
    std::vector<const L1Inst *> extra{};
    const char *base = intern("base");
    uint32_t tempSeq = 0;
    // 每个字段一个自己的临时名：同一个过程里重名会静默指向前一个。
    auto nextLocal = [&]() {
      return intern("t" + std::to_string(tempSeq++));
    };
    // 字段值算不出常量时退化成过程里的 store（docs/04：字段值可以是表达式与调用）。
    auto storeIntoExtra = [&](uint32_t offset, TypeId valueTy,
                              const ast::Expr *value) -> bool {
      L1Operand addr = lainir_ref(base);
      L1Operand leaOps[4] = {addr, lainir_int(0), lainir_int(0),
                             lainir_int(offset)};
      const char *slot = nextLocal();
      extra.push_back(emit(builder_, INST_LEA, slot, nullptr, leaOps, 4, line,
                           column));
      L1Operand val{};
      if (!lowerExpr(value, valueTy, extra, val)) return false;
      L1Operand storeOps[2] = {val, lainir_ref(slot)};
      extra.push_back(emitMem(builder_, INST_STORE, nullptr,
                              storageTypeOf(builder_, *types_, valueTy),
                              storeOps, 2, line, column));
      return true;
    };

    if (!layout->variants.empty()) {
      // 变体构造：tag 在前，载荷按布局给的偏移。
      const VariantLayout *variant = nullptr;
      for (const VariantLayout &v : layout->variants) {
        if (v.name == ctor->target.name) {
          variant = &v;
          break;
        }
      }
      if (variant == nullptr) {
        fail(ctor->span, D::SemaUnknownMember,
             std::string("未知变体: ") + std::string(ctor->target.name));
        return false;
      }
      putLittleEndian(bytes, 0, 1, variant->tag);
      if (variant->payload != kInvalidType && !ctor->fields.empty()) {
        const ast::FieldInit &payload = ctor->fields.front();
        uint64_t value = 0;
        if (constInt(payload.value, value)) {
          putLittleEndian(bytes, variant->payloadOffset,
                          bytesOf(types_->width(variant->payload)), value);
        } else if (!storeIntoExtra(variant->payloadOffset, variant->payload,
                                   payload.value)) {
          return false;
        }
      }
      return publish(bytes, !d->isRo, extra);
    }

    for (size_t i = 0; i < ctor->fields.size(); ++i) {
      const ast::FieldInit &field = ctor->fields[i];
      const FieldLayout *target = nullptr;
      if (!field.name.empty()) {
        for (const FieldLayout &fl : layout->fields) {
          if (fl.name == field.name) {
            target = &fl;
            break;
          }
        }
      } else if (i < layout->fields.size()) {
        target = &layout->fields[i]; // 位置式写法
      }
      if (target == nullptr) {
        fail(field.span, D::SemaUnknownMember,
             std::string("未知字段: ") + std::string(field.name));
        return false;
      }
      uint64_t value = 0;
      if (constInt(field.value, value)) {
        putLittleEndian(bytes, target->offset, bytesOf(types_->width(target->type)),
                        value);
      } else if (!storeIntoExtra(target->offset, target->type, field.value)) {
        return false;
      }
    }
    return publish(bytes, !d->isRo, extra);
  }

  // --- 其余顶层绑定：发一个取值过程（\`let main: i32 = add(3, 4);\` 这一类）---
  TypeId ty = d->hasType ? types_->resolve(d->type, *diags_) : kInvalidType;
  if (ty == kInvalidType) {
    auto it = sema_->topLevelTypes.find(std::string(d->name));
    if (it != sema_->topLevelTypes.end()) ty = it->second;
  }
  if (ty == kInvalidType) {
    fail(d->span, D::SemaWidthUnknown,
         std::string("顶层绑定 ") + std::string(d->name) + " 的类型取不到");
    return false;
  }

  std::vector<const L1Inst *> insts{};
  L1Operand value{};
  if (!lowerExpr(init, ty, insts, value)) return false;
  if (value.kind == OPERAND_VALUE && value.name == nullptr) {
    fail(init->span, D::LowerUnsupported, "顶层绑定的初值没有产生可用的值");
    return false;
  }
  insts.push_back(emit(builder_, INST_RETURN, nullptr, nullptr, &value, 1, line,
                       column));

  const L1Region *body =
      lainir_region(builder_, nullptr, 0, nullptr, 0, insts.data(),
                    static_cast<uint32_t>(insts.size()));
  const L1Type *results[1] = {signatureTypeOf(builder_, *types_, ty)};
  const L1Subroutine *sub =
      lainir_subroutine(builder_, procName, nullptr, 0, results, 1, body);
  if (sub == nullptr) {
    fail(d->span, D::LowerUnsupported, "顶层绑定过程构造失败");
    return false;
  }
  subsOut.push_back(*sub);
  return true;
}

} // namespace ln
