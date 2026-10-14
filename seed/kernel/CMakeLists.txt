cmake_minimum_required(VERSION 4.0)

project(lain-kernel
  VERSION 0.1.0
  DESCRIPTION "PL Experiment Lain Kernel"
  LANGUAGES C
)

add_library(lain_seed STATIC)
add_library(lain::seed ALIAS lain_seed)

target_sources(lain_seed
  PRIVATE
    src/meta/apply.c
    src/backend/cbackend.c
    src/ir/build.c
    src/ir/infer.c
    src/text/opname.c
    src/text/parse.c
    src/text/print.c
    src/ir/verify.c
    src/meta/ast_out.c
    src/meta/host.c
    src/meta/tree.c
    src/vm/caps.c
    src/vm/engine.c
    src/vm/image.c
    src/vm/quota.c
    src/vm/space.c
    src/vm/tcb.c
  PUBLIC
    FILE_SET HEADERS
      BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include
      FILES
        include/lain/meta/apply.h
        include/lain/backend/emit.h
        include/lain/backend/target.h
        include/lain/ir/build.h
        include/lain/ir/core.h
        include/lain/ir/codes.h
        include/lain/ir/infer.h
        include/lain/ir/value.h
        include/lain/ir/verify.h
        include/lain/meta/apply.h
        include/lain/meta/ast_v1.h
        include/lain/meta/expand.h
        include/lain/meta/host.h
        include/lain/meta/tree.h
        include/lain/text/opname.h
        include/lain/text/parse.h
        include/lain/text/print.h
        include/lain/vm/caps.h
        include/lain/vm/engine.h
        include/lain/vm/endpoint.h
        include/lain/vm/quota.h
        include/lain/vm/space.h
        include/lain/vm/tcb.h
        include/lain/vm/trap.h
)

target_compile_features(lain_seed PUBLIC c_std_11)

set_target_properties(lain_seed PROPERTIES
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF
)

# 单独配置 kernel（cmake -S seed/kernel）与被 seed 聚合时都要落到 build/lib，
# 所以不按 PROJECT_IS_TOP_LEVEL 分支。
set_target_properties(lain_seed PROPERTIES
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)
