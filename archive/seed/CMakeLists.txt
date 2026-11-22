cmake_minimum_required(VERSION 4.0)

project(lain-seed
  VERSION 0.1.0
  DESCRIPTION "PL Experiment Lain Seed"
)

# seed 由 kernel 层组成：kernel 编出 C 静态库 lain_seed，各工具再链接它构建。
#
# 产物路径固定（README 的快速开始与 .github/workflows/lain-bootstrap.yml 都按这个断言）：
#   build/lib/lain_seed.lib      kernel 的静态库
#   build/lain-meta-v2.exe       Meta 驱动
#   build/lainir-verify.exe      校验工具
#   build/lainir-run.exe         执行工具
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

option(LAIN_BUILD_DRIVER "Build the Meta driver tool" ON)

add_subdirectory(kernel)

if(LAIN_BUILD_DRIVER)
  # Meta 驱动：结构化句柄 + 一次符号驻留 + 沙箱 apply。
  add_executable(lain_meta_v2_driver meta_v2_driver.c)
  target_link_libraries(lain_meta_v2_driver PRIVATE lain_seed)
  target_compile_features(lain_meta_v2_driver PRIVATE c_std_11)
  set_target_properties(lain_meta_v2_driver PROPERTIES
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    OUTPUT_NAME "lain-meta-v2"
  )
  if(WIN32)
    target_compile_definitions(lain_meta_v2_driver PRIVATE _CRT_SECURE_NO_WARNINGS)
  endif()

  # 校验工具：把若干份产物单元当成一个模块解析再验证。Meta 的产物是单元，
  # 跨模块引用要拼齐才能解析，所以校验必须在一个 builder 里做。
  add_executable(lainir_verify lainir_verify.c)
  target_link_libraries(lainir_verify PRIVATE lain_seed)
  target_compile_features(lainir_verify PRIVATE c_std_11)
  set_target_properties(lainir_verify PROPERTIES
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    OUTPUT_NAME "lainir-verify"
  )
  if(WIN32)
    target_compile_definitions(lainir_verify PRIVATE _CRT_SECURE_NO_WARNINGS)
  endif()

  # 执行工具：把产物真跑一遍，给降级结果做真值断言与差分比对。
  add_executable(lainir_run lainir_run.c)
  target_link_libraries(lainir_run PRIVATE lain_seed)
  target_compile_features(lainir_run PRIVATE c_std_11)
  set_target_properties(lainir_run PROPERTIES
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    OUTPUT_NAME "lainir-run"
  )
  if(WIN32)
    target_compile_definitions(lainir_run PRIVATE _CRT_SECURE_NO_WARNINGS)
  endif()

  # C99 后端：把已验证的 LAINIR 发射为标准 C99 源码。
  add_executable(lainir_c lainir_c.c)
  target_link_libraries(lainir_c PRIVATE lain_seed)
  target_compile_features(lainir_c PRIVATE c_std_11)
  set_target_properties(lainir_c PROPERTIES
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    OUTPUT_NAME "lainir-c"
  )
  if(WIN32)
    target_compile_definitions(lainir_c PRIVATE _CRT_SECURE_NO_WARNINGS)
  endif()
endif()
