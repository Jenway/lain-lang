cmake_minimum_required(VERSION 4.0)

project(lain-seed
  VERSION 0.1.0
  DESCRIPTION "PL Experiment Lain Seed"
)

# seed 由两层组成，组织顺序固定：kernel 先编出 C 静态库 lain_seed，
# lain0 再链接它构建 C++ 前端。两个 add_subdirectory 的顺序不许调换。
#
# 产物路径固定（docs/development.md、scripts/ 与回归脚本都按这个断言）：
#   build/lib/lain_seed.lib    kernel 的静态库
#   build/lain-meta.exe        Meta 驱动
#   build/lain0/lain0.exe      lain0 的 CLI
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

option(LAIN_BUILD_DRIVER "Build the Meta driver tool" ON)
option(LAIN_BUILD_LAIN0 "Build the lain0 C++ frontend against lain_seed" ON)

add_subdirectory(kernel)

if(LAIN_BUILD_LAIN0)
  add_subdirectory(lain0)
endif()

if(LAIN_BUILD_DRIVER)
  add_executable(lain_meta_driver meta_driver.c)
  target_link_libraries(lain_meta_driver PRIVATE lain_seed)
  target_compile_features(lain_meta_driver PRIVATE c_std_11)
  set_target_properties(lain_meta_driver PROPERTIES
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    OUTPUT_NAME "lain-meta"
  )
  if(WIN32)
    # 驱动用 fopen 一类 POSIX 名，MSVC 的 ucrt 只给弃用警告。
    target_compile_definitions(lain_meta_driver PRIVATE _CRT_SECURE_NO_WARNINGS)
  endif()
endif()
