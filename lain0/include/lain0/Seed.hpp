/* lain0 与 seed 的 ABI 边界。
 *
 * 实测：seed 的 23 个头文件里 `extern "C"` 守卫的数量是 0，
 * 所以 C++ 侧**必须**从这里进入 seed，不允许再直接 #include seed 的头文件，
 * 否则链接期会因 name mangling 全线失败。
 *
 * 这里同时是 lain0 唯一的、受信任的输出闸门：
 *   L1Builder 搭 IR → lainir_verify → lainir_print_to_string
 * 三者缺一不可。lainir_verify 不过，就永远到不了打印。
 */
#ifndef LAIN0_SEED_HPP
#define LAIN0_SEED_HPP

#include <cstddef>
#include <cstdint>

extern "C" {

#include "lain/ir/build.h"
#include "lain/ir/codes.h"
#include "lain/ir/core.h"
#include "lain/ir/infer.h"
#include "lain/ir/verify.h"
#include "lain/text/parse.h"
#include "lain/text/print.h"

}

#endif /* LAIN0_SEED_HPP */
