// 契约自洽检查：只 include 全部头文件，不实例化任何东西。
// 编译通过 == lain0 的对外契约内部一致（含 Seed.hpp 的 extern "C" 边界）。
#include "lain0/Ast.hpp"
#include "lain0/Diag.hpp"
#include "lain0/Driver.hpp"
#include "lain0/Lexer.hpp"
#include "lain0/Lower.hpp"
#include "lain0/Parser.hpp"
#include "lain0/Seed.hpp"
#include "lain0/Sema.hpp"
#include "lain0/Source.hpp"

int main() { return 0; }
