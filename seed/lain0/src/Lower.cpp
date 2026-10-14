// Lowerer 的骨架：构造器、模块组装、以及三个 .cpp 共用的小工具。
//
// 这里也是「产物必经验证」这条纪律的落点：run() 只负责把 AST 装成
// L1Builder 里的内存模块，打印与验证都在 Driver 里，且顺序不可调换。

#include "lain0/Lower.hpp"

#include <string>
#include <utility>

namespace ln {

Lowerer::Lowerer(L1Builder *builder, const TypeStore &types,
                 const SemaResult &sema, const SourceMgr &src, DiagSink &diags,
                 const LowerOptions &opts)
    : builder_(builder), types_(&types), sema_(&sema), src_(&src),
      diags_(&diags), opts_(opts),
      srcPath_(opts.sourcePath ? opts.sourcePath : "") {}

const L1Module *Lowerer::run(const ast::Module &mod) {
  data_.clear();
  subs_.clear();
  tempCounter_ = 0;
  strlitCounter_ = 0;

  for (const ast::Decl *d : mod.decls) {
    if (!lowerDecl(d)) return nullptr;
  }
  if (diags_->hasErrors()) return nullptr;

  std::string_view moduleName = mod.name.empty() ? std::string_view{opts_.moduleName}
                                                 : mod.name;
  return lainir_module(builder_, intern(moduleName),
                       data_.empty() ? nullptr : data_.data(),
                       static_cast<uint32_t>(data_.size()),
                       subs_.empty() ? nullptr : subs_.data(),
                       static_cast<uint32_t>(subs_.size()));
}

const char *Lowerer::nextTemp() {
  std::string name = std::string(opts_.tempPrefix) + std::to_string(++tempCounter_);
  return intern(name);
}

const char *Lowerer::intern(std::string_view s) {
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
