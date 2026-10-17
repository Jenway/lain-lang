#ifndef LAIN0_SEMA_HPP
#define LAIN0_SEMA_HPP

// 语义层：把语法形状翻译成可降级的结论。
//
// 三类结论：
//   1. 每个表达式的类型（TypeId）            —— 降级选算子全靠它
//   2. 聚合类型（struct / enum）的布局        —— 字段偏移、总尺寸、变体 tag 与载荷偏移
//   3. 作用域绑定（名字 → 类型）              —— 参数、局部 let、循环参数、import 成员
//
// 模块命名空间（docs/plan-modules.md §3）：每个模块的顶层声明都发出
// `<模块键>__<名字>`。模块键由源根相对路径去掉 `.lain`、把 `/` 换成 `_` 得到，
// 所以 `Program` 必须在分析之前把整个 import 依赖图加载完，并且每个模块各存一份
// 文本 / AST / 别名表 / 成员表。

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "lain0/Ast.hpp"
#include "lain0/Diag.hpp"
#include "lain0/Lexer.hpp"
#include "lain0/Seed.hpp"
#include "lain0/Source.hpp"

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

// 当前模块的上下文：模块键、源根、别名表（`别名 -> 模块键`）。
// 类型键与符号名的限定全靠它 —— 裸名限定成 `<键>__<名字>`，
// 限定名 `M.Point` 先经别名表换成被导入模块的键再查。
//
// 根名（§3.2 / proposal-modules-v2 §四）：源码里带 `@root` 标注的顶层声明例外，
// 发出的名字保持平坦。宿主按名字找它们（Meta 驱动默认入口 `lain_std_lower` 就是
// 这样一个名字），所以 qualify 对根名原样返回 —— 顶层符号、数据符号、类型键于是
// 全部平坦，且因为全部经这一个函数，不存在漏改的旁路。
// rootNames 由 Program::addUnit 从 AST 的 `@root` 标注登记（不再来自清单的根名列）。
struct ModuleContext {
  std::string key{};        // 模块键，如 `std_lex` / `arith`
  std::string sourceRoot{}; // 源根目录（入口文件或清单所在目录）
  // import 的别名 -> 被导入模块的键。
  std::unordered_map<std::string, std::string> aliases{};
  // 源码里带 `@root` 标注的顶层声明的裸名字。
  std::unordered_set<std::string> rootNames{};

  std::string qualify(std::string_view name) const {
    if (key.empty()) return std::string(name);
    if (rootNames.count(std::string(name)) != 0) return std::string(name);
    return key + "__" + std::string(name);
  }

  bool isRootName(std::string_view name) const {
    return rootNames.count(std::string(name)) != 0;
  }
};

// 已导入模块的成员：`M.answer` 的值绑定是一个顶层 proc，
// `M.Point` 的类型要在被导入源码的类型表里查。
struct ModuleMember {
  enum class Kind { Value, Type, Func } kind = Kind::Value;
  std::string_view name{};
  // 值成员的类型是**访问器真正返回的类型**（§3.6）：数据块（表 / 字节串 /
  // 顶层聚合）的访问器返回 #addr，于是 `M.NAME + i`、`store(M.NAME + i, v)`
  // 走既有地址算术；其余静态 let 的访问器是值过程，返回它的声明类型。
  // 与同模块的 `bind(name, addr)` 对齐（那条路只覆盖数据块）。
  TypeId type = kInvalidType;
  std::string mangled{}; // 值绑定：`<mod>__<name>` 的过程名（owning，因为登记时
                         // 来自临时 SemaResult 的 key，不能留 string_view 悬垂）
  // 静态 let 的**声明类型**：`M.NAME[i]` 的元素步长要从它取 —— type 已经是
  // 访问器返回的 addr 时看不出元素宽度了。
  TypeId declType = kInvalidType;
};

class TypeStore {
public:
  // 预置全部标量名 —— 与 docs/04《A. 算子表》同一份名单：
  //   i32 u32 i64 u64 i8 u8 usize bool addr
  // i8/u8 只作 load/store 宽度，**不绑定算子**。
  void initBuiltins();

  TypeId builtin(std::string_view name) const;
  // 第一轮：只建 struct/enum 的**名字条目**（名字必须已经是模块限定名），
  // 布局留空。第二轮再由 fillStructLayout / fillEnumLayout 填。
  TypeId declareStruct(std::string_view name);
  TypeId declareEnum(std::string_view name);
  void fillStructLayout(TypeId id, AggregateLayout layout);
  void fillEnumLayout(TypeId id, AggregateLayout layout);
  // 布局是否已经算过（内建标量、数组、引用、切片、模块类型恒为 true）。
  bool hasLayout(TypeId id) const;
  // 数组类型可能被重复解析，登记是幂等的；resolve 是 const，所以这个也必须是。
  // 按名字查已登记的类型（enum 名 / struct 名）；没有返回 kInvalidType。
  TypeId lookup(std::string_view name) const;
  // 数组的元素类型（取不到返回 kInvalidType）。
  TypeId elementOf(TypeId id) const;
  TypeId addArrayOf(TypeId element, bool isRo) const;
  TypeId addModuleType(std::string_view moduleAlias);
  // 引用 / 切片类型。Ref 的 LAINIR 形态就是 #addr（附带 pointee 类型）；
  // Slice 的 LAINIR 形态是 2 字段聚合 (addr base, u64 len)。
  TypeId addRef(TypeId pointee, bool isMutable) const;
  TypeId addSlice(TypeId element) const;

  // 解析源码里的类型标注。ctx 是「当前模块」：裸名按 `<键>__<名字>` 查、
  // 限定名 `M.Point` 先过别名表；内建标量名不限定。
  // 失败返回 kInvalidType 并写诊断。
  TypeId resolve(const ast::TypeSyntax &ts, const ModuleContext &ctx,
                 DiagSink &diags) const;

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
    enum class K { Bits, Addr, Bool, Struct, Enum, Array, Module, Ref, Slice } k = K::Bits;
    bool signedInt = false;
    bool roArray = false;
    bool laidOut = true; // struct/enum 第一轮建条目时为 false
    TypeId element = kInvalidType;
    bool refMutable = false;  // Ref：是否可变引用（mut ref）
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
  // 顶层函数的返回值类型（供无标注 let 推导）。键是模块限定名。
  std::unordered_map<std::string, TypeId> functionReturns{};
  // 顶层静态绑定的类型。键是模块限定名。
  std::unordered_map<std::string, TypeId> topLevelTypes{};
  // 顶层表/串的元素个数（len(NAME) 要用）：顶层静态绑定的长度编译期可知。
  // 键是模块限定名。
  std::unordered_map<std::string, uint64_t> topLevelLength{};
  // 会发出 `data <键>__<名字>_storage` 的顶层绑定（symbol_addr 只认这些）。
  std::unordered_set<std::string> topLevelData{};
  // 顶层声明发出的符号名：源码里的名字（模块限定键）-> 发出的名字。
  // func 与顶层 let 是 `<键>__<名字>`；宿主 extern 保持平坦（宿主按 link_name
  // 找它，且同名 extern 跨模块重复是允许的），所以这里存的是裸名。
  std::unordered_map<std::string, std::string> topLevelSymbols{};
  // import 出来的模块成员：`<别名>.<成员>` -> ModuleMember
  std::unordered_map<std::string, ModuleMember> moduleMembers{};
  // 主源文件完整路径：import 解析被导入模块的相对路径要用到。
  std::string sourcePath{};
};

// 布局登记点：算布局时要按**声明所在模块**的上下文解析字段类型。
// 顶层类型：Program 的两轮分析要传它，Lowerer 不碰。
struct LayoutSite {
  const ast::Decl *decl = nullptr;
  const ModuleContext *ctx = nullptr;
  bool isEnum = false;
};
using LayoutSites = std::unordered_map<TypeId, LayoutSite>;

// 一个已加载的模块：文本、AST、别名表与成员表都归它。
// 存在 deque 里 —— 元素地址稳定，AST 里指向 text 的 string_view 不会失效。
struct ModuleUnit {
  std::string key{};             // 模块键
  std::string path{};            // 规范化后的文件路径
  std::string dir{};             // 所在目录（import 解析用）
  std::string text{};            // 拥有的源文本：token 与 AST 的 string_view 指向它
  uint32_t fileId = 0;           // SourceMgr 里的文件号（诊断位置用）；0 = 无位置
  std::vector<Token> toks{};     // 词法结果（--dump-tokens 用）
  ast::Module *ast = nullptr;
  std::unique_ptr<ast::AstArena> arena{};
  ModuleContext ctx{};
  SemaResult sema{};
  std::vector<std::string> importOrder{}; // 导入的模块键，按源码里的出现顺序
};

// 模块集合：一次加载整个 import 依赖图（同键只加载一次、只解析一次），
// 再分轮分析 —— 先给所有模块的 struct/enum 建名字条目，再算布局，
// 然后按依赖序登记顶层声明与成员表，最后走函数体。
class Program {
public:
  // 入口：单文件时源根 = 入口文件所在目录。src 只用来拿文件号与诊断位置，
  // 文本由 ModuleUnit 自己持一份（AST 的 string_view 指向它）。
  bool load(const std::string &entryPath, SourceMgr &src, DiagSink &diags);

  // 入口：模块清单（§3.3/§3.7）。每行 `<相对路径> [<根名> …]`，路径相对清单
  // 所在目录、省略 `.lain`，`#` 开头为注释。源根 = 清单所在目录。
  // 行尾的名字**不再读**（proposal-modules-v2 §四）：导出名从源码的 `@root` 标注来。
  bool loadUnit(const std::string &manifestPath, SourceMgr &src,
                DiagSink &diags);

  // 两轮分析。ts 由调用方给（全部模块共用一套类型表）。
  bool analyze(TypeStore &ts, DiagSink &diags);

  // §四 防撞（全部模块加载完之后跑）：`@root` 平坦名之间，以及平坦名与任何
  // `<键>__<名字>` 之间，都不许重名。编译期发现，不留给运行的 start FAILED 9102。
  bool checkRootExports(DiagSink &diags) const;

  const ModuleUnit *root() const { return root_; }
  const ModuleUnit *find(const std::string &key) const;
  const std::deque<ModuleUnit> &units() const { return units_; }
  const std::string &sourceRoot() const { return sourceRoot_; }

  // 降级顺序（§3.4）：先清单里的模块（按清单顺序），再依赖图里新出现的模块
  // （按发现顺序）。单文件入口时就是发现顺序，与旧行为一致。
  std::vector<const ModuleUnit *> lowerOrder() const;

private:
  using LayoutSites = std::unordered_map<TypeId, LayoutSite>;

  ModuleUnit *addUnit(const std::string &path, SourceMgr &src, DiagSink &diags);
  // 规范化后的文件路径 -> 模块键。跑出源根或键里含 `__` 都报 SemaBadImport。
  bool keyOf(const std::string &normPath, std::string &key, Span sp,
             DiagSink &diags) const;
  // 已加载的模块之后，把 import 依赖图按宽度优先补完。
  bool loadImports(SourceMgr &src, DiagSink &diags);

  std::deque<ModuleUnit> units_{};
  std::unordered_map<std::string, size_t> index_{}; // 模块键 -> units_ 下标
  std::vector<std::string> manifestOrder_{};        // 清单给出的模块键顺序
  ModuleUnit *root_ = nullptr;
  std::string sourceRoot_{};
};

} // namespace ln

#endif
