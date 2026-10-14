#ifndef LAIN0_PARSER_HPP
#define LAIN0_PARSER_HPP

// Pratt 解析。输入是 lex() 出来的平铺 token 流，输出一棵 AstArena 上的 Module。
//
// 这里只决定「形状」，不决定含义： `p + i` 是不是合法要看操作数类型，那是 Sema 的事。
// 但优先级、结合性、语句终止这些纯语法问题是这里的职责。

#include <vector>

#include "lain0/Ast.hpp"
#include "lain0/Diag.hpp"
#include "lain0/Lexer.hpp"
#include "lain0/Source.hpp"

namespace ln {

// 出错时也可能返回一棵残缺的 Module（尽力恢复），调用方先看 diags.hasErrors()。
// 致命无法继续时返回 nullptr。
ast::Module *parseModule(const std::vector<Token> &toks, ast::AstArena &arena,
                         DiagSink &diags);

// 供单元彩排用：从一个 token 位置起解析一个表达式。
ast::Expr *parseExpressionAt(const std::vector<Token> &toks, size_t &pos,
                             ast::AstArena &arena, DiagSink &diags);

} // namespace ln

#endif
