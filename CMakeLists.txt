cmake_minimum_required(VERSION 4.0)

project(lain-seed
  VERSION 0.1.0
  DESCRIPTION "PL Experiment Lain Seed"
  LANGUAGES C
)

add_library(lain_seed STATIC)
add_library(lain::seed ALIAS lain_seed)

target_sources(lain_seed
  PRIVATE
    seed/src/meta/apply.c
    seed/src/backend/cbackend.c
    seed/src/core/build.c
    seed/src/core/infer.c
    seed/src/core/opname.c
    seed/src/core/parse.c
    seed/src/core/print.c
    seed/src/core/verify.c
    seed/src/meta/ast_out.c
    seed/src/meta/host.c
    seed/src/meta/tree.c
    seed/src/vm/caps.c
    seed/src/vm/engine.c
    seed/src/vm/image.c
    seed/src/vm/memcap.c
    seed/src/vm/quota.c
    seed/src/vm/space.c
    seed/src/vm/tcb.c
  PUBLIC
    FILE_SET HEADERS
      BASE_DIRS seed/include
      FILES
        seed/include/lain/meta/apply.h
        seed/include/lain/backend/emit.h
        seed/include/lain/backend/target.h
        seed/include/lain/ir/build.h
        seed/include/lain/ir/core.h
        seed/include/lain/ir/infer.h
        seed/include/lain/ir/opname.h
        seed/include/lain/ir/parse.h
        seed/include/lain/ir/print.h
        seed/include/lain/ir/value.h
        seed/include/lain/ir/verify.h
        seed/include/lain/meta/ast_v1.h
        seed/include/lain/meta/expand.h
        seed/include/lain/meta/host.h
        seed/include/lain/meta/tree.h
        seed/include/lain/vm/caps.h
        seed/include/lain/vm/engine.h
        seed/include/lain/vm/image.h
        seed/include/lain/vm/memcap.h
        seed/include/lain/vm/quota.h
        seed/include/lain/vm/space.h
        seed/include/lain/vm/tcb.h
)

target_compile_features(lain_seed PUBLIC c_std_11)

set_target_properties(lain_seed PROPERTIES
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF
)

if(PROJECT_IS_TOP_LEVEL)
  set_target_properties(lain_seed PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
  )
endif()
