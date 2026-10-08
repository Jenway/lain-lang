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
    seed/src/ir/build.c
    seed/src/ir/infer.c
    seed/src/text/opname.c
    seed/src/text/parse.c
    seed/src/text/print.c
    seed/src/ir/verify.c
    seed/src/meta/ast_out.c
    seed/src/meta/host.c
    seed/src/meta/tree.c
    seed/src/vm/caps.c
    seed/src/vm/engine.c
    seed/src/vm/image.c
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
        seed/include/lain/text/opname.h
        seed/include/lain/text/parse.h
        seed/include/lain/text/print.h
        seed/include/lain/ir/value.h
        seed/include/lain/ir/verify.h
        seed/include/lain/meta/ast_v1.h
        seed/include/lain/meta/expand.h
        seed/include/lain/meta/host.h
        seed/include/lain/meta/tree.h
        seed/include/lain/vm/caps.h
        seed/include/lain/vm/engine.h
              seed/include/lain/ir/codes.h
        seed/include/lain/vm/endpoint.h
        seed/include/lain/vm/trap.h
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

option(LAIN_BUILD_DRIVER "Build the Meta driver tool" ON)

if(LAIN_BUILD_DRIVER AND PROJECT_IS_TOP_LEVEL)
  add_executable(lain_meta_driver tools/meta_driver.c)
  target_link_libraries(lain_meta_driver PRIVATE lain_seed)
  target_compile_features(lain_meta_driver PRIVATE c_std_11)
  set_target_properties(lain_meta_driver PROPERTIES
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    OUTPUT_NAME "lain-meta"
  )
  if(WIN32)
    # 驱动用 fopen 一类 POSIX 名，MSVC 的 ucrt 只给弃用警告。
    target_compile_definitions(lain_meta_driver PRIVATE _CRT_SECURE_NO_WARNINGS)
  endif()
endif()
