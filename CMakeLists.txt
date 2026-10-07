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
    seed/src/apply/apply.c
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
        seed/include/lainapply/apply.h
        seed/include/lainbackend/emit.h
        seed/include/lainbackend/target.h
        seed/include/lainir/build.h
        seed/include/lainir/core.h
        seed/include/lainir/infer.h
        seed/include/lainir/opname.h
        seed/include/lainir/parse.h
        seed/include/lainir/print.h
        seed/include/lainir/value.h
        seed/include/lainir/verify.h
        seed/include/lainmeta/ast_v1.h
        seed/include/lainmeta/expand.h
        seed/include/lainmeta/host.h
        seed/include/lainmeta/tree.h
        seed/include/lainvm/caps.h
        seed/include/lainvm/engine.h
        seed/include/lainvm/image.h
        seed/include/lainvm/memcap.h
        seed/include/lainvm/quota.h
        seed/include/lainvm/space.h
        seed/include/lainvm/tcb.h
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
