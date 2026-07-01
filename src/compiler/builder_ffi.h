// compiler/builder_ffi.h — FFI 注册入口

#ifndef BUILDER_FFI_H
#define BUILDER_FFI_H

#include "compiler/native_runtime.h"

void native_register_core_ffi(void *ctx_ptr, void *env_ptr,
                              native_foreign_registrar registrar,
                              void *user_data);

#endif
