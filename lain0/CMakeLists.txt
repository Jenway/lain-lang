cmake_minimum_required(VERSION 4.0)

project(lain0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# seed 的根目录（C 实现的 IR / VM / 文本层）
set(LAIN0_SEED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../seed" CACHE PATH "seed root")
# 预构建的 seed 静态库。seed 是独立的 C 工程，见 docs/development.md：
#   cmake -S . -B build/cmake -G Ninja -DCMAKE_C_COMPILER=clang
#   cmake --build build/cmake        # -> build/lib/lain_seed.lib
set(LAIN0_SEED_LIB "${CMAKE_CURRENT_SOURCE_DIR}/../build/lib/lain_seed.lib"
    CACHE FILEPATH "prebuilt lain_seed static library")

add_library(lain0_core STATIC
  src/Source.cpp
  src/Diag.cpp
  src/Lexer.cpp
  src/Parser.cpp
  src/Sema.cpp
  src/Lower.cpp
  src/LowerExpr.cpp
  src/LowerStmt.cpp
  src/LowerDecl.cpp
  src/Driver.cpp
)

target_include_directories(lain0_core
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${LAIN0_SEED_DIR}/include"
)

# seed 自己的 _Static_assert 在 C++ 模式下会触发匿名枚举比较告警，
# 与我们无关，静音。
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  target_compile_options(lain0_core PUBLIC -Wno-deprecated-anon-enum-enum-conversion)
endif()

target_link_libraries(lain0_core PUBLIC "${LAIN0_SEED_LIB}")

# CLI
add_executable(lain0 tools/main.cpp)
target_link_libraries(lain0 PRIVATE lain0_core)

# 契约自洽 + seed 链接路径的两个哨兵。任何时候都应当能编译并跑通。
add_executable(headers_check tools/headers_check.cpp)
target_link_libraries(headers_check PRIVATE lain0_core)

add_executable(probe_seed tools/probe_seed.cpp)
target_link_libraries(probe_seed PRIVATE lain0_core)
