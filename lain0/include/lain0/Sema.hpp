#ifndef LAIN0_SEMA_HPP
#define LAIN0_SEMA_HPP

// 语义层：把语法形状翻译成可降级的结论。
//
// 三类结论：
//   1. 每个表达式的类型（TypeId）            —— 降级选算子全靠它
//   2. 聚合类型（struct / enum）的布局        —— 字段偏移、总尺寸、变体 tag 与载荷偏移
//   3. 作用域绑定（名字 → 类型）              —— 参数、局部 let、循环参数、import 成员

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "lain0/Ast.hpp"
#include "lain0/Diag.hpp"
#include "lain0/Seed.hpp"

namespace ln {

using TypeId = uint32_t;

enum : TypeId {
  kInvalidType = 0,
  kFirstBuiltin = 1,
};

struct FieldLayout {
  std::string_view name{};
  TypeId type = kInvalidType;
  uint32_t offset = 0;
};

struct VariantLayout {
  std::string_view name{};
  uint64_t tag = 0;
  TypeId payload = kInvalidType; // 空变体为 kInvalidType
  uint32_t payloadOffset = 0;
};

struct AggregateLayout {
  // struct：逐 field 累加；enum：tag 在前，payload 在后。
  uint32_t size = 0;
  uint32_t align = 1;
  std::vector<FieldLayout> fields{};       // struct
  std::vector<VariantLayout> variants{};   // enum
};

// 已导入模块的成员：`M.answer` 的值绑定是一个顶层 proc，
// `M.Point` 的类型要在被导入源码的类型表里查。
struct ModuleMember {
  enum class Kind { Value, Type, Func } kind = Kind::Value;
  std::string_view name{};
  TypeId type = kInvalidType;
  std::string mangled{}; // 值绑定：`<mod>__<name>` 的过程名（owning，因为登记时
                         // 来自临时 SemaResult 的 key，不能留 string_view 悬垂）
};

class TypeStore {
public:
  // 预置全部标量名 —— 与 docs/04《A. 算子表》同一份名单：
  //   i32 u32 i64 u64 i8 u8 usize bool addr
  // i8/u8 只作 load/store 宽度，**不绑定算子**。
  void initBuiltins();

  TypeId builtin(std::string_view name) const;
  TypeId addStruct(std::string_view name, AggregateLayout layout);
  TypeId addEnum(std::string_view name, AggregateLayout layout);
  // 数组类型可能被重复解析，登记是幂等的；resolve 是 const，所以这个也必须是。
  // 按名字查已登记的类型（enum 名 / struct 名）；没有返回 kInvalidType。
  TypeId lookup(std::string_view name) const;
  // 数组的元素类型（取不到返回 kInvalidType）。
  TypeId elementOf(TypeId id) const;
  TypeId addArrayOf(TypeId element, bool isRo) const;
  TypeId addModuleType(std::string_view moduleAlias);

  // 解析源码里的类型标注。失败返回 kInvalidType 并写诊断。
  TypeId resolve(const ast::TypeSyntax &ts, DiagSink &diags) const;

  const L1Type *lain(TypeId id) const; // 聚合/模块类型返回 nullptr
  bool isAggregate(TypeId id) const;
  bool isModuleMemberType(TypeId id) const;
  bool isAddr(TypeId id) const;
  bool isBits(TypeId id) const;
  uint32_t width(TypeId id) const; // bits 宽度；非 bits 返回 0
  bool signedness(TypeId id) const;

  std::string_view name(TypeId id) const;
  const AggregateLayout *layout(TypeId id) const;

private:
  struct Entry {
    std::string name{};
    const L1Type *lain = nullptr; // nullptr 表示非标量（聚合 / 模块 / 数组）
    enum class K { Bits, Addr, Bool, Struct, Enum, Array, Module } k = K::Bits;
    bool signedInt = false;
    bool roArray = false;
    TypeId element = kInvalidType;
    AggregateLayout layout{};
  };
  // entries_ 用 deque：vector 扩容会搬移元素，而 name() 返回的是指进元素内部的
  // string_view，lain() 返回的是指进元素的指针——两者都会失效。
  // mutable：const 的 resolve/addArrayOf 也要能登记新类型。
  mutable std::deque<Entry> entries_; // entries_[i] 对应 id i+1
  std::unordered_map<std::string, TypeId> byName_{};
  // resolve 是 const（Lowerer 持有 const TypeStore&），但解析数组类型时要登记，
  // 所以这张表必须可写。
  mutable std::unordered_map<uint64_t, TypeId> arrays_{}; // (element<<1|ro) -> id
};

// 算子选择：给定操作数类型，决定发哪一条 LAINIR 指令。
// 这就是 docs/04 里那份「算子表」在 lain0 里的形态 —— 不再是手动维护的字节流。
struct OperatorChoice {
  L1InstKind kind = INST_COUNT; // 未命中保持 INST_COUNT
  const L1Type *operandTy = nullptr;
  const L1Type *resultTy = nullptr;
};

// 比较类算子的结果恒为 #bits<1>，其余跟随操作数宽度。
bool selectBinary(ast::BinOp op, TypeId operandTy, const TypeStore &ts,
                  OperatorChoice &out);

// 表达式 / 绑定的类型结论。
using ExprTypeMap = std::unordered_map<const ast::Expr *, TypeId>;
using NameTypeMap = std::unordered_map<std::string, TypeId>;

struct SemaResult {
  ExprTypeMap exprTypes{};
  // 顶层函数的返回值类型（供无标注 let 推导）
  std::unordered_map<std::string, TypeId> functionReturns{};
  // 顶层静态绑定的类型
  std::unordered_map<std::string, TypeId> topLevelTypes{};
  // 顶层表/串的元素个数（len(NAME) 要用）：顶层静态绑定的长度编译期可知。
  std::unordered_map<std::string, uint64_t> topLevelLength{};
  // import 出来的模块成员：`<alias>.<member>` -> ModuleMember
  std::unordered_map<std::string, ModuleMember> moduleMembers{};
  // 主源文件完整路径：import 解析被导入模块的相对路径要用到。
  std::string sourcePath{};
};

bool analyze(const ast::Module &mod, TypeStore &ts, SemaResult &out,
             DiagSink &diags);

} // namespace ln

#endif
