/*
 * Native host for the LAIN-IR-written compiler.
 *
 * This file owns process I/O and allocation only. Seed parsing, verification,
 * and #eval are exposed through the runtime; C emission remains in
 * seed/lainir/compiler.l1 and in the generated C compiled beside this host.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lainir/eval_source.h"
#include "lainir/parse.h"
#include "lainir/verify.h"

static uint8_t *source_bytes;
static int64_t source_size;
static const char *artifact_path;
static FILE *artifact_file;
static uint64_t *eval_values;
static size_t eval_value_count;
static size_t eval_value_index;
static LainirModuleHandle *prepared_module;

static const L1Subroutine *find_prepared_procedure(int64_t id) {
  const L1Subroutine *procedure;
  if (!prepared_module || id == 0)
    return NULL;
  for (procedure = lainir_module_handle_first(prepared_module);
       procedure; procedure = lainir_procedure_next(procedure)) {
    if ((int64_t)(uintptr_t)procedure == id)
      return procedure;
  }
  return NULL;
}

static const L1Block *find_block_tree(const L1Block *block, int64_t id) {
  for (; block; block = lainir_block_next(block)) {
    const L1Instruction *instruction;
    if ((int64_t)(uintptr_t)block == id) return block;
    for (instruction = lainir_block_first_instruction(block);
         instruction; instruction = lainir_instruction_next(instruction)) {
      const L1Block *nested = lainir_instruction_then_block(instruction);
      const L1Block *found = find_block_tree(nested, id);
      if (found) return found;
      nested = lainir_instruction_else_block(instruction);
      found = find_block_tree(nested, id);
      if (found) return found;
      nested = lainir_instruction_loop_block(instruction);
      found = find_block_tree(nested, id);
      if (found) return found;
    }
  }
  return NULL;
}

static const L1Block *find_prepared_block(int64_t id) {
  const L1Subroutine *procedure;
  if (!prepared_module || id == 0) return NULL;
  for (procedure = lainir_module_handle_first(prepared_module);
       procedure; procedure = lainir_procedure_next(procedure)) {
    const L1Block *found = find_block_tree(
        lainir_procedure_first_block(procedure), id);
    if (found) return found;
  }
  return NULL;
}

static const L1Instruction *find_instruction_tree(
    const L1Block *block, int64_t id) {
  for (; block; block = lainir_block_next(block)) {
    const L1Instruction *instruction;
    for (instruction = lainir_block_first_instruction(block);
         instruction; instruction = lainir_instruction_next(instruction)) {
      const L1Instruction *found;
      if ((int64_t)(uintptr_t)instruction == id) return instruction;
      found = find_instruction_tree(
          lainir_instruction_then_block(instruction), id);
      if (found) return found;
      found = find_instruction_tree(
          lainir_instruction_else_block(instruction), id);
      if (found) return found;
      found = find_instruction_tree(
          lainir_instruction_loop_block(instruction), id);
      if (found) return found;
    }
  }
  return NULL;
}

static const L1Instruction *find_prepared_instruction(int64_t id) {
  const L1Subroutine *procedure;
  if (!prepared_module || id == 0) return NULL;
  for (procedure = lainir_module_handle_first(prepared_module);
       procedure; procedure = lainir_procedure_next(procedure)) {
    const L1Instruction *found = find_instruction_tree(
        lainir_procedure_first_block(procedure), id);
    if (found) return found;
  }
  return NULL;
}

static const L1Expr *find_expression_tree(const L1Expr *expression,
                                          int64_t id, unsigned depth);

static const L1Expr *find_expression_in_block(const L1Block *block,
                                               int64_t id, unsigned depth) {
  for (; block; block = lainir_block_next(block)) {
    const L1Instruction *instruction;
    for (instruction = lainir_block_first_instruction(block); instruction;
         instruction = lainir_instruction_next(instruction)) {
      const L1Expr *found;
      if ((found = find_expression_tree(lainir_instruction_value(instruction), id, depth + 1))) return found;
      if ((found = find_expression_tree(lainir_instruction_destination(instruction), id, depth + 1))) return found;
      if ((found = find_expression_tree(lainir_instruction_condition(instruction), id, depth + 1))) return found;
      if ((found = find_expression_in_block(lainir_instruction_then_block(instruction), id, depth + 1))) return found;
      if ((found = find_expression_in_block(lainir_instruction_else_block(instruction), id, depth + 1))) return found;
      if ((found = find_expression_in_block(lainir_instruction_loop_block(instruction), id, depth + 1))) return found;
    }
  }
  return NULL;
}

static const L1Expr *find_expression_tree(const L1Expr *expression,
                                          int64_t id, unsigned depth) {
  uint32_t index, count;
  const L1Expr *found;
  if (!expression || depth > 1024) return NULL;
  if ((int64_t)(uintptr_t)expression == id) return expression;
  if ((found = find_expression_tree(lainir_expr_left(expression), id, depth + 1))) return found;
  if ((found = find_expression_tree(lainir_expr_right(expression), id, depth + 1))) return found;
  if ((found = find_expression_tree(lainir_expr_operand(expression), id, depth + 1))) return found;
  if ((found = find_expression_in_block(lainir_expr_block(expression), id, depth + 1))) return found;
  count = lainir_expr_argument_count(expression);
  for (index = 0; index < count; ++index) {
    if ((found = find_expression_tree(lainir_expr_argument_at(expression, index), id, depth + 1))) return found;
  }
  return NULL;
}

static const L1Expr *find_prepared_expression(int64_t id) {
  const L1Subroutine *procedure;
  if (!prepared_module || id == 0) return NULL;
  for (procedure = lainir_module_handle_first(prepared_module);
       procedure; procedure = lainir_procedure_next(procedure)) {
    const L1Expr *found = find_expression_in_block(
        lainir_procedure_first_block(procedure), id, 0);
    if (found) return found;
  }
  return NULL;
}

int32_t lainir_compile(void);
int32_t lainir_compile_module(void);

int32_t bootstrap_dot_eval_source(uint8_t *source, int64_t length) {
  L1Diagnostic diagnostic = {0};
  const char *error = NULL;
  if (!source || length < 0 || !prepared_module ||
      source != source_bytes || length != source_size)
    return 0;
  free(eval_values);
  eval_values = NULL;
  eval_value_count = 0;
  eval_value_index = 0;
  if (lainir_module_handle_eval_values(
          prepared_module, NULL, &eval_values, &eval_value_count,
          &diagnostic, &error) != LAINIR_RUN_OK) {
    fprintf(stderr, "lainir-compiler: #eval failed [%d] line %d:%d: %s%s%s\n",
            lainir_diagnostic_code(&diagnostic),
            lainir_diagnostic_line(&diagnostic),
            lainir_diagnostic_column(&diagnostic),
            error ? error : "",
            error && *lainir_diagnostic_message(&diagnostic) ? "; " : "",
            lainir_diagnostic_message(&diagnostic));
    return 0;
  }
  return 1;
}

int64_t bootstrap_dot_eval_dash_next(void) {
  if (eval_value_index >= eval_value_count) return 0;
  return (int64_t)eval_values[eval_value_index++];
}

int64_t bootstrap_dot_ir_dash_module_dash_id(uint8_t *source, int64_t length) {
  (void)source;
  (void)length;
  return (int64_t)(uintptr_t)prepared_module;
}

int64_t bootstrap_dot_ir_dash_module_dash_id_dash_release(int64_t id) {
  return id == (int64_t)(uintptr_t)prepared_module ? 0 : 1;
}

int64_t bootstrap_dot_ir_dash_module_dash_first(int64_t id) {
  if (id != (int64_t)(uintptr_t)prepared_module) return 0;
  return (int64_t)(uintptr_t)lainir_module_handle_first(prepared_module);
}

int64_t bootstrap_dot_ir_dash_procedure_dash_find(int64_t module_id,
                                                   uint8_t *name) {
  const L1Subroutine *procedure;
  if (module_id != (int64_t)(uintptr_t)prepared_module || !name) return 0;
  for (procedure = lainir_module_handle_first(prepared_module);
       procedure; procedure = lainir_procedure_next(procedure)) {
    if (lainir_procedure_name(procedure) &&
        strcmp(lainir_procedure_name(procedure), (const char *)name) == 0)
      return (int64_t)(uintptr_t)procedure;
  }
  return 0;
}

int64_t bootstrap_dot_ir_dash_procedure_dash_next(int64_t id) {
  const L1Subroutine *procedure = find_prepared_procedure(id);
  return procedure ? (int64_t)(uintptr_t)lainir_procedure_next(procedure) : 0;
}

uint8_t *bootstrap_dot_ir_dash_procedure_dash_name(int64_t id) {
  const L1Subroutine *procedure = find_prepared_procedure(id);
  return procedure ? (uint8_t *)lainir_procedure_name(procedure) : NULL;
}

int64_t bootstrap_dot_ir_dash_procedure_dash_name_dash_length(int64_t id) {
  const L1Subroutine *procedure = find_prepared_procedure(id);
  return procedure ? (int64_t)lainir_procedure_name_length(procedure) : 0;
}

uint8_t *bootstrap_dot_ir_dash_procedure_dash_link_dash_name(int64_t id) {
  const L1Subroutine *procedure = find_prepared_procedure(id);
  return procedure ? (uint8_t *)lainir_procedure_link_name(procedure) : NULL;
}

int64_t bootstrap_dot_ir_dash_procedure_dash_return_dash_width(int64_t id) {
  const L1Subroutine *procedure = find_prepared_procedure(id);
  return procedure ? (int64_t)lainir_type_width(
                         lainir_procedure_return_type(procedure))
                   : -1;
}

int64_t bootstrap_dot_ir_dash_procedure_dash_return_dash_kind(int64_t id) {
  const L1Subroutine *procedure = find_prepared_procedure(id);
  const L1Type *type = procedure ? lainir_procedure_return_type(procedure) : NULL;
  return type ? (int64_t)lainir_type_kind(type) : -1;
}

int64_t bootstrap_dot_ir_dash_procedure_dash_parameter_dash_count(int64_t id) {
  const L1Subroutine *procedure = find_prepared_procedure(id);
  return procedure ? (int64_t)lainir_procedure_parameter_count(procedure) : -1;
}

int64_t bootstrap_dot_ir_dash_procedure_dash_parameter_dash_kind(
    int64_t id, int64_t index) {
  const L1Subroutine *procedure = find_prepared_procedure(id);
  const L1Type *type = procedure && index >= 0 && (uint64_t)index <= UINT32_MAX
      ? lainir_procedure_parameter_type(procedure, (uint32_t)index) : NULL;
  return type ? (int64_t)lainir_type_kind(type) : -1;
}

int64_t bootstrap_dot_ir_dash_procedure_dash_external(int64_t id) {
  const L1Subroutine *procedure = find_prepared_procedure(id);
  return procedure ? (int64_t)lainir_procedure_is_external(procedure) : -1;
}

int64_t bootstrap_dot_ir_dash_procedure_dash_data(int64_t id) {
  const L1Subroutine *item = find_prepared_procedure(id);
  return item ? (int64_t)lainir_procedure_is_data(item) : -1;
}

int64_t bootstrap_dot_ir_dash_data_dash_size(int64_t id) {
  const L1Subroutine *item = find_prepared_procedure(id);
  return item && lainir_procedure_is_data(item) ? (int64_t)lainir_data_size(item) : -1;
}

int64_t bootstrap_dot_ir_dash_data_dash_alignment(int64_t id) {
  const L1Subroutine *item = find_prepared_procedure(id);
  return item && lainir_procedure_is_data(item) ? (int64_t)lainir_data_alignment(item) : -1;
}

int8_t bootstrap_dot_ir_dash_data_dash_byte(int64_t id, int64_t index) {
  const L1Subroutine *item = find_prepared_procedure(id);
  const uint8_t *bytes;
  if (!item || !lainir_procedure_is_data(item) || index < 0 ||
      (uint64_t)index >= lainir_data_size(item) || !(bytes = lainir_data_bytes(item)))
    return 0;
  return (int8_t)bytes[index];
}

int64_t bootstrap_dot_ir_dash_procedure_dash_first_dash_block(int64_t id) {
  const L1Subroutine *procedure = find_prepared_procedure(id);
  return procedure ? (int64_t)(uintptr_t)lainir_procedure_first_block(procedure) : 0;
}

int64_t bootstrap_dot_ir_dash_block_dash_first_dash_instruction(int64_t id) {
  const L1Block *block = find_prepared_block(id);
  return block ? (int64_t)(uintptr_t)lainir_block_first_instruction(block) : 0;
}

int64_t bootstrap_dot_ir_dash_block_dash_next(int64_t id) {
  const L1Block *block = find_prepared_block(id);
  return block ? (int64_t)(uintptr_t)lainir_block_next(block) : 0;
}

int64_t bootstrap_dot_ir_dash_instruction_dash_next(int64_t id) {
  const L1Instruction *instruction = find_prepared_instruction(id);
  return instruction ? (int64_t)(uintptr_t)lainir_instruction_next(instruction) : 0;
}

int64_t bootstrap_dot_ir_dash_instruction_dash_kind(int64_t id) {
  const L1Instruction *instruction = find_prepared_instruction(id);
  return instruction ? (int64_t)lainir_instruction_kind(instruction) : -1;
}

int64_t bootstrap_dot_ir_dash_instruction_dash_value(int64_t id) {
  const L1Instruction *instruction = find_prepared_instruction(id);
  return instruction ? (int64_t)(uintptr_t)lainir_instruction_value(instruction) : 0;
}

int64_t bootstrap_dot_ir_dash_instruction_dash_destination(int64_t id) {
  const L1Instruction *instruction = find_prepared_instruction(id);
  return instruction ? (int64_t)(uintptr_t)lainir_instruction_destination(instruction) : 0;
}

uint8_t *bootstrap_dot_ir_dash_instruction_dash_name(int64_t id) {
  const L1Instruction *instruction = find_prepared_instruction(id);
  return instruction ? (uint8_t *)lainir_instruction_name(instruction) : NULL;
}

uint8_t *bootstrap_dot_ir_dash_instruction_dash_label(int64_t id) {
  const L1Instruction *instruction = find_prepared_instruction(id);
  return instruction ? (uint8_t *)lainir_instruction_label(instruction) : NULL;
}

int64_t bootstrap_dot_ir_dash_instruction_dash_type_dash_width(int64_t id) {
  const L1Instruction *instruction = find_prepared_instruction(id);
  const L1Type *type = instruction ? lainir_instruction_type(instruction) : NULL;
  return type ? (int64_t)lainir_type_width(type) : 0;
}

int64_t bootstrap_dot_ir_dash_instruction_dash_type_dash_kind(int64_t id) {
  const L1Instruction *instruction = find_prepared_instruction(id);
  const L1Type *type = instruction ? lainir_instruction_type(instruction) : NULL;
  return type ? (int64_t)lainir_type_kind(type) : -1;
}

int64_t bootstrap_dot_ir_dash_instruction_dash_condition(int64_t id) {
  const L1Instruction *instruction = find_prepared_instruction(id);
  return instruction ? (int64_t)(uintptr_t)lainir_instruction_condition(instruction) : 0;
}

int64_t bootstrap_dot_ir_dash_instruction_dash_then_dash_block(int64_t id) {
  const L1Instruction *instruction = find_prepared_instruction(id);
  return instruction ? (int64_t)(uintptr_t)lainir_instruction_then_block(instruction) : 0;
}

int64_t bootstrap_dot_ir_dash_instruction_dash_else_dash_block(int64_t id) {
  const L1Instruction *instruction = find_prepared_instruction(id);
  return instruction ? (int64_t)(uintptr_t)lainir_instruction_else_block(instruction) : 0;
}

int64_t bootstrap_dot_ir_dash_instruction_dash_loop_dash_block(int64_t id) {
  const L1Instruction *instruction = find_prepared_instruction(id);
  return instruction ? (int64_t)(uintptr_t)lainir_instruction_loop_block(instruction) : 0;
}

int64_t bootstrap_dot_ir_dash_expression_dash_kind(int64_t id) {
  const L1Expr *expression = find_prepared_expression(id);
  return expression ? (int64_t)lainir_expr_kind(expression) : -1;
}

int64_t bootstrap_dot_ir_dash_expression_dash_const(int64_t id) {
  const L1Expr *expression = find_prepared_expression(id);
  return expression ? lainir_expr_const_value(expression) : 0;
}

int64_t bootstrap_dot_ir_dash_expression_dash_left(int64_t id) {
  const L1Expr *expression = find_prepared_expression(id);
  return expression ? (int64_t)(uintptr_t)lainir_expr_left(expression) : 0;
}

int64_t bootstrap_dot_ir_dash_expression_dash_right(int64_t id) {
  const L1Expr *expression = find_prepared_expression(id);
  return expression ? (int64_t)(uintptr_t)lainir_expr_right(expression) : 0;
}

int64_t bootstrap_dot_ir_dash_expression_dash_operand(int64_t id) {
  const L1Expr *expression = find_prepared_expression(id);
  return expression ? (int64_t)(uintptr_t)lainir_expr_operand(expression) : 0;
}

int64_t bootstrap_dot_ir_dash_expression_dash_block(int64_t id) {
  const L1Expr *expression = find_prepared_expression(id);
  return expression ? (int64_t)(uintptr_t)lainir_expr_block(expression) : 0;
}

int64_t bootstrap_dot_ir_dash_expression_dash_type_dash_width(int64_t id) {
  const L1Expr *expression = find_prepared_expression(id);
  const L1Type *type = expression ? lainir_expr_type(expression) : NULL;
  return type ? (int64_t)lainir_type_width(type) : 0;
}

int64_t bootstrap_dot_ir_dash_expression_dash_type_dash_kind(int64_t id) {
  const L1Expr *expression = find_prepared_expression(id);
  const L1Type *type = expression ? lainir_expr_type(expression) : NULL;
  return type ? (int64_t)lainir_type_kind(type) : -1;
}

int64_t bootstrap_dot_ir_dash_expression_dash_scale(int64_t id) {
  const L1Expr *expression = find_prepared_expression(id);
  return expression ? (int64_t)lainir_expr_scale(expression) : 0;
}

int64_t bootstrap_dot_ir_dash_expression_dash_offset(int64_t id) {
  const L1Expr *expression = find_prepared_expression(id);
  return expression ? (int64_t)lainir_expr_offset(expression) : 0;
}

int64_t bootstrap_dot_ir_dash_expression_dash_byte_dash_size(int64_t id) {
  const L1Expr *expression = find_prepared_expression(id);
  return expression ? (int64_t)lainir_expr_byte_size(expression) : 0;
}

uint8_t *bootstrap_dot_ir_dash_expression_dash_string(int64_t id) {
  const L1Expr *expression = find_prepared_expression(id);
  return expression ? (uint8_t *)lainir_expr_string(expression) : NULL;
}

uint8_t *bootstrap_dot_ir_dash_procedure_dash_parameter_dash_name(
    int64_t id, int64_t index) {
  const L1Subroutine *procedure = find_prepared_procedure(id);
  return procedure && index >= 0 && (uint64_t)index <= UINT32_MAX
             ? (uint8_t *)lainir_procedure_parameter_name(procedure, (uint32_t)index)
             : NULL;
}

int64_t bootstrap_dot_ir_dash_procedure_dash_parameter_dash_width(
    int64_t id, int64_t index) {
  const L1Subroutine *procedure = find_prepared_procedure(id);
  const L1Type *type = procedure && index >= 0 && (uint64_t)index <= UINT32_MAX
      ? lainir_procedure_parameter_type(procedure, (uint32_t)index) : NULL;
  return type ? (int64_t)lainir_type_width(type) : -1;
}

int64_t bootstrap_dot_ir_dash_expression_dash_arg_dash_index(int64_t id) {
  const L1Expr *expression = find_prepared_expression(id);
  return expression ? (int64_t)lainir_expr_arg_index(expression) : -1;
}

uint8_t *bootstrap_dot_ir_dash_expression_dash_callee_dash_name(int64_t id) {
  const L1Expr *expression = find_prepared_expression(id);
  return expression ? (uint8_t *)lainir_expr_callee_name(expression) : NULL;
}

uint8_t *bootstrap_dot_ir_dash_expression_dash_name(int64_t id) {
  const L1Expr *expression = find_prepared_expression(id);
  return expression ? (uint8_t *)lainir_expr_name(expression) : NULL;
}

int64_t bootstrap_dot_ir_dash_expression_dash_argument_dash_count(int64_t id) {
  const L1Expr *expression = find_prepared_expression(id);
  return expression ? (int64_t)lainir_expr_argument_count(expression) : -1;
}

int64_t bootstrap_dot_ir_dash_expression_dash_argument_dash_at(
    int64_t id, int64_t index) {
  const L1Expr *expression = find_prepared_expression(id);
  const L1Expr *argument = expression && index >= 0 && (uint64_t)index <= UINT32_MAX
      ? lainir_expr_argument_at(expression, (uint32_t)index) : NULL;
  return argument ? (int64_t)(uintptr_t)argument : 0;
}

int32_t bootstrap_dot_validate_dash_source(uint8_t *source, int64_t length) {
  if (!source || length < 0 || !prepared_module)
    return 0;
  return source == source_bytes && length == source_size ? 1 : 0;
}

int64_t bootstrap_dot_source_dash_count(void) { return 1; }

uint8_t *bootstrap_dot_source_dash_data(int64_t index) {
  return index == 0 ? source_bytes : NULL;
}

int64_t bootstrap_dot_source_dash_length(int64_t index) {
  return index == 0 ? source_size : 0;
}

uint8_t *bootstrap_dot_allocate_dash_pages(int64_t size) {
  return calloc(size > 0 ? (size_t)size : 1, 1);
}

void bootstrap_dot_release_dash_pages(uint8_t *memory) { free(memory); }

void bootstrap_dot_artifact_dash_begin(void) {
  if (artifact_file)
    return;
  artifact_file = fopen(artifact_path, "wb");
}

void bootstrap_dot_artifact_dash_write_dash_byte(int8_t byte) {
  if (artifact_file)
    fputc((unsigned char)byte, artifact_file);
}

void bootstrap_dot_artifact_dash_write_dash_literal(uint8_t *text) {
  if (artifact_file && text)
    fputs((const char *)text, artifact_file);
}

void bootstrap_dot_artifact_dash_write_dash_identifier(uint8_t *name) {
  if (!artifact_file || !name)
    return;
  for (; *name; ++name) {
    const char *replacement = NULL;
    if (*name == '.') replacement = "_dot_";
    else if (*name == '-') replacement = "_dash_";
    else if (*name == '!') replacement = "_bang_";
    if (replacement) fputs(replacement, artifact_file);
    else fputc(*name, artifact_file);
  }
}

void bootstrap_dot_artifact_dash_finish(void) {
  if (!artifact_file)
    return;
  if (fclose(artifact_file) != 0)
    remove(artifact_path);
  artifact_file = NULL;
}

void bootstrap_dot_write_dash_diagnostic(uint8_t *text, int64_t length) {
  if (text && length > 0)
    fwrite(text, 1, (size_t)length, stderr);
  fputc('\n', stderr);
}

static uint8_t *read_source(const char *path, int64_t *length_out) {
  FILE *file = fopen(path, "rb");
  long length;
  uint8_t *bytes;
  if (!file)
    return NULL;
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return NULL;
  }
  length = ftell(file);
  if (length < 0 || fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return NULL;
  }
  /* The seed parser accepts a C string.  Keep the terminator outside the
   * reported source length so byte-oriented L1 code still sees the exact
   * input while the C-side preparse remains bounded. */
  bytes = malloc((size_t)length + 1);
  if (!bytes || (length && fread(bytes, 1, (size_t)length, file) != (size_t)length)) {
    free(bytes);
    fclose(file);
    return NULL;
  }
  bytes[length] = 0;
  fclose(file);
  *length_out = (int64_t)length;
  return bytes;
}

int main(int argc, char **argv) {
  int32_t status;
  int module_mode = argc == 4 && strcmp(argv[1], "--module") == 0;
  const char *output;
  const char *input;
  if ((!module_mode && argc != 3) || (module_mode && argc != 4)) {
    fprintf(stderr, "usage: lainir-compiler [--module] <output.c> <input.l1>\n");
    return 2;
  }
  output = module_mode ? argv[2] : argv[1];
  input = module_mode ? argv[3] : argv[2];
  artifact_path = output;
  source_bytes = read_source(input, &source_size);
  if (!source_bytes) {
    fprintf(stderr, "lainir-c: cannot read %s\n", input);
    return 2;
  }
  {
    L1Diagnostic diagnostic = {0};
    if (lainir_module_parse_handle((const char *)source_bytes, &prepared_module,
                                   &diagnostic) != LAINIR_RUN_OK ||
        lainir_module_handle_verify(prepared_module, &diagnostic) != LAINIR_RUN_OK) {
      fprintf(stderr, "lainir-compiler: input rejected [%d] line %d: %s\n",
              diagnostic.code, diagnostic.line, diagnostic.message);
      lainir_module_handle_destroy(&prepared_module);
      free(source_bytes);
      return 2;
    }
  }
  status = module_mode ? lainir_compile_module() : lainir_compile();
  if (artifact_file) {
    fclose(artifact_file);
    artifact_file = NULL;
  }
  if (status != 0)
    remove(artifact_path);
  free(source_bytes);
  source_bytes = NULL;
  lainir_module_handle_destroy(&prepared_module);
  free(eval_values);
  eval_values = NULL;
  return status;
}
