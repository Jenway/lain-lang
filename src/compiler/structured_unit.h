#ifndef LAIN_STRUCTURED_UNIT_H
#define LAIN_STRUCTURED_UNIT_H

#include "compiler/native_runtime.h"
#include "lainir/interpreter.h"

void native_register_structured_unit_ffi(
    void *ctx_ptr, void *env_ptr, native_foreign_registrar registrar,
    void *user_data);

/* Physical storage used to pass structured compiler inputs across the
 * artifact boundary.  Source/module policy remains in Lain. */
uint32_t structured_compiler_storage_new(uint32_t kind);
int structured_compiler_storage_reserve(uint32_t id, uint32_t capacity);
int structured_compiler_storage_set_i32(
    uint32_t id, uint32_t index, int32_t value);
int structured_compiler_storage_get_i32(
    uint32_t id, uint32_t index, int32_t *value_out);
int structured_compiler_storage_set_string(
    uint32_t id, uint32_t index, const char *value);
const char *structured_compiler_storage_get_string(
    uint32_t id, uint32_t index);
int structured_compiler_storage_destroy(uint32_t id);

#endif
