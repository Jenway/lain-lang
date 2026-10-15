// Lowerer 的骨架：构造器、模块循环、以及三个 .cpp 共用的小工具。
//
// 这里也是「产物必经验证」这条纪律的落点：run() 只负责把 AST 装成
// L1Builder 里的内存模块，打印与验证都在 Driver 里，且顺序不可调换。
//
// 模块循环（docs/plan-modules.md §3.4）：按 Program 的模块顺序依次降级，
// 全部模块的数据块与过程追加进**同一个** L1Builder，末尾建一个
// lainir_module（模块名取根模块键）。每进一个模块换一次 ModuleContext
// 与 SemaResult —— 顶层 func / 静态 let 发出的名字是 `<模块键>__<名字>`，
// 宿主 extern 保持平坦。

#include "lain0/Lower.hpp"

#include <string>
#include <utility>

namespace ln {

Lowerer::Lowerer(L1Builder *builder, const TypeStore &types,
                 const SourceMgr &src, DiagSink &diags, const LowerOptions &opts)
    : builder_(builder), types_(&types), src_(&src), diags_(&diags),
      opts_(opts) {}

const L1Module *Lowerer::run(const Program &prog) {
  data_.clear();
  subs_.clear();
  tempCounter_ = 0;
  strlitCounter_ = 0;

  // §3.4：清单顺序在前（产物确定性的来源），依赖图里新出现的模块接在后面。
  for (const ModuleUnit *u : prog.lowerOrder()) {
    if (u == nullptr || u->ast == nullptr) return nullptr;
    // 换模块上下文：类型键、符号名、成员表全部跟着换。
    ctx_ = u->ctx;
    sema_ = &u->sema;
    bindings_.clear();
    loopParams_.clear();
    for (const ast::Decl *d : u->ast->decls) {
      // import 的加载与解析是 Program 的事，降级这里不再重复。
      if (d->kind == ast::DeclKind::Import) continue;
      if (!lowerDecl(d)) return nullptr;
    }
    if (diags_->hasErrors()) return nullptr;
  }

  const ModuleUnit *root = prog.root();
  std::string_view moduleName =
      (root != nullptr && !root->key.empty())
          ? std::string_view{root->key}
          : std::string_view{opts_.moduleName};
  return lainir_module(builder_, intern(moduleName),
                       data_.empty() ? nullptr : data_.data(),
                       static_cast<uint32_t>(data_.size()),
                       subs_.empty() ? nullptr : subs_.data(),
                       static_cast<uint32_t>(subs_.size()));
}

const char *Lowerer::symbolFor(std::string_view srcName) const {
  if (sema_ == nullptr) return intern(srcName);
  auto it = sema_->topLevelSymbols.find(ctx_.qualify(srcName));
  if (it == sema_->topLevelSymbols.end()) return intern(srcName);
  return intern(it->second);
}

TypeId Lowerer::resolveTy(const ast::TypeSyntax &ts) const {
  return types_->resolve(ts, ctx_, *diags_);
}

TypeId Lowerer::lookupTy(std::string_view name) const {
  return types_->lookup(ctx_.qualify(name));
}

const char *Lowerer::nextTemp() {
  std::string name = std::string(opts_.tempPrefix) + std::to_string(++tempCounter_);
  return intern(name);
}

const char *Lowerer::intern(std::string_view s) const {
  // lainir_builder_string 需要 NUL 结尾的 C 串，string_view 不保证。
  std::string tmp(s);
  return lainir_builder_string(builder_, tmp.c_str());
}

uint32_t Lowerer::lineOf(Span s) const {
  if (!s.valid()) return 0;
  return src_->position(s.file, s.begin).line;
}

uint32_t Lowerer::columnOf(Span s) const {
  if (!s.valid()) return 0;
  return src_->position(s.file, s.begin).col;
}

void Lowerer::fail(Span s, D code, std::string msg) {
  diags_->error(s, code, std::move(msg));
}

} // namespace ln
