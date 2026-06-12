/**
 * compiler/helpers.c — Native compiler C runtime
 *
 * Provides:
 *   - L1 IR type definitions (shared with bootstrap_l1.c layout)
 *   - Token grouping (complements compiler/lexer.lain's flat tokenizer)
 *   - L1 IR builder FFI functions (for Chibi-Scheme meta passes)
 *   - C code emission (walks L1 IR, writes C source)
 *   - Top-level API functions callable from Lain via @foreign
 *
 * Linked with:
 *   - Bootstrap-compiled Lain → C code (main.lain + imports)
 *   - Chibi-Scheme (libchibi)
 */

#include <alloca.h>
#include <chibi/eval.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// ============================================================================
// 1. L1 Core IR Type Definitions
// ============================================================================

typedef enum { TY_BITS, TY_ADDR, TY_VOID, TY_PRODUCT } L1TypeKind;

typedef struct L1ProductField {
  char *name;
  struct L1Type *ty;
  uint32_t offset;
} L1ProductField;

typedef struct L1Type {
  L1TypeKind kind;
  uint32_t width;
  uint32_t field_count;
  L1ProductField *fields;
  char *struct_name;
} L1Type;

typedef struct L1StructInfo {
  char *name;
  L1Type *ty;
  struct L1StructInfo *next;
} L1StructInfo;

static L1StructInfo *g_struct_registry = NULL;

typedef enum {
  INST_SET,
  INST_STORE,
  INST_LOOP,
  INST_BREAK,
  INST_RETURN,
  INST_CALL,
  INST_PHI_ASSIGN
} L1InstKind;

typedef enum {
  EXPR_VAR,
  EXPR_CONST,
  EXPR_ARG,
  EXPR_LOAD,
  EXPR_LEA,
  EXPR_ADD,
  EXPR_SUB,
  EXPR_CALL,
  EXPR_STRING,
  EXPR_PRIMITIVE,
  EXPR_ALLOCA,
  EXPR_FIELD,
  EXPR_CALL_INDIRECT
} L1ExprKind;

typedef struct L1Expr {
  L1ExprKind kind;
  union {
    struct {
      char *name;
      L1Type *ty;
    } var;
    int64_t const_val;
    uint32_t arg_idx;
    struct {
      struct L1Expr *addr;
      L1Type *ty;
    } load;
    struct {
      struct L1Expr *base;
      struct L1Expr *idx;
      uint32_t scale;
      uint32_t offset;
    } lea;
    struct {
      struct L1Expr *left;
      struct L1Expr *right;
    } bin;
    struct {
      char *fn_name;
      struct L1Expr **args;
      uint32_t arg_count;
      L1Type *ret_ty;
    } call;
    struct {
      char *content;
      L1Type *ty;
    } str_val;
    struct {
      char *opcode;
      struct L1Expr **operands;
      uint32_t operand_count;
      L1Type *result_ty;
    } primitive;
    struct {
      L1Type *element_ty;
      uint32_t byte_size;
      L1Type *result_ty;
    } alloca;
    struct {
      struct L1Expr *base;
      L1Type *struct_ty;
      uint32_t field_index;
      L1Type *field_ty;
    } field;
    struct {
      struct L1Expr *fn_ptr;
      L1Type *ret_ty;
      L1Type **param_tys;
      uint32_t param_count;
      struct L1Expr **args;
      uint32_t arg_count;
    } call_indirect;
  } data;
} L1Expr;

typedef struct L1Instruction {
  L1InstKind kind;
  union {
    struct {
      char *name;
      L1Expr *val;
    } set;
    struct {
      L1Expr *dest;
      L1Expr *val;
      L1Type *store_ty;
    } store;
    struct {
      struct L1Instruction *body;
    } loop_stmt;
    struct {
      L1Expr *val;
    } ret;
    struct {
      L1Expr *expr;
    } call_inst;
  } data;
  struct L1Instruction *next;
} L1Instruction;

typedef enum {
  TERM_NONE,
  TERM_RETURN,
  TERM_BRANCH,
  TERM_COND_BRANCH
} L1TerminatorKind;

typedef struct L1Terminator {
  L1TerminatorKind kind;
  union {
    L1Expr *ret_val;
    int target_id;
    struct {
      L1Expr *condition;
      int true_id;
      int false_id;
    } cond_branch;
  } data;
} L1Terminator;

typedef struct L1Block {
  int id;
  L1Instruction *body;
  L1Instruction *body_tail;
  L1Terminator *terminator;
  struct L1Subroutine *parent;
  struct L1Block *next;
} L1Block;

typedef struct L1PhiVar {
  char *name;
  L1Type *type;
  struct L1PhiVar *next;
} L1PhiVar;

typedef struct L1Subroutine {
  char *name;
  char *link_name;
  L1Type *ret_ty;
  uint32_t param_count;
  L1Type **param_tys;
  L1Block *blocks;
  L1Block *blocks_tail;
  L1PhiVar *phi_vars;
  int is_extern;
  struct L1Subroutine *next;
} L1Subroutine;

static L1Subroutine *g_subroutines_head = NULL;
static L1Subroutine *g_current_sub = NULL;
static L1Block *g_current_block = NULL;
static uint32_t g_temp_counter = 0;

static void append_instruction(L1Subroutine *sub, L1Instruction *inst) {
  if (!g_current_block) {
    fprintf(stderr, "ERROR: append_instruction with no current block\n");
    exit(1);
  }
  if (!g_current_block->body) {
    g_current_block->body = inst;
    g_current_block->body_tail = inst;
  } else {
    g_current_block->body_tail->next = inst;
    g_current_block->body_tail = inst;
  }
}

// ── Token types (for grouping) ─────────────────────────────────────────────

typedef enum {
  T_IDENT,
  T_NUMBER,
  T_STRING,
  T_PUNCT,
  T_LPAREN,
  T_RPAREN,
  T_LBRACKET,
  T_RBRACKET,
  T_LBRACE,
  T_RBRACE,
  T_EOF
} RawTokenKind;

typedef struct {
  RawTokenKind kind;
  char *val;
  int64_t int_val;
} RawToken;

typedef enum { GRP_PAREN, GRP_BRACKET, GRP_BRACE, GRP_ROOT } L1GroupKind;
typedef enum { TOK_RAW, TOK_GROUP } L1TokenKind;

typedef struct L1Token {
  L1TokenKind kind;
  union {
    RawToken raw;
    struct {
      L1GroupKind kind;
      struct L1Token **children;
      uint32_t count;
    } group;
  } data;
} L1Token;

// ============================================================================
// 2. Read a single byte from a pointer (used by lexer.lain)
// ============================================================================

uint8_t read_byte_at(const uint8_t *ptr, size_t offset) { return ptr[offset]; }

// ============================================================================
// 3. Simple Source Lexer (used by native_lex_and_group)
// ============================================================================

static int lexer_fgetc(const uint8_t **p, uint32_t *pos, uint32_t len) {
  if (*pos >= len)
    return EOF;
  return (*p)[(*pos)++];
}

static void lexer_ungetc(const uint8_t **p, uint32_t *pos, uint32_t len) {
  if (*pos > 0)
    (*pos)--;
}

static RawToken lex_one_token_from_mem(const uint8_t *src, uint32_t *pos,
                                       uint32_t len) {
  RawToken tok = {T_EOF, NULL, 0};
  const uint8_t *p = src;
  uint32_t *idx = pos;

  int c = lexer_fgetc(&p, idx, len);
  // skip whitespace
  while (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
    c = lexer_fgetc(&p, idx, len);
  }
  // skip line comments
  if (c == '/') {
    int next = lexer_fgetc(&p, idx, len);
    if (next == '/') {
      while (c != '\n' && c != EOF)
        c = lexer_fgetc(&p, idx, len);
      return lex_one_token_from_mem(src, idx, len);
    }
    lexer_ungetc(&p, idx, len);
  }
  if (c == EOF)
    return tok;
  if (c == '(') {
    tok.kind = T_LPAREN;
    return tok;
  }
  if (c == ')') {
    tok.kind = T_RPAREN;
    return tok;
  }
  if (c == '[') {
    tok.kind = T_LBRACKET;
    return tok;
  }
  if (c == ']') {
    tok.kind = T_RBRACKET;
    return tok;
  }
  if (c == '{') {
    tok.kind = T_LBRACE;
    return tok;
  }
  if (c == '}') {
    tok.kind = T_RBRACE;
    return tok;
  }

  char buf[1024];
  int i = 0;

  if (c == '"') {
    c = lexer_fgetc(&p, idx, len);
    while (c != '"' && c != EOF) {
      buf[i++] = c;
      c = lexer_fgetc(&p, idx, len);
    }
    buf[i] = '\0';
    tok.kind = T_STRING;
    tok.val = strdup(buf);
    return tok;
  }

  if (c >= '0' && c <= '9') {
    while (c >= '0' && c <= '9') {
      buf[i++] = c;
      c = lexer_fgetc(&p, idx, len);
    }
    lexer_ungetc(&p, idx, len);
    buf[i] = '\0';
    tok.kind = T_NUMBER;
    tok.int_val = atoll(buf);
    return tok;
  }

  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
    while ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_') {
      buf[i++] = c;
      c = lexer_fgetc(&p, idx, len);
    }
    lexer_ungetc(&p, idx, len);
    buf[i] = '\0';
    tok.kind = T_IDENT;
    tok.val = strdup(buf);
    return tok;
  }

  // punctuation (single or double char)
  buf[i++] = c;
  int next = lexer_fgetc(&p, idx, len);
  if ((c == ':' && next == ':') || (c == '-' && next == '>') ||
      (c == '=' && next == '>') || (c == '=' && next == '=') ||
      (c == '!' && next == '=') || (c == '<' && next == '=') ||
      (c == '>' && next == '=')) {
    buf[i++] = next;
  } else {
    lexer_ungetc(&p, idx, len);
  }
  buf[i] = '\0';
  tok.kind = T_PUNCT;
  tok.val = strdup(buf);
  return tok;
}

// ============================================================================
// 4. Token Tree Grouper
// ============================================================================

static L1Token *group_tokens_recursive(RawToken *raw, uint32_t total,
                                       uint32_t *idx,
                                       L1GroupKind current_kind) {
  L1Token *parent = malloc(sizeof(L1Token));
  parent->kind = TOK_GROUP;
  parent->data.group.kind = current_kind;

  L1Token **children = malloc(sizeof(L1Token *) * 1024);
  uint32_t count = 0;

  while (*idx < total) {
    RawToken t = raw[*idx];
    if (t.kind == T_RPAREN && current_kind == GRP_PAREN) {
      (*idx)++;
      break;
    }
    if (t.kind == T_RBRACKET && current_kind == GRP_BRACKET) {
      (*idx)++;
      break;
    }
    if (t.kind == T_RBRACE && current_kind == GRP_BRACE) {
      (*idx)++;
      break;
    }

    if (t.kind == T_LPAREN || t.kind == T_LBRACKET || t.kind == T_LBRACE) {
      (*idx)++;
      L1GroupKind k = (t.kind == T_LPAREN)     ? GRP_PAREN
                      : (t.kind == T_LBRACKET) ? GRP_BRACKET
                                               : GRP_BRACE;
      children[count++] = group_tokens_recursive(raw, total, idx, k);
    } else {
      L1Token *leaf = malloc(sizeof(L1Token));
      leaf->kind = TOK_RAW;
      leaf->data.raw = t;
      children[count++] = leaf;
      (*idx)++;
    }
  }

  parent->data.group.children = children;
  parent->data.group.count = count;
  return parent;
}

// ============================================================================
// 5. Top-level Form Splitter
// ============================================================================

L1Token **split_root_group(L1Token *root, uint32_t *out_count) {
  L1Token **forms = malloc(sizeof(L1Token *) * 64);
  uint32_t form_count = 0;
  L1Token **buf = malloc(sizeof(L1Token *) * 512);
  uint32_t buf_len = 0;
  int depth = 0;

  for (uint32_t i = 0; i < root->data.group.count; i++) {
    L1Token *child = root->data.group.children[i];
    if (child->kind == TOK_RAW && child->data.raw.kind == T_PUNCT &&
        strcmp(child->data.raw.val, ";") == 0 && depth == 0) {
      int needs_semi = 0;
      for (uint32_t j = 0; j < buf_len; j++) {
        if (buf[j]->kind == TOK_RAW) {
          if (buf[j]->data.raw.kind == T_IDENT) {
            if (strcmp(buf[j]->data.raw.val, "import") == 0 ||
                strcmp(buf[j]->data.raw.val, "us") == 0) {
              needs_semi = 1;
            }
            break;
          }
          if (buf[j]->data.raw.kind == T_PUNCT &&
              strcmp(buf[j]->data.raw.val, "@") == 0) {
            needs_semi = 1;
            break;
          }
        }
      }
      if (needs_semi)
        buf[buf_len++] = child;
      L1Token *form = malloc(sizeof(L1Token));
      form->kind = TOK_GROUP;
      form->data.group.kind = GRP_ROOT;
      L1Token **copy = malloc(sizeof(L1Token *) * buf_len);
      memcpy(copy, buf, sizeof(L1Token *) * buf_len);
      form->data.group.children = copy;
      form->data.group.count = buf_len;
      forms[form_count++] = form;
      buf = malloc(sizeof(L1Token *) * 512);
      buf_len = 0;
      continue;
    }
    int is_brace =
        (child->kind == TOK_GROUP && child->data.group.kind == GRP_BRACE);
    if (is_brace)
      depth++;
    buf[buf_len++] = child;
    if (is_brace) {
      depth--;
      if (depth == 0) {
        int next_is_semi_or_brace = 0;
        if (i + 1 < root->data.group.count) {
          L1Token *next = root->data.group.children[i + 1];
          if (next->kind == TOK_RAW && next->data.raw.kind == T_PUNCT &&
              strcmp(next->data.raw.val, ";") == 0)
            next_is_semi_or_brace = 1;
          if (next->kind == TOK_GROUP && next->data.group.kind == GRP_BRACE)
            next_is_semi_or_brace = 1;
        }
        if (!next_is_semi_or_brace) {
          L1Token *form = malloc(sizeof(L1Token));
          form->kind = TOK_GROUP;
          form->data.group.kind = GRP_ROOT;
          L1Token **copy = malloc(sizeof(L1Token *) * buf_len);
          memcpy(copy, buf, sizeof(L1Token *) * buf_len);
          form->data.group.children = copy;
          form->data.group.count = buf_len;
          forms[form_count++] = form;
          buf = malloc(sizeof(L1Token *) * 512);
          buf_len = 0;
        }
      }
    }
  }
  if (buf_len > 0) {
    L1Token *form = malloc(sizeof(L1Token));
    form->kind = TOK_GROUP;
    form->data.group.kind = GRP_ROOT;
    L1Token **copy = malloc(sizeof(L1Token *) * buf_len);
    memcpy(copy, buf, sizeof(L1Token *) * buf_len);
    form->data.group.children = copy;
    form->data.group.count = buf_len;
    forms[form_count++] = form;
  }
  *out_count = form_count;
  return forms;
}

// ============================================================================
// 6. Helper Utilities
// ============================================================================

static const char *sexp_to_c_string(sexp ctx, sexp val) {
  if (sexp_symbolp(val))
    return sexp_string_data(sexp_symbol_to_string(ctx, val));
  if (sexp_stringp(val))
    return sexp_string_data(val);
  return "";
}

static uint32_t get_list_length(sexp list) {
  uint32_t len = 0;
  sexp curr = list;
  while (sexp_pairp(curr)) {
    len++;
    curr = sexp_cdr(curr);
  }
  return len;
}

static L1Type *find_struct_by_name(const char *name) {
  L1StructInfo *info = g_struct_registry;
  while (info) {
    if (strcmp(info->name, name) == 0)
      return info->ty;
    info = info->next;
  }
  return NULL;
}

static void append_inst_to_block(L1Block *block, L1Instruction *inst) {
  if (block->body_tail) {
    block->body_tail->next = inst;
    block->body_tail = inst;
  } else {
    block->body = inst;
    block->body_tail = inst;
  }
}

static L1Type *infer_expr_type(L1Expr *expr) {
  switch (expr->kind) {
  case EXPR_VAR:
    return expr->data.var.ty;
  case EXPR_CONST: {
    L1Type *t = malloc(sizeof(L1Type));
    t->kind = TY_BITS;
    t->width = 64;
    return t;
  }
  case EXPR_LOAD:
    return expr->data.load.ty;
  case EXPR_ADD:
  case EXPR_SUB:
    return infer_expr_type(expr->data.bin.left);
  case EXPR_CALL:
    return expr->data.call.ret_ty;
  case EXPR_ARG: {
    L1Type *t = malloc(sizeof(L1Type));
    t->kind = TY_BITS;
    t->width = 64;
    return t;
  }
  case EXPR_FIELD:
    return expr->data.field.field_ty;
  case EXPR_ALLOCA:
    return expr->data.alloca.result_ty;
  default: {
    L1Type *t = malloc(sizeof(L1Type));
    t->kind = TY_BITS;
    t->width = 64;
    return t;
  }
  }
}

// ============================================================================
// 7. Core FFI Functions (for Scheme meta passes)
// ============================================================================

static sexp sexp_core_make_bits(sexp ctx, sexp self, sexp_sint_t n, sexp arg) {
  uint32_t width = sexp_unbox_fixnum(arg);
  L1Type *ty = malloc(sizeof(L1Type));
  ty->kind = TY_BITS;
  ty->width = width;
  return sexp_make_cpointer(ctx, SEXP_CPOINTER, ty, SEXP_FALSE, 0);
}

static sexp sexp_core_make_addr(sexp ctx, sexp self, sexp_sint_t n) {
  L1Type *ty = malloc(sizeof(L1Type));
  ty->kind = TY_ADDR;
  return sexp_make_cpointer(ctx, SEXP_CPOINTER, ty, SEXP_FALSE, 0);
}

static sexp sexp_core_make_unit(sexp ctx, sexp self, sexp_sint_t n) {
  L1Type *ty = malloc(sizeof(L1Type));
  ty->kind = TY_VOID;
  return sexp_make_cpointer(ctx, SEXP_CPOINTER, ty, SEXP_FALSE, 0);
}

static sexp sexp_type_registered(sexp ctx, sexp self, sexp_sint_t n,
                                 sexp name_val) {
  const char *name = sexp_to_c_string(ctx, name_val);
  L1Type *struct_ty = find_struct_by_name(name);
  if (struct_ty)
    return sexp_make_cpointer(ctx, SEXP_CPOINTER, struct_ty, SEXP_FALSE, 0);
  L1Type *ty = malloc(sizeof(L1Type));
  if (strcmp(name, "bool") == 0) {
    ty->kind = TY_BITS;
    ty->width = 1;
  } else if (strcmp(name, "i8") == 0 || strcmp(name, "u8") == 0) {
    ty->kind = TY_BITS;
    ty->width = 8;
  } else if (strcmp(name, "i32") == 0 || strcmp(name, "u32") == 0) {
    ty->kind = TY_BITS;
    ty->width = 32;
  } else if (strcmp(name, "i64") == 0 || strcmp(name, "u64") == 0) {
    ty->kind = TY_BITS;
    ty->width = 64;
  } else if (strcmp(name, "addr") == 0) {
    ty->kind = TY_ADDR;
  } else {
    ty->kind = TY_BITS;
    ty->width = 32;
  }
  return sexp_make_cpointer(ctx, SEXP_CPOINTER, ty, SEXP_FALSE, 0);
}

static sexp sexp_core_make_set(sexp ctx, sexp self, sexp_sint_t n,
                               sexp arg_name, sexp arg_val) {
  const char *name = sexp_to_c_string(ctx, arg_name);
  L1Expr *val = (L1Expr *)sexp_cpointer_value(arg_val);
  L1Instruction *inst = malloc(sizeof(L1Instruction));
  inst->kind = INST_SET;
  inst->data.set.name = strdup(name);
  inst->data.set.val = val;
  inst->next = NULL;
  return sexp_make_cpointer(ctx, SEXP_CPOINTER, inst, SEXP_FALSE, 0);
}

static sexp sexp_core_make_proc(sexp ctx, sexp self, sexp_sint_t n,
                                sexp arg_name, sexp arg_ret, sexp arg_params,
                                sexp arg_block) {
  const char *name = sexp_to_c_string(ctx, arg_name);
  L1Type *ret_ty = (L1Type *)sexp_cpointer_value(arg_ret);
  uint32_t param_count = get_list_length(arg_params);
  L1Type **param_tys = malloc(sizeof(L1Type *) * param_count);
  sexp curr = arg_params;
  for (uint32_t i = 0; i < param_count; i++) {
    param_tys[i] = (L1Type *)sexp_cpointer_value(sexp_car(curr));
    curr = sexp_cdr(curr);
  }
  L1Subroutine *sub = malloc(sizeof(L1Subroutine));
  sub->name = strdup(name);
  sub->ret_ty = ret_ty;
  sub->param_count = param_count;
  sub->param_tys = param_tys;
  sub->blocks = NULL;
  sub->blocks_tail = NULL;
  sub->phi_vars = NULL;
  sub->next = g_subroutines_head;
  g_subroutines_head = sub;
  return sexp_make_cpointer(ctx, SEXP_CPOINTER, sub, SEXP_FALSE, 0);
}

static sexp sexp_core_const_bits(sexp ctx, sexp self, sexp_sint_t n,
                                 sexp arg_block, sexp arg_ty, sexp arg_val) {
  L1Block *block = (L1Block *)sexp_cpointer_value(arg_block);
  L1Type *ty = (L1Type *)sexp_cpointer_value(arg_ty);
  int64_t val = sexp_unbox_fixnum(arg_val);
  L1Expr *expr = malloc(sizeof(L1Expr));
  expr->kind = EXPR_CONST;
  expr->data.const_val = val;
  return sexp_make_cpointer(ctx, SEXP_CPOINTER, expr, SEXP_FALSE, 0);
}

static sexp sexp_core_const_string(sexp ctx, sexp self, sexp_sint_t n,
                                   sexp arg_block, sexp arg_ty, sexp arg_val) {
  L1Block *block = (L1Block *)sexp_cpointer_value(arg_block);
  L1Type *ty = (L1Type *)sexp_cpointer_value(arg_ty);
  const char *content = sexp_to_c_string(ctx, arg_val);
  L1Expr *expr = malloc(sizeof(L1Expr));
  expr->kind = EXPR_STRING;
  expr->data.str_val.content = strdup(content);
  expr->data.str_val.ty = ty;
  return sexp_make_cpointer(ctx, SEXP_CPOINTER, expr, SEXP_FALSE, 0);
}

static sexp sexp_core_load(sexp ctx, sexp self, sexp_sint_t n, sexp arg_block,
                           sexp arg_addr, sexp arg_ty) {
  L1Block *block = (L1Block *)sexp_cpointer_value(arg_block);
  L1Expr *addr = (L1Expr *)sexp_cpointer_value(arg_addr);
  L1Type *ty = (L1Type *)sexp_cpointer_value(arg_ty);
  L1Expr *expr = malloc(sizeof(L1Expr));
  expr->kind = EXPR_LOAD;
  expr->data.load.addr = addr;
  expr->data.load.ty = ty;
  return sexp_make_cpointer(ctx, SEXP_CPOINTER, expr, SEXP_FALSE, 0);
}

static sexp sexp_core_store(sexp ctx, sexp self, sexp_sint_t n, sexp arg_block,
                            sexp arg_dest, sexp arg_val) {
  L1Block *block = (L1Block *)sexp_cpointer_value(arg_block);
  L1Expr *dest = (L1Expr *)sexp_cpointer_value(arg_dest);
  L1Expr *val = (L1Expr *)sexp_cpointer_value(arg_val);
  L1Instruction *inst = malloc(sizeof(L1Instruction));
  inst->kind = INST_STORE;
  inst->data.store.dest = dest;
  inst->data.store.val = val;
  inst->data.store.store_ty = NULL;
  inst->next = NULL;
  append_inst_to_block(block, inst);
  return SEXP_VOID;
}

static sexp sexp_core_begin_function(sexp ctx, sexp self, sexp_sint_t n,
                                     sexp arg_name, sexp arg_param_types,
                                     sexp arg_ret_ty) {
  // arg_name: symbol (e.g. |main|)
  // arg_param_types: list of L1Type* cpointers
  // arg_ret_ty: L1Type* cpointer
  const char *name = sexp_to_c_string(ctx, arg_name);

  L1Type *ret_ty = (L1Type *)sexp_cpointer_value(arg_ret_ty);

  uint32_t param_count = 0;
  L1Type **param_tys = NULL;
  if (sexp_pairp(arg_param_types)) {
    // Count params
    sexp curr = arg_param_types;
    while (sexp_pairp(curr)) {
      param_count++;
      curr = sexp_cdr(curr);
    }
    param_tys = malloc(sizeof(L1Type *) * param_count);
    curr = arg_param_types;
    for (uint32_t i = 0; i < param_count; i++) {
      param_tys[i] = (L1Type *)sexp_cpointer_value(sexp_car(curr));
      curr = sexp_cdr(curr);
    }
  }

  L1Subroutine *sub = malloc(sizeof(L1Subroutine));
  sub->name = strdup(name);
  sub->link_name = NULL;
  sub->ret_ty = ret_ty;
  sub->param_count = param_count;
  sub->param_tys = param_tys;
  sub->blocks = NULL;
  sub->blocks_tail = NULL;
  sub->phi_vars = NULL;
  sub->is_extern = 0;
  sub->next = g_subroutines_head;
  g_subroutines_head = sub;

  L1Block *block = malloc(sizeof(L1Block));
  block->id = 0;
  block->body = NULL;
  block->body_tail = NULL;
  block->terminator = NULL;
  block->parent = sub;
  block->next = NULL;
  sub->blocks = block;
  sub->blocks_tail = block;
  g_current_sub = sub;
  g_current_block = block;
  return sexp_make_cpointer(ctx, SEXP_CPOINTER, block, SEXP_FALSE, 0);
}

static sexp sexp_core_declare_extern_function(sexp ctx, sexp self,
                                              sexp_sint_t n, sexp arg_name,
                                              sexp arg_link_name,
                                              sexp arg_param_types,
                                              sexp arg_ret_ty) {
  const char *name = sexp_to_c_string(ctx, arg_name);
  const char *link_name = sexp_to_c_string(ctx, arg_link_name);
  L1Type *ret_ty = (L1Type *)sexp_cpointer_value(arg_ret_ty);

  uint32_t param_count = 0;
  L1Type **param_tys = NULL;
  if (sexp_pairp(arg_param_types)) {
    sexp curr = arg_param_types;
    while (sexp_pairp(curr)) {
      param_count++;
      curr = sexp_cdr(curr);
    }
    param_tys = malloc(sizeof(L1Type *) * param_count);
    curr = arg_param_types;
    for (uint32_t i = 0; i < param_count; i++) {
      param_tys[i] = (L1Type *)sexp_cpointer_value(sexp_car(curr));
      curr = sexp_cdr(curr);
    }
  }

  L1Subroutine *sub = malloc(sizeof(L1Subroutine));
  sub->name = strdup(name);
  sub->link_name = strdup(link_name);
  sub->ret_ty = ret_ty;
  sub->param_count = param_count;
  sub->param_tys = param_tys;
  sub->blocks = NULL;
  sub->blocks_tail = NULL;
  sub->phi_vars = NULL;
  sub->is_extern = 1;
  sub->next = g_subroutines_head;
  g_subroutines_head = sub;

  return sexp_make_cpointer(ctx, SEXP_CPOINTER, sub, SEXP_FALSE, 0);
}

static sexp sexp_core_function_by_name(sexp ctx, sexp self, sexp_sint_t n,
                                       sexp arg_name) {
  const char *name = sexp_to_c_string(ctx, arg_name);
  L1Subroutine *s = g_subroutines_head;
  while (s) {
    if (strcmp(s->name, name) == 0)
      return sexp_make_cpointer(ctx, SEXP_CPOINTER, s, SEXP_FALSE, 0);
    s = s->next;
  }
  // Auto-create stub for external runtime functions
  // Default signature: fn(addr, i32) -> void (L1 physical ABI knowledge)
  L1Subroutine *ext = malloc(sizeof(L1Subroutine));
  ext->name = strdup(name);
  ext->link_name = NULL;
  ext->ret_ty = malloc(sizeof(L1Type));
  ext->ret_ty->kind = TY_VOID;
  ext->param_count = 2;
  ext->param_tys = malloc(sizeof(L1Type *) * 2);
  ext->param_tys[0] = malloc(sizeof(L1Type));
  ext->param_tys[0]->kind = TY_ADDR;
  ext->param_tys[1] = malloc(sizeof(L1Type));
  ext->param_tys[1]->kind = TY_BITS;
  ext->param_tys[1]->width = 32;
  ext->blocks = NULL;
  ext->blocks_tail = NULL;
  ext->phi_vars = NULL;
  ext->is_extern = 1;
  ext->next = g_subroutines_head;
  g_subroutines_head = ext;
  return sexp_make_cpointer(ctx, SEXP_CPOINTER, ext, SEXP_FALSE, 0);
}

static sexp sexp_core_append_block(sexp ctx, sexp self, sexp_sint_t n,
                                   sexp arg_sub) {
  L1Subroutine *sub = (L1Subroutine *)sexp_cpointer_value(arg_sub);
  L1Block *block = malloc(sizeof(L1Block));
  block->id = sub->blocks_tail ? sub->blocks_tail->id + 1 : 0;
  block->body = NULL;
  block->body_tail = NULL;
  block->terminator = NULL;
  block->parent = sub;
  block->next = NULL;
  if (sub->blocks_tail) {
    sub->blocks_tail->next = block;
    sub->blocks_tail = block;
  } else {
    sub->blocks = block;
    sub->blocks_tail = block;
  }
  g_current_block = block;
  return sexp_make_cpointer(ctx, SEXP_CPOINTER, block, SEXP_FALSE, 0);
}

static sexp sexp_core_return_value(sexp ctx, sexp self, sexp_sint_t n,
                                   sexp arg_block, sexp arg_val) {
  L1Block *block = (L1Block *)sexp_cpointer_value(arg_block);
  L1Expr *val = (L1Expr *)sexp_cpointer_value(arg_val);
  if (!block->terminator) {
    block->terminator = malloc(sizeof(L1Terminator));
    block->terminator->kind = TERM_RETURN;
    block->terminator->data.ret_val = val;
  }
  return SEXP_VOID;
}

static sexp sexp_core_return_none(sexp ctx, sexp self, sexp_sint_t n,
                                  sexp arg_block) {
  L1Block *block = (L1Block *)sexp_cpointer_value(arg_block);
  if (!block->terminator) {
    block->terminator = malloc(sizeof(L1Terminator));
    block->terminator->kind = TERM_RETURN;
    block->terminator->data.ret_val = NULL;
  }
  return SEXP_VOID;
}

static sexp sexp_core_function_return_type(sexp ctx, sexp self, sexp_sint_t n,
                                           sexp arg_sub) {
  L1Subroutine *sub = (L1Subroutine *)sexp_cpointer_value(arg_sub);
  return sexp_make_cpointer(ctx, SEXP_CPOINTER, sub->ret_ty, SEXP_FALSE, 0);
}

static sexp sexp_core_function_param_types(sexp ctx, sexp self, sexp_sint_t n,
                                           sexp arg_sub) {
  L1Subroutine *sub = (L1Subroutine *)sexp_cpointer_value(arg_sub);
  sexp result = SEXP_NULL;
  for (int i = (int)sub->param_count - 1; i >= 0; i--) {
    result = sexp_cons(ctx,
                       sexp_make_cpointer(ctx, SEXP_CPOINTER, sub->param_tys[i],
                                          SEXP_FALSE, 0),
                       result);
  }
  return result;
}

static sexp sexp_core_call(sexp ctx, sexp self, sexp_sint_t n, sexp arg_block,
                           sexp arg_fn, sexp arg_args) {
  L1Subroutine *sub = (L1Subroutine *)sexp_cpointer_value(arg_fn);
  uint32_t arg_count = get_list_length(arg_args);
  L1Expr **args = malloc(sizeof(L1Expr *) * arg_count);
  sexp curr = arg_args;
  for (uint32_t i = 0; i < arg_count; i++) {
    args[i] = (L1Expr *)sexp_cpointer_value(sexp_car(curr));
    curr = sexp_cdr(curr);
  }
  L1Expr *expr = malloc(sizeof(L1Expr));
  expr->kind = EXPR_CALL;
  expr->data.call.fn_name = strdup(sub->name);
  expr->data.call.args = args;
  expr->data.call.arg_count = arg_count;
  expr->data.call.ret_ty = sub->ret_ty;

  // Also append as instruction
  L1Instruction *inst = malloc(sizeof(L1Instruction));
  inst->kind = INST_CALL;
  inst->data.call_inst.expr = expr;
  inst->next = NULL;
  append_instruction(g_current_sub, inst);

  return sexp_make_cpointer(ctx, SEXP_CPOINTER, expr, SEXP_FALSE, 0);
}

static sexp sexp_core_call_expr(sexp ctx, sexp self, sexp_sint_t n,
                                sexp block_val, sexp fn_val, sexp args_val) {
  L1Subroutine *sub = (L1Subroutine *)sexp_cpointer_value(fn_val);
  uint32_t arg_count = get_list_length(args_val);

  L1Expr **args = malloc(sizeof(L1Expr *) * arg_count);
  sexp curr = args_val;
  for (uint32_t i = 0; i < arg_count; i++) {
    args[i] = (L1Expr *)sexp_cpointer_value(sexp_car(curr));
    curr = sexp_cdr(curr);
  }

  L1Expr *expr = malloc(sizeof(L1Expr));
  expr->kind = EXPR_CALL;
  expr->data.call.fn_name = strdup(sub->name);
  expr->data.call.args = args;
  expr->data.call.arg_count = arg_count;
  expr->data.call.ret_ty = sub->ret_ty;
  // Does NOT append instruction — caller decides how to emit

  return sexp_make_cpointer(ctx, SEXP_CPOINTER, expr, SEXP_FALSE, 0);
}

static sexp sexp_core_assign_temp(sexp ctx, sexp self, sexp_sint_t n,
                                  sexp block_val, sexp expr_val) {
  L1Expr *expr = (L1Expr *)sexp_cpointer_value(expr_val);
  char name[32];
  snprintf(name, sizeof(name), "_t%d", g_temp_counter++);

  // Append INST_SET: auto _tN = <expr>;
  L1Instruction *inst = malloc(sizeof(L1Instruction));
  inst->kind = INST_SET;
  inst->data.set.name = strdup(name);
  inst->data.set.val = expr;
  inst->next = NULL;
  append_instruction(g_current_sub, inst);

  // Return EXPR_VAR referencing the temp variable
  L1Expr *var = malloc(sizeof(L1Expr));
  var->kind = EXPR_VAR;
  var->data.var.name = strdup(name);
  return sexp_make_cpointer(ctx, SEXP_CPOINTER, var, SEXP_FALSE, 0);
}

static sexp sexp_core_primitive(sexp ctx, sexp self, sexp_sint_t n,
                                sexp arg_block, sexp arg_op, sexp arg_operands,
                                sexp arg_result_ty) {
  L1Block *block = (L1Block *)sexp_cpointer_value(arg_block);
  const char *op = sexp_to_c_string(ctx, arg_op);
  uint32_t operand_count = get_list_length(arg_operands);
  L1Expr **operands = malloc(sizeof(L1Expr *) * operand_count);
  sexp curr = arg_operands;
  for (uint32_t i = 0; i < operand_count; i++) {
    operands[i] = (L1Expr *)sexp_cpointer_value(sexp_car(curr));
    curr = sexp_cdr(curr);
  }
  L1Type *result_ty = (L1Type *)sexp_cpointer_value(arg_result_ty);
  L1Expr *expr = malloc(sizeof(L1Expr));
  expr->kind = EXPR_PRIMITIVE;
  expr->data.primitive.opcode = strdup(op);
  expr->data.primitive.operands = operands;
  expr->data.primitive.operand_count = operand_count;
  expr->data.primitive.result_ty = result_ty;
  return sexp_make_cpointer(ctx, SEXP_CPOINTER, expr, SEXP_FALSE, 0);
}

static sexp sexp_core_local_alloc(sexp ctx, sexp self, sexp_sint_t n,
                                  sexp arg_block, sexp arg_element_ty,
                                  sexp arg_byte_size) {
  L1Block *block = (L1Block *)sexp_cpointer_value(arg_block);
  L1Type *element_ty = (L1Type *)sexp_cpointer_value(arg_element_ty);
  uint32_t byte_size = sexp_unbox_fixnum(arg_byte_size);
  L1Type *result_ty = malloc(sizeof(L1Type));
  result_ty->kind = TY_ADDR;
  L1Expr *expr = malloc(sizeof(L1Expr));
  expr->kind = EXPR_ALLOCA;
  expr->data.alloca.element_ty = element_ty;
  expr->data.alloca.byte_size = byte_size;
  expr->data.alloca.result_ty = result_ty;
  return sexp_make_cpointer(ctx, SEXP_CPOINTER, expr, SEXP_FALSE, 0);
}

static sexp sexp_core_param(sexp ctx, sexp self, sexp_sint_t n,
                            sexp arg_function, sexp arg_index) {
  // arg_function is L1Block* cpointer (we only need the index for EXPR_ARG)
  uint32_t idx = sexp_unbox_fixnum(arg_index);
  L1Expr *expr = malloc(sizeof(L1Expr));
  expr->kind = EXPR_ARG;
  expr->data.arg_idx = idx;
  return sexp_make_cpointer(ctx, SEXP_CPOINTER, expr, SEXP_FALSE, 0);
}

static sexp sexp_core_block_function(sexp ctx, sexp self, sexp_sint_t n,
                                     sexp arg_block) {
  L1Block *block = (L1Block *)sexp_cpointer_value(arg_block);
  if (block->parent)
    return sexp_make_cpointer(ctx, SEXP_CPOINTER, block->parent, SEXP_FALSE, 0);
  return SEXP_FALSE;
}

static sexp sexp_core_branch(sexp ctx, sexp self, sexp_sint_t n, sexp arg_block,
                             sexp arg_target) {
  L1Block *block = (L1Block *)sexp_cpointer_value(arg_block);
  L1Block *target = (L1Block *)sexp_cpointer_value(arg_target);
  if (!block->terminator) {
    block->terminator = malloc(sizeof(L1Terminator));
    block->terminator->kind = TERM_BRANCH;
    block->terminator->data.target_id = target->id;
  }
  return SEXP_VOID;
}

static sexp sexp_core_cond_branch(sexp ctx, sexp self, sexp_sint_t n,
                                  sexp arg_block, sexp arg_cond, sexp arg_true,
                                  sexp arg_false) {
  L1Block *block = (L1Block *)sexp_cpointer_value(arg_block);
  L1Expr *cond = (L1Expr *)sexp_cpointer_value(arg_cond);
  L1Block *true_bb = (L1Block *)sexp_cpointer_value(arg_true);
  L1Block *false_bb = (L1Block *)sexp_cpointer_value(arg_false);
  if (!block->terminator) {
    block->terminator = malloc(sizeof(L1Terminator));
    block->terminator->kind = TERM_COND_BRANCH;
    block->terminator->data.cond_branch.condition = cond;
    block->terminator->data.cond_branch.true_id = true_bb->id;
    block->terminator->data.cond_branch.false_id = false_bb->id;
  }
  return SEXP_VOID;
}

static sexp sexp_core_phi(sexp ctx, sexp self, sexp_sint_t n, sexp arg_block,
                          sexp arg_ty, sexp arg_pred1, sexp arg_val1,
                          sexp arg_pred2, sexp arg_val2) {
  L1Block *block = (L1Block *)sexp_cpointer_value(arg_block);
  L1Type *ty = (L1Type *)sexp_cpointer_value(arg_ty);
  L1Block *pred1 = (L1Block *)sexp_cpointer_value(arg_pred1);
  L1Expr *val1 = (L1Expr *)sexp_cpointer_value(arg_val1);
  L1Block *pred2 = (L1Block *)sexp_cpointer_value(arg_pred2);
  L1Expr *val2 = (L1Expr *)sexp_cpointer_value(arg_val2);

  // Create a phi variable name
  static int phi_counter = 0;
  char buf[64];
  snprintf(buf, sizeof(buf), "__phi_%d", phi_counter++);

  // Register phi variable in the subroutine
  L1PhiVar *pv = malloc(sizeof(L1PhiVar));
  pv->name = strdup(buf);
  pv->type = ty;
  pv->next = block->parent->phi_vars;
  block->parent->phi_vars = pv;

  // In pred1: append a PHI_ASSIGN inst
  L1Instruction *assign1 = malloc(sizeof(L1Instruction));
  assign1->kind = INST_PHI_ASSIGN;
  assign1->data.set.name = strdup(buf);
  assign1->data.set.val = val1;
  assign1->next = NULL;
  append_inst_to_block(pred1, assign1);

  // In pred2: append a PHI_ASSIGN inst
  L1Instruction *assign2 = malloc(sizeof(L1Instruction));
  assign2->kind = INST_PHI_ASSIGN;
  assign2->data.set.name = strdup(buf);
  assign2->data.set.val = val2;
  assign2->next = NULL;
  append_inst_to_block(pred2, assign2);

  // In cont block: create a var expression referring to the phi variable
  L1Expr *expr = malloc(sizeof(L1Expr));
  expr->kind = EXPR_VAR;
  expr->data.var.name = strdup(buf);
  expr->data.var.ty = ty;
  return sexp_make_cpointer(ctx, SEXP_CPOINTER, expr, SEXP_FALSE, 0);
}

static sexp sexp_core_type_is_void(sexp ctx, sexp self, sexp_sint_t n,
                                   sexp arg_ty) {
  L1Type *ty = (L1Type *)sexp_cpointer_value(arg_ty);
  return (ty && ty->kind == TY_VOID) ? SEXP_TRUE : SEXP_FALSE;
}

// ============================================================================
// 8. Struct/tuple FFI functions
// ============================================================================

static sexp sexp_core_declare_struct_name(sexp ctx, sexp self, sexp_sint_t n,
                                          sexp arg_name) {
  const char *name = sexp_to_c_string(ctx, arg_name);
  L1StructInfo *info = malloc(sizeof(L1StructInfo));
  info->name = strdup(name);
  info->ty = NULL; // will be filled by declare-struct!
  info->next = g_struct_registry;
  g_struct_registry = info;
  return SEXP_VOID;
}

static sexp sexp_core_declare_struct(sexp ctx, sexp self, sexp_sint_t n,
                                     sexp arg_info, sexp arg_fields) {
  L1StructInfo *info = (L1StructInfo *)sexp_cpointer_value(arg_info);
  if (!info) {
    // arg_info could be a string name
    const char *name = sexp_to_c_string(ctx, arg_info);
    info = find_struct_by_name(name) ? NULL : malloc(sizeof(L1StructInfo));
    if (!info)
      return SEXP_VOID;
    // find the existing info entry
    L1StructInfo *s = g_struct_registry;
    while (s) {
      if (strcmp(s->name, name) == 0) {
        info = s;
        break;
      }
      s = s->next;
    }
  }
  if (!info || !sexp_pairp(arg_fields))
    return SEXP_VOID;

  uint32_t field_count = get_list_length(arg_fields);
  L1ProductField *fields = malloc(sizeof(L1ProductField) * field_count);
  uint32_t offset = 0;

  sexp curr = arg_fields;
  for (uint32_t i = 0; i < field_count; i++) {
    sexp field_pair = sexp_car(curr);
    const char *fname = sexp_to_c_string(ctx, sexp_car(field_pair));
    L1Type *fty = (L1Type *)sexp_cpointer_value(sexp_cdr(field_pair));
    uint32_t field_size = (fty->kind == TY_BITS) ? (fty->width / 8) : 8;
    // Align offset
    if (field_size == 8 && (offset % 8 != 0))
      offset = (offset + 7) & ~7;
    fields[i].name = strdup(fname);
    fields[i].ty = fty;
    fields[i].offset = offset;
    offset += field_size;
  }

  L1Type *ty = malloc(sizeof(L1Type));
  ty->kind = TY_PRODUCT;
  ty->width = offset; // total struct size
  ty->field_count = field_count;
  ty->fields = fields;
  ty->struct_name = strdup(info->name);
  info->ty = ty;
  return SEXP_VOID;
}

static sexp sexp_core_struct_type(sexp ctx, sexp self, sexp_sint_t n,
                                  sexp arg_name) {
  const char *name = sexp_to_c_string(ctx, arg_name);
  L1Type *ty = find_struct_by_name(name);
  if (ty)
    return sexp_make_cpointer(ctx, SEXP_CPOINTER, ty, SEXP_FALSE, 0);
  return SEXP_FALSE;
}

static sexp sexp_core_struct_field_type(sexp ctx, sexp self, sexp_sint_t n,
                                        sexp arg_name, sexp arg_field) {
  const char *name = sexp_to_c_string(ctx, arg_name);
  const char *field = sexp_to_c_string(ctx, arg_field);
  L1Type *ty = find_struct_by_name(name);
  if (ty && ty->kind == TY_PRODUCT) {
    for (uint32_t i = 0; i < ty->field_count; i++) {
      if (strcmp(ty->fields[i].name, field) == 0)
        return sexp_make_cpointer(ctx, SEXP_CPOINTER, ty->fields[i].ty,
                                  SEXP_FALSE, 0);
    }
  }
  return SEXP_FALSE;
}

static sexp sexp_core_struct_field_type_from_type(sexp ctx, sexp self,
                                                  sexp_sint_t n, sexp arg_ty,
                                                  sexp arg_field) {
  L1Type *ty = (L1Type *)sexp_cpointer_value(arg_ty);
  const char *field = sexp_to_c_string(ctx, arg_field);
  if (ty && ty->kind == TY_PRODUCT) {
    for (uint32_t i = 0; i < ty->field_count; i++) {
      if (strcmp(ty->fields[i].name, field) == 0)
        return sexp_make_cpointer(ctx, SEXP_CPOINTER, ty->fields[i].ty,
                                  SEXP_FALSE, 0);
    }
  }
  return SEXP_FALSE;
}

static sexp sexp_core_struct_field_index(sexp ctx, sexp self, sexp_sint_t n,
                                         sexp arg_name, sexp arg_field) {
  const char *name = sexp_to_c_string(ctx, arg_name);
  const char *field = sexp_to_c_string(ctx, arg_field);
  L1Type *ty = find_struct_by_name(name);
  if (ty && ty->kind == TY_PRODUCT) {
    for (uint32_t i = 0; i < ty->field_count; i++) {
      if (strcmp(ty->fields[i].name, field) == 0)
        return sexp_make_fixnum(i);
    }
  }
  return sexp_make_fixnum(0);
}

static sexp sexp_core_struct_field_index_from_type(sexp ctx, sexp self,
                                                   sexp_sint_t n, sexp arg_ty,
                                                   sexp arg_field) {
  L1Type *ty = (L1Type *)sexp_cpointer_value(arg_ty);
  const char *field = sexp_to_c_string(ctx, arg_field);
  if (ty && ty->kind == TY_PRODUCT) {
    for (uint32_t i = 0; i < ty->field_count; i++) {
      if (strcmp(ty->fields[i].name, field) == 0)
        return sexp_make_fixnum(i);
    }
  }
  return sexp_make_fixnum(0);
}

static sexp sexp_core_aggregate(sexp ctx, sexp self, sexp_sint_t n,
                                sexp arg_block, sexp arg_struct_ty,
                                sexp arg_fields) {
  L1Block *block = (L1Block *)sexp_cpointer_value(arg_block);
  L1Type *struct_ty = (L1Type *)sexp_cpointer_value(arg_struct_ty);
  uint32_t field_count = get_list_length(arg_fields);

  // alloca for the struct
  uint32_t struct_size = struct_ty->width;
  if (struct_size == 0)
    struct_size = field_count * 8;

  // Create EXPR_ALLOCA for struct storage
  L1Type *result_ty = malloc(sizeof(L1Type));
  result_ty->kind = TY_ADDR;
  L1Expr *alloca_expr = malloc(sizeof(L1Expr));
  alloca_expr->kind = EXPR_ALLOCA;
  alloca_expr->data.alloca.element_ty = struct_ty;
  alloca_expr->data.alloca.byte_size = struct_size;
  alloca_expr->data.alloca.result_ty = result_ty;

  // Add set instruction: auto __agg = alloca(size)
  static int agg_counter = 0;
  char agg_name[64];
  snprintf(agg_name, sizeof(agg_name), "__agg_%d", agg_counter++);
  L1Instruction *set_inst = malloc(sizeof(L1Instruction));
  set_inst->kind = INST_SET;
  set_inst->data.set.name = strdup(agg_name);
  set_inst->data.set.val = alloca_expr;
  set_inst->next = NULL;
  append_inst_to_block(block, set_inst);

  // Build EXPR_VAR for the aggregate
  L1Expr *agg_var = malloc(sizeof(L1Expr));
  agg_var->kind = EXPR_VAR;
  agg_var->data.var.name = strdup(agg_name);
  agg_var->data.var.ty = result_ty;

  // Store each field into the struct
  sexp curr = arg_fields;
  for (uint32_t i = 0; i < field_count; i++) {
    sexp field_pair = sexp_car(curr);
    L1Expr *field_val = (L1Expr *)sexp_cpointer_value(sexp_cdr(field_pair));
    L1Type *field_ty = infer_expr_type(field_val);
    uint32_t field_offset = (struct_ty->kind == TY_PRODUCT && struct_ty->fields)
                                ? struct_ty->fields[i].offset
                                : (i * 8);

    // Build EXPR_LEA: (uint8_t*)agg_var + offset
    L1Expr *offset_expr = malloc(sizeof(L1Expr));
    offset_expr->kind = EXPR_CONST;
    offset_expr->data.const_val = field_offset;
    L1Expr *dest_expr = malloc(sizeof(L1Expr));
    dest_expr->kind = EXPR_LEA;
    dest_expr->data.lea.base = agg_var;
    dest_expr->data.lea.idx = offset_expr;
    dest_expr->data.lea.scale = 1;
    dest_expr->data.lea.offset = 0;

    L1Instruction *store_inst = malloc(sizeof(L1Instruction));
    store_inst->kind = INST_STORE;
    store_inst->data.store.dest = dest_expr;
    store_inst->data.store.val = field_val;
    store_inst->data.store.store_ty = field_ty;
    store_inst->next = NULL;
    append_inst_to_block(block, store_inst);
    curr = sexp_cdr(curr);
  }

  return sexp_make_cpointer(ctx, SEXP_CPOINTER, agg_var, SEXP_FALSE, 0);
}

static sexp sexp_core_field(sexp ctx, sexp self, sexp_sint_t n, sexp arg_block,
                            sexp arg_base, sexp arg_struct_ty,
                            sexp arg_field_idx, sexp arg_field_ty) {
  L1Block *block = (L1Block *)sexp_cpointer_value(arg_block);
  L1Expr *base = (L1Expr *)sexp_cpointer_value(arg_base);
  L1Type *struct_ty = (L1Type *)sexp_cpointer_value(arg_struct_ty);
  uint32_t field_idx = sexp_unbox_fixnum(arg_field_idx);
  L1Type *field_ty = (L1Type *)sexp_cpointer_value(arg_field_ty);
  L1Expr *expr = malloc(sizeof(L1Expr));
  expr->kind = EXPR_FIELD;
  expr->data.field.base = base;
  expr->data.field.struct_ty = struct_ty;
  expr->data.field.field_index = field_idx;
  expr->data.field.field_ty = field_ty;
  return sexp_make_cpointer(ctx, SEXP_CPOINTER, expr, SEXP_FALSE, 0);
}

static sexp sexp_core_call_indirect(sexp ctx, sexp self, sexp_sint_t n,
                                    sexp arg_block, sexp arg_fn_ptr,
                                    sexp arg_ret_ty, sexp arg_args) {
  L1Block *block = (L1Block *)sexp_cpointer_value(arg_block);
  L1Expr *fn_ptr = (L1Expr *)sexp_cpointer_value(arg_fn_ptr);
  L1Type *ret_ty = (L1Type *)sexp_cpointer_value(arg_ret_ty);
  uint32_t arg_count = get_list_length(arg_args);
  L1Expr **args = malloc(sizeof(L1Expr *) * arg_count);
  sexp curr = arg_args;
  for (uint32_t i = 0; i < arg_count; i++) {
    args[i] = (L1Expr *)sexp_cpointer_value(sexp_car(curr));
    curr = sexp_cdr(curr);
  }
  L1Expr *expr = malloc(sizeof(L1Expr));
  expr->kind = EXPR_CALL_INDIRECT;
  expr->data.call_indirect.fn_ptr = fn_ptr;
  expr->data.call_indirect.ret_ty = ret_ty;
  expr->data.call_indirect.param_tys = NULL;
  expr->data.call_indirect.param_count = 0;
  expr->data.call_indirect.args = args;
  expr->data.call_indirect.arg_count = arg_count;
  return sexp_make_cpointer(ctx, SEXP_CPOINTER, expr, SEXP_FALSE, 0);
}

// ============================================================================
// 9. C Code Emission
// ============================================================================

static void emit_c_type(L1Type *ty, FILE *out) {
  if (!ty) {
    fprintf(out, "void");
    return;
  }
  if (ty->kind == TY_BITS) {
    fprintf(out, "uint%d_t", ty->width);
  } else if (ty->kind == TY_ADDR) {
    fprintf(out, "void*");
  } else if (ty->kind == TY_PRODUCT) {
    fprintf(out, "void*");
  } else {
    fprintf(out, "void");
  }
}

static void emit_c_expr(L1Expr *expr, FILE *out);

static void emit_c_expr(L1Expr *expr, FILE *out) {
  if (!expr) {
    fprintf(out, "0");
    return;
  }
  switch (expr->kind) {
  case EXPR_VAR:
    fprintf(out, "%s", expr->data.var.name);
    break;
  case EXPR_CONST:
    fprintf(out, "%lld", (long long)expr->data.const_val);
    break;
  case EXPR_ARG:
    fprintf(out, "arg%d", expr->data.arg_idx);
    break;
  case EXPR_LOAD:
    fprintf(out, "*(");
    emit_c_type(expr->data.load.ty, out);
    fprintf(out, "*)(");
    emit_c_expr(expr->data.load.addr, out);
    fprintf(out, ")");
    break;
  case EXPR_LEA:
    fprintf(out, "(void*)((uintptr_t)(");
    emit_c_expr(expr->data.lea.base, out);
    fprintf(out, ") + (uintptr_t)(");
    emit_c_expr(expr->data.lea.idx, out);
    fprintf(out, ") * %d + %d)", expr->data.lea.scale, expr->data.lea.offset);
    break;
  case EXPR_ADD:
    fprintf(out, "(");
    emit_c_expr(expr->data.bin.left, out);
    fprintf(out, " + ");
    emit_c_expr(expr->data.bin.right, out);
    fprintf(out, ")");
    break;
  case EXPR_SUB:
    fprintf(out, "(");
    emit_c_expr(expr->data.bin.left, out);
    fprintf(out, " - ");
    emit_c_expr(expr->data.bin.right, out);
    fprintf(out, ")");
    break;
  case EXPR_CALL:
    fprintf(out, "%s(", expr->data.call.fn_name);
    for (uint32_t i = 0; i < expr->data.call.arg_count; i++) {
      emit_c_expr(expr->data.call.args[i], out);
      if (i < expr->data.call.arg_count - 1)
        fprintf(out, ", ");
    }
    fprintf(out, ")");
    break;
  case EXPR_STRING:
    fprintf(out, "\"");
    for (char *p = expr->data.str_val.content; *p; p++) {
      switch (*p) {
      case '\n':
        fprintf(out, "\\n");
        break;
      case '\t':
        fprintf(out, "\\t");
        break;
      case '\r':
        fprintf(out, "\\r");
        break;
      case '\\':
        fprintf(out, "\\\\");
        break;
      case '"':
        fprintf(out, "\\\"");
        break;
      default:
        fputc(*p, out);
        break;
      }
    }
    fprintf(out, "\"");
    break;
  case EXPR_PRIMITIVE: {
    const char *op = expr->data.primitive.opcode;
    uint32_t nops = expr->data.primitive.operand_count;
    if (nops == 1) {
      if (strcmp(op, "cpu.popcount") == 0) {
        fprintf(out, "__builtin_popcountll(");
        emit_c_expr(expr->data.primitive.operands[0], out);
        fprintf(out, ")");
      } else if (strcmp(op, "cpu.leading-zeros") == 0) {
        fprintf(out, "__builtin_clzll(");
        emit_c_expr(expr->data.primitive.operands[0], out);
        fprintf(out, ")");
      } else if (strcmp(op, "cpu.bswap") == 0) {
        fprintf(out, "__builtin_bswap64(");
        emit_c_expr(expr->data.primitive.operands[0], out);
        fprintf(out, ")");
      } else {
        fprintf(out, "0");
      }
    } else if (nops >= 2) {
      L1Expr *left = expr->data.primitive.operands[0];
      L1Expr *right = expr->data.primitive.operands[1];
      if (strcmp(op, "integer.add") == 0 || strcmp(op, "float.add") == 0) {
        fprintf(out, "(");
        emit_c_expr(left, out);
        fprintf(out, " + ");
        emit_c_expr(right, out);
        fprintf(out, ")");
      } else if (strcmp(op, "integer.sub") == 0 ||
                 strcmp(op, "float.sub") == 0) {
        fprintf(out, "(");
        emit_c_expr(left, out);
        fprintf(out, " - ");
        emit_c_expr(right, out);
        fprintf(out, ")");
      } else if (strcmp(op, "integer.mul") == 0 ||
                 strcmp(op, "float.mul") == 0) {
        fprintf(out, "(");
        emit_c_expr(left, out);
        fprintf(out, " * ");
        emit_c_expr(right, out);
        fprintf(out, ")");
      } else if (strcmp(op, "integer.div") == 0 ||
                 strcmp(op, "float.div") == 0) {
        fprintf(out, "(");
        emit_c_expr(left, out);
        fprintf(out, " / ");
        emit_c_expr(right, out);
        fprintf(out, ")");
      } else if (strcmp(op, "integer.eq") == 0 || strcmp(op, "float.eq") == 0) {
        fprintf(out, "(");
        emit_c_expr(left, out);
        fprintf(out, " == ");
        emit_c_expr(right, out);
        fprintf(out, ")");
      } else if (strcmp(op, "integer.ne") == 0) {
        fprintf(out, "(");
        emit_c_expr(left, out);
        fprintf(out, " != ");
        emit_c_expr(right, out);
        fprintf(out, ")");
      } else if (strcmp(op, "integer.lt") == 0 || strcmp(op, "float.lt") == 0) {
        fprintf(out, "((int64_t)(");
        emit_c_expr(left, out);
        fprintf(out, ") < (int64_t)(");
        emit_c_expr(right, out);
        fprintf(out, "))");
      } else if (strcmp(op, "integer.le") == 0) {
        fprintf(out, "((int64_t)(");
        emit_c_expr(left, out);
        fprintf(out, ") <= (int64_t)(");
        emit_c_expr(right, out);
        fprintf(out, "))");
      } else if (strcmp(op, "integer.gt") == 0) {
        fprintf(out, "((int64_t)(");
        emit_c_expr(left, out);
        fprintf(out, ") > (int64_t)(");
        emit_c_expr(right, out);
        fprintf(out, "))");
      } else if (strcmp(op, "integer.ge") == 0) {
        fprintf(out, "((int64_t)(");
        emit_c_expr(left, out);
        fprintf(out, ") >= (int64_t)(");
        emit_c_expr(right, out);
        fprintf(out, "))");
      } else if (strcmp(op, "cpu.rotate-left") == 0) {
        fprintf(out, "__builtin_rotateleft64(");
        emit_c_expr(left, out);
        fprintf(out, ", ");
        emit_c_expr(right, out);
        fprintf(out, ")");
      } else if (strcmp(op, "cpu.extract-bits") == 0) {
        fprintf(out, "(");
        emit_c_expr(left, out);
        fprintf(out, " & ");
        emit_c_expr(right, out);
        fprintf(out, ")");
      } else {
        fprintf(out, "0");
      }
    } else {
      fprintf(out, "0");
    }
  } break;
  case EXPR_ALLOCA:
    if (expr->data.alloca.byte_size > 0) {
      fprintf(out, "alloca(%u)", expr->data.alloca.byte_size);
    } else {
      fprintf(out, "alloca(sizeof(");
      emit_c_type(expr->data.alloca.element_ty, out);
      fprintf(out, "))");
    }
    break;
  case EXPR_FIELD:
    fprintf(out, "*(");
    emit_c_type(expr->data.field.field_ty, out);
    fprintf(out, "*)((uint8_t*)(");
    emit_c_expr(expr->data.field.base, out);
    fprintf(out, ") + %u)",
            expr->data.field.struct_ty->fields[expr->data.field.field_index]
                .offset);
    break;
  case EXPR_CALL_INDIRECT:
    fprintf(out, "((");
    emit_c_type(expr->data.call_indirect.ret_ty, out);
    fprintf(out, " (*)(");
    for (uint32_t i = 0; i < expr->data.call_indirect.param_count; i++) {
      emit_c_type(expr->data.call_indirect.param_tys[i], out);
      if (i < expr->data.call_indirect.param_count - 1)
        fprintf(out, ", ");
    }
    fprintf(out, "))(");
    emit_c_expr(expr->data.call_indirect.fn_ptr, out);
    fprintf(out, "))(");
    for (uint32_t i = 0; i < expr->data.call_indirect.arg_count; i++) {
      emit_c_expr(expr->data.call_indirect.args[i], out);
      if (i < expr->data.call_indirect.arg_count - 1)
        fprintf(out, ", ");
    }
    fprintf(out, ")");
    break;
  }
}

static void emit_c_block_terminator(L1Block *block, FILE *out);

static void emit_c_instructions(L1Block *block, FILE *out) {
  L1Instruction *inst = block->body;
  while (inst) {
    switch (inst->kind) {
    case INST_SET:
      fprintf(out, "    auto %s = ", inst->data.set.name);
      emit_c_expr(inst->data.set.val, out);
      fprintf(out, ";\n");
      break;
    case INST_PHI_ASSIGN:
      fprintf(out, "    %s = ", inst->data.set.name);
      emit_c_expr(inst->data.set.val, out);
      fprintf(out, ";\n");
      break;
    case INST_STORE: {
      L1Type *store_ty = inst->data.store.store_ty;
      if (!store_ty)
        store_ty = infer_expr_type(inst->data.store.val);
      fprintf(out, "    *(");
      emit_c_type(store_ty, out);
      fprintf(out, "*)(");
      emit_c_expr(inst->data.store.dest, out);
      fprintf(out, ") = ");
      emit_c_expr(inst->data.store.val, out);
      fprintf(out, ";\n");
      break;
    }
    case INST_LOOP:
      fprintf(out, "    while (1) {\n");
      {
        L1Instruction *li = inst->data.loop_stmt.body;
        while (li) {
          if (li->kind == INST_BREAK) {
            fprintf(out, "    break;\n");
          } else if (li->kind == INST_SET) {
            fprintf(out, "        auto %s = ", li->data.set.name);
            emit_c_expr(li->data.set.val, out);
            fprintf(out, ";\n");
          }
          li = li->next;
        }
      }
      fprintf(out, "    }\n");
      break;
    case INST_CALL:
      if (block->terminator && block->terminator->kind == TERM_RETURN &&
          block->terminator->data.ret_val == inst->data.call_inst.expr) {
        break;
      }
      fprintf(out, "    ");
      emit_c_expr(inst->data.call_inst.expr, out);
      fprintf(out, ";\n");
      break;
    case INST_BREAK:
    case INST_RETURN:
    default:
      break;
    }
    inst = inst->next;
  }
}

static void emit_c_block_terminator(L1Block *block, FILE *out) {
  if (!block->terminator)
    return;
  switch (block->terminator->kind) {
  case TERM_RETURN:
    if (block->terminator->data.ret_val) {
      fprintf(out, "    return ");
      emit_c_expr(block->terminator->data.ret_val, out);
      fprintf(out, ";\n");
    } else {
      fprintf(out, "    return;\n");
    }
    break;
  case TERM_BRANCH:
    fprintf(out, "    goto block_%d;\n", block->terminator->data.target_id);
    break;
  case TERM_COND_BRANCH:
    fprintf(out, "    if (");
    emit_c_expr(block->terminator->data.cond_branch.condition, out);
    fprintf(out, ") { goto block_%d; } else { goto block_%d; }\n",
            block->terminator->data.cond_branch.true_id,
            block->terminator->data.cond_branch.false_id);
    break;
  default:
    break;
  }
}

static void emit_c_subroutine(L1Subroutine *sub, FILE *out) {
  if (!sub->blocks)
    return;
  // Special-case: main with 0 params gets argc/argv injection
  int is_main = (strcmp(sub->name, "main") == 0 && sub->param_count == 0);
  if (is_main) {
    fprintf(out, "int main(int argc, char **argv) {\n");
    fprintf(out, "    native_set_args(argc, argv);\n");
  } else {
    emit_c_type(sub->ret_ty, out);
    fprintf(out, " %s(", sub->name);
    for (uint32_t i = 0; i < sub->param_count; i++) {
      emit_c_type(sub->param_tys[i], out);
      fprintf(out, " arg%d%s", i, (i == sub->param_count - 1) ? "" : ", ");
    }
    fprintf(out, ") {\n");
  }

  // Add native_set_args to forward declarations for main
  if (is_main) {
    // Pre-declare native_set_args in the forward declaration section
    // (handled by native_emit_module_to_file)
  }

  // Declare PHI variables at top
  L1PhiVar *pv = sub->phi_vars;
  while (pv) {
    fprintf(out, "    ");
    emit_c_type(pv->type, out);
    fprintf(out, " %s = 0;\n", pv->name);
    pv = pv->next;
  }

  // Emit blocks
  L1Block *block = sub->blocks;
  int first_block = 1;
  while (block) {
    fprintf(out, "\nblock_%d:\n", block->id);
    first_block = 0;
    emit_c_instructions(block, out);
    emit_c_block_terminator(block, out);
    block = block->next;
  }
  fprintf(out, "}\n\n");
}

// ============================================================================
// 10. Syntax Cursor FFI Functions
// ============================================================================

typedef struct {
  L1Token **tokens;
  uint32_t count;
  uint32_t index;
} L1Cursor;

static sexp sexp_syntax_group_cursor(sexp ctx, sexp self, sexp_sint_t n,
                                     sexp arg_group) {
  L1Token *group = (L1Token *)sexp_cpointer_value(arg_group);
  L1Cursor *cursor = malloc(sizeof(L1Cursor));
  cursor->tokens = group->data.group.children;
  cursor->count = group->data.group.count;
  cursor->index = 0;
  return sexp_make_cpointer(ctx, SEXP_CPOINTER, cursor, SEXP_FALSE, 0);
}

static sexp sexp_syntax_group_kind(sexp ctx, sexp self, sexp_sint_t n,
                                   sexp arg_group) {
  L1Token *group = (L1Token *)sexp_cpointer_value(arg_group);
  if (!group || group->kind != TOK_GROUP)
    return SEXP_FALSE;
  switch (group->data.group.kind) {
  case GRP_PAREN:
    return sexp_intern(ctx, "|paren|", -1);
  case GRP_BRACKET:
    return sexp_intern(ctx, "|bracket|", -1);
  case GRP_BRACE:
    return sexp_intern(ctx, "|brace|", -1);
  case GRP_ROOT:
    return sexp_intern(ctx, "|root|", -1);
  }
  return SEXP_FALSE;
}

static int cursor_current_is_raw(L1Cursor *c, RawTokenKind kind,
                                 const char *val) {
  if (c->index >= c->count)
    return 0;
  L1Token *t = c->tokens[c->index];
  if (t->kind != TOK_RAW)
    return 0;
  if (t->data.raw.kind != kind)
    return 0;
  if (val && t->data.raw.val && strcmp(t->data.raw.val, val) != 0)
    return 0;
  return 1;
}

static sexp cursor_match_ident(L1Cursor *c, sexp ctx, const char *filter) {
  if (c->index >= c->count)
    return SEXP_FALSE;
  L1Token *t = c->tokens[c->index];
  if (t->kind != TOK_RAW || t->data.raw.kind != T_IDENT)
    return SEXP_FALSE;
  const char *ident = t->data.raw.val ? t->data.raw.val : "";
  // If filter is provided, check that the ident matches
  if (filter && strcmp(ident, filter) != 0)
    return SEXP_FALSE;
  c->index++;
  return sexp_intern(ctx, ident, -1);
}

// match-ident-raw! — 带可选过滤参数 (arity 2)
static sexp sexp_cursor_match_ident(sexp ctx, sexp self, sexp_sint_t n,
                                    sexp arg_cursor, sexp arg_filter) {
  L1Cursor *c = (L1Cursor *)sexp_cpointer_value(arg_cursor);
  const char *filter = NULL;
  if (sexp_symbolp(arg_filter)) {
    filter = sexp_to_c_string(ctx, arg_filter);
  }
  return cursor_match_ident(c, ctx, filter);
}

// expect-ident-raw! — 无过滤参数 (arity 1)
static sexp sexp_cursor_expect_ident(sexp ctx, sexp self, sexp_sint_t n,
                                     sexp arg_cursor) {
  L1Cursor *c = (L1Cursor *)sexp_cpointer_value(arg_cursor);
  return cursor_match_ident(c, ctx, NULL);
}

static sexp sexp_cursor_match_punct(sexp ctx, sexp self, sexp_sint_t n,
                                    sexp arg_cursor, sexp arg_val) {
  L1Cursor *c = (L1Cursor *)sexp_cpointer_value(arg_cursor);
  const char *val = sexp_to_c_string(ctx, arg_val);
  if (cursor_current_is_raw(c, T_PUNCT, val)) {
    c->index++;
    return SEXP_TRUE;
  }
  return SEXP_FALSE;
}

static sexp sexp_cursor_match_string(sexp ctx, sexp self, sexp_sint_t n,
                                     sexp arg_cursor) {
  L1Cursor *c = (L1Cursor *)sexp_cpointer_value(arg_cursor);
  if (c->index >= c->count)
    return SEXP_FALSE;
  L1Token *t = c->tokens[c->index];
  if (t->kind != TOK_RAW || t->data.raw.kind != T_STRING)
    return SEXP_FALSE;
  c->index++;
  return sexp_intern(ctx, t->data.raw.val ? t->data.raw.val : "", -1);
}

static sexp sexp_cursor_match_number(sexp ctx, sexp self, sexp_sint_t n,
                                     sexp arg_cursor) {
  L1Cursor *c = (L1Cursor *)sexp_cpointer_value(arg_cursor);
  if (c->index >= c->count)
    return SEXP_FALSE;
  L1Token *t = c->tokens[c->index];
  if (t->kind != TOK_RAW || t->data.raw.kind != T_NUMBER)
    return SEXP_FALSE;
  c->index++;
  return sexp_make_fixnum((sexp_sint_t)t->data.raw.int_val);
}

static sexp sexp_cursor_match_group(sexp ctx, sexp self, sexp_sint_t n,
                                    sexp arg_cursor, sexp arg_kind) {
  L1Cursor *c = (L1Cursor *)sexp_cpointer_value(arg_cursor);
  if (c->index >= c->count)
    return SEXP_FALSE;
  L1Token *t = c->tokens[c->index];
  if (t->kind != TOK_GROUP)
    return SEXP_FALSE;

  const char *kind_name = sexp_to_c_string(ctx, arg_kind);
  L1GroupKind expected;
  if (strcmp(kind_name, "paren") == 0)
    expected = GRP_PAREN;
  else if (strcmp(kind_name, "bracket") == 0)
    expected = GRP_BRACKET;
  else if (strcmp(kind_name, "brace") == 0)
    expected = GRP_BRACE;
  else
    return SEXP_FALSE;

  if (t->data.group.kind != expected)
    return SEXP_FALSE;
  c->index++;
  return sexp_make_cpointer(ctx, SEXP_CPOINTER, t, SEXP_FALSE, 0);
}

static sexp sexp_cursor_eof_raw(sexp ctx, sexp self, sexp_sint_t n,
                                sexp arg_cursor) {
  L1Cursor *c = (L1Cursor *)sexp_cpointer_value(arg_cursor);
  return (c->index >= c->count) ? SEXP_TRUE : SEXP_FALSE;
}

static sexp sexp_cursor_get_index(sexp ctx, sexp self, sexp_sint_t n,
                                  sexp arg_cursor) {
  L1Cursor *c = (L1Cursor *)sexp_cpointer_value(arg_cursor);
  return sexp_make_fixnum(c->index);
}

static sexp sexp_cursor_set_index(sexp ctx, sexp self, sexp_sint_t n,
                                  sexp arg_cursor, sexp arg_idx) {
  L1Cursor *c = (L1Cursor *)sexp_cpointer_value(arg_cursor);
  uint32_t idx = sexp_unbox_fixnum(arg_idx);
  if (idx <= c->count)
    c->index = idx;
  return SEXP_VOID;
}

static sexp sexp_cursor_expect_eof_raw(sexp ctx, sexp self, sexp_sint_t n,
                                       sexp arg_cursor) {
  L1Cursor *c = (L1Cursor *)sexp_cpointer_value(arg_cursor);
  if (c->index >= c->count)
    return SEXP_TRUE;
  fprintf(stderr, "Expected EOF in cursor\n");
  exit(1);
}

static sexp sexp_cursor_expect_punct_raw(sexp ctx, sexp self, sexp_sint_t n,
                                         sexp arg_cursor, sexp arg_val) {
  L1Cursor *c = (L1Cursor *)sexp_cpointer_value(arg_cursor);
  const char *val = sexp_to_c_string(ctx, arg_val);
  if (cursor_current_is_raw(c, T_PUNCT, val)) {
    c->index++;
    return SEXP_TRUE;
  }
  fprintf(stderr, "Expected punct `%s` in cursor\n", val);
  exit(1);
}

static sexp sexp_cursor_expect_number_raw(sexp ctx, sexp self, sexp_sint_t n,
                                          sexp arg_cursor) {
  L1Cursor *c = (L1Cursor *)sexp_cpointer_value(arg_cursor);
  if (c->index >= c->count) {
    fprintf(stderr, "Expected number in cursor\n");
    exit(1);
  }
  L1Token *t = c->tokens[c->index];
  if (t->kind != TOK_RAW || t->data.raw.kind != T_NUMBER) {
    fprintf(stderr, "Expected number in cursor\n");
    exit(1);
  }
  c->index++;
  return sexp_make_fixnum((sexp_sint_t)t->data.raw.int_val);
}

static sexp sexp_cursor_expect_group_raw(sexp ctx, sexp self, sexp_sint_t n,
                                         sexp arg_cursor, sexp arg_kind) {
  L1Cursor *c = (L1Cursor *)sexp_cpointer_value(arg_cursor);
  const char *expected_kind = sexp_to_c_string(ctx, arg_kind);
  if (c->index >= c->count) {
    fprintf(stderr, "Expected group '%s' in cursor (EOF at idx=%u count=%u)\n",
            expected_kind, c->index, c->count);
    exit(1);
  }
  L1Token *t = c->tokens[c->index];
  if (t->kind != TOK_GROUP) {
    fprintf(stderr,
            "Expected group '%s' in cursor (not a group at idx=%u, kind=%d)\n",
            expected_kind, c->index, t->kind);
    exit(1);
  }

  const char *kind_name = expected_kind;
  L1GroupKind expected;
  if (strcmp(kind_name, "paren") == 0)
    expected = GRP_PAREN;
  else if (strcmp(kind_name, "bracket") == 0)
    expected = GRP_BRACKET;
  else if (strcmp(kind_name, "brace") == 0)
    expected = GRP_BRACE;
  else {
    fprintf(stderr, "Expected group: unknown kind '%s'\n", kind_name);
    exit(1);
  }

  if (t->data.group.kind != expected) {
    fprintf(stderr, "Expected group kind '%s' but got '%s'\n", kind_name,
            t->data.group.kind == GRP_PAREN     ? "paren"
            : t->data.group.kind == GRP_BRACKET ? "bracket"
            : t->data.group.kind == GRP_BRACE   ? "brace"
                                                : "root");
    exit(1);
  }
  c->index++;
  return sexp_make_cpointer(ctx, SEXP_CPOINTER, t, SEXP_FALSE, 0);
}

// ============================================================================
// 11. Scheme Initialization and Meta Source Loading
// ============================================================================

static void check_exception(sexp ctx, sexp res) {
  if (sexp_exceptionp(res)) {
    sexp_print_exception(ctx, res, sexp_current_error_port(ctx));
    fprintf(stderr, "\n");
    exit(1);
  }
}

static void native_eval_string(sexp ctx, sexp env, const char *code) {
  sexp res = sexp_eval_string(ctx, code, -1, env);
  check_exception(ctx, res);
}

static void native_load_file(sexp ctx, sexp env, const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) {
    fprintf(stderr, "Warning: cannot open file: %s\n", path);
    return;
  }
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *buf = malloc(len + 1);
  if (!buf) {
    fprintf(stderr, "OOM reading %s\n", path);
    exit(1);
  }
  fread(buf, 1, len, f);
  buf[len] = '\0';
  fclose(f);
  // Wrap in (begin ...) so ALL top-level expressions are evaluated.
  // sexp_eval_string only evaluates the first expression otherwise.
  int wrapped_len = len + 9; // "(begin " + content + ")" + NUL
  char *wrapped = malloc(wrapped_len);
  if (!wrapped) {
    fprintf(stderr, "OOM wrapping %s\n", path);
    exit(1);
  }
  sprintf(wrapped, "(begin %s)", buf);
  free(buf);
  sexp res = sexp_eval_string(ctx, wrapped, -1, env);
  check_exception(ctx, res);
  free(wrapped);
}

static void native_inject_all_polyfills(sexp ctx, sexp env) {
  // 1. Import R7RS base
  native_eval_string(ctx, env,
                     "(import (scheme base) (scheme cxr) (scheme load))");

  // 2. Basic polyfills
  native_eval_string(ctx, env,
                     "(begin (define unit #f) (define (meta-source x) #f))");

  // 3. define-pass macro system
  native_eval_string(ctx, env, "(define __lain-passes '())");
  native_eval_string(
      ctx, env,
      "(define (define-pass* stage kind body)"
      "  (set! __lain-passes"
      "    (cons (cons stage (cons kind (cons body '()))) __lain-passes)))");
  native_eval_string(
      ctx, env,
      "(define-syntax define-pass"
      "  (syntax-rules ()"
      "    ((_ (stage kind arg ...) body ...)"
      "     (define-pass* 'stage 'kind (lambda (arg ...) body ...)))))");

  // 4. optional.* polyfills
  native_eval_string(ctx, env,
                     "(begin (define (optional.none) #f)"
                     "  (define (optional.some v) v)"
                     "  (define (optional.value v) v)"
                     "  (define (optional.some? v) (if v #t #f))"
                     "  (define (optional.none? v) (not v)))");

  // 5. list.* polyfills
  native_eval_string(ctx, env,
                     "(begin (define (list.empty? lst) (null? lst))"
                     "  (define (list.first lst) (car lst))"
                     "  (define (list.rest lst) (cdr lst))"
                     "  (define (list.cons item lst) (cons item lst))"
                     "  (define (list.reverse lst) (reverse lst)))");

  // 6. symbol=? polyfill
  native_eval_string(ctx, env, "(define (symbol=? a b) (equal? a b))");

  // 7. record.* polyfills
  native_eval_string(ctx, env,
                     "(begin (define (record kind . fields) (cons kind fields))"
                     "  (define (record.field k v) (cons k v))"
                     "  (define (record.get payload name)"
                     "    (let loop ((fields (cdr payload)))"
                     "      (if (null? fields) #f"
                     "          (let ((field (car fields)))"
                     "            (if (equal? (car field) name)"
                     "                (cdr field)"
                     "                (loop (cdr fields))))))))");

  // 8. pre_declare_variables (Scheme-side)
  native_eval_string(ctx, env,
                     "(begin"
                     "  (define *syntax-temp-counter* 0)"
                     "  (define *effect-temp-counter* 0)"
                     "  (define (syntax.temp) (set! *syntax-temp-counter* (+ "
                     "*syntax-temp-counter* 1))"
                     "    (string->symbol (string-append \"__syntax_temp_\""
                     "      (number->string *syntax-temp-counter*))))"
                     "  (define (effects.temp) (set! *effect-temp-counter* (+ "
                     "*effect-temp-counter* 1))"
                     "    (string->symbol (string-append \"__effect_temp_\""
                     "      (number->string *effect-temp-counter*)))))");

  // 9. raw.* polyfills
  native_eval_string(
      ctx, env,
      "(begin (define (raw.node! kind payload) (vector 'raw-node kind payload))"
      "  (define (raw.kind node) (vector-ref node 1))"
      "  (define (raw.payload node) (vector-ref node 2)))");

  // 10. middle.* polyfills
  native_eval_string(ctx, env,
                     "(begin (define (middle.node! kind payload) (vector "
                     "'middle-node kind payload))"
                     "  (define (middle.kind node) (vector-ref node 1))"
                     "  (define (middle.payload node) (vector-ref node 2)))");

  // 11. decl.* polyfills
  native_eval_string(ctx, env,
                     "(begin (define *lain-declarations* '())"
                     "  (define (decl.define! kind name node)"
                     "    (set! *lain-declarations* (cons (list name kind node)"
                     "  *lain-declarations*)))"
                     "  (define (decl.name decl) (car decl))"
                     "  (define (decl.payload decl) (caddr decl)))");

  // 12. type.* wrappers
  native_eval_string(ctx, env,
                     "(define type.unit (lambda () (core.make-unit)))");
  native_eval_string(
      ctx, env, "(define type.bits (lambda (width) (core.make-bits width)))");
  native_eval_string(ctx, env,
                     "(define type.addr (lambda () (core.make-addr)))");
  native_eval_string(ctx, env,
                     "(define type.never (lambda () (core.make-unit)))");
  native_eval_string(ctx, env,
                     "(define type.registered (lambda (name . args)"
                     "  (type.registered-raw name)))");
  native_eval_string(ctx, env,
                     "(define type.raw-ptr (lambda args (core.make-addr)))");
  native_eval_string(ctx, env, "(define type.eq? equal?)");
  native_eval_string(ctx, env,
                     "(define (type.unit? ty) (core.type-is-void! ty))");
  native_eval_string(ctx, env, "(define type.float? (lambda (ty) #f))");
  native_eval_string(
      ctx, env, "(define type.unsupported (lambda args (core.make-bits 32)))");
  native_eval_string(ctx, env,
                     "(define type.fn (lambda (params ret) (core.make-addr)))");
  native_eval_string(ctx, env,
                     "(define type.array (lambda (ty size) (core.make-addr)))");
  native_eval_string(
      ctx, env, "(define type.product (lambda (types) (core.make-bits 32)))");
  native_eval_string(
      ctx, env, "(define type.product-field-types (lambda (product) (list)))");
  native_eval_string(ctx, env, "(define string-byte-len string-length)");

  // 13. core.* Scheme polyfills (non-FFI)
  native_eval_string(
      ctx, env,
      "(begin (define (core.invoke-intrinsic! name) #f)"
      "  (define (core.empty-effects) 'empty-effects)"
      "  (define (registry.operator-registered? op) #t)"
      "  (define (u64.add1 n) (+ n 1))"
      "  (define (core.effect! name args) (list 'effect name args))"
      "  (define (core.effect-row! ids) 'effect-row)"
      "  (define (core.bind-generic! name) (if #f #f))"
      "  (define (core.clear-generics!) (if #f #f))"
      "  (define (core.function-effects fn) (quote empty-effects))"
      "  (define (core.begin-function-with-effects! name params effects ret)"
      "    (core.begin-function! name params ret))"
      "  (define (core.const-zero! block ty)"
      "    (core.const-bits! block ty 0))"
      "  (define (diag.raise! . args) #f)"
      "  (define (core.declare-enum-name! name variants) unit)))");

  // 14. effects.* polyfills
  native_eval_string(ctx, env,
                     "(begin (define (effects.find-throws-arg eff) #f)"
                     "  (define effects.row-effect-count (lambda (r) 0))"
                     "  (define effects.row-effect-at (lambda (r i) '()))"
                     "  (define effects.effect-name (lambda (e) 'unknown))"
                     "  (define effects.effect-args (lambda (e) '())))");

  // 14.5 Host function polyfills (called at top-level by various meta sources)
  // All are no-ops in the bootstrap — type registration is handled by the
  // simplified L1 type system.
  native_eval_string(
      ctx, env,
      "(begin"
      " (define (register-type-constructor! name arity repr) unit)"
      " (define (register-string-literal-type! name) unit)"
      " (define (register-memory-ordering-type! name) unit)"
      " (define (register-array-type! name) unit)"
      " (define (register-condition-type! name) unit)"
      " (define (register-raw-pointer-type! mut name) unit)"
      " (define (register-raw-pointer-index-type! name) unit)"
      " (define (register-constraint! name pred) unit)"
      " (define (register-interface-rule! name rule) unit)"
      " (define (register-operator! symbol name) unit)"
      " (define (register-effect-constructor! name arity repr) unit)"
      " (define (register-expression-macro! name handler) unit)"
      " (define (register-raw-pointer-effect! kind name) unit))");

  // 15. Syntax cursor high-level wrappers
  native_eval_string(
      ctx, env,
      "(define (syntax.cursor-match-ident! cursor . opt-symbol)"
      "  (let ((res (if (null? opt-symbol)"
      "      (syntax.cursor-match-ident-raw! cursor #f)"
      "      (syntax.cursor-match-ident-raw! cursor (car opt-symbol)))))"
      "    (if res (optional.some res) (optional.none))))");
  native_eval_string(ctx, env,
                     "(define (syntax.cursor-expect-ident! cursor)"
                     "  (syntax.cursor-expect-ident-raw! cursor))");
  native_eval_string(
      ctx, env,
      "(define (syntax.cursor-match-punct! cursor symbol)"
      "  (let ((res (syntax.cursor-match-punct-raw! cursor symbol)))"
      "    (if res (optional.some res) (optional.none))))");
  native_eval_string(ctx, env,
                     "(define (syntax.cursor-expect-punct! cursor symbol)"
                     "  (syntax.cursor-expect-punct-raw! cursor symbol))");
  native_eval_string(ctx, env,
                     "(define (syntax.cursor-match-string! cursor)"
                     "  (let ((res (syntax.cursor-match-string-raw! cursor)))"
                     "    (if res (optional.some res) (optional.none))))");
  native_eval_string(ctx, env,
                     "(define (syntax.cursor-match-number! cursor)"
                     "  (let ((res (syntax.cursor-match-number-raw! cursor)))"
                     "    (if res (optional.some res) (optional.none))))");
  native_eval_string(ctx, env,
                     "(define (syntax.cursor-expect-number! cursor)"
                     "  (syntax.cursor-expect-number-raw! cursor))");
  native_eval_string(
      ctx, env,
      "(define (syntax.cursor-match-group! cursor kind)"
      "  (let ((res (syntax.cursor-match-group-raw! cursor kind)))"
      "    (if res (optional.some res) (optional.none))))");
  native_eval_string(ctx, env,
                     "(define (syntax.cursor-expect-group! cursor kind)"
                     "  (syntax.cursor-expect-group-raw! cursor kind))");
  native_eval_string(ctx, env,
                     "(define (syntax.cursor-expect-eof! cursor)"
                     "  (syntax.cursor-expect-eof-raw! cursor))");
  native_eval_string(ctx, env,
                     "(define (syntax.cursor-eof? cursor)"
                     "  (eq? (syntax.cursor-eof-raw? cursor) #t))");

  // 16. Pre-declare variables that meta sources define via set!
  // Chibi-Scheme requires the variable to exist before set! can mutate it.
  // This covers all (set! ...) patterns from:
  //   syntax/common.scm, core/lower.scm, core/call.scm, core/effects.scm
  native_eval_string(ctx, env,
                     "(begin"
                     " (define syntax.parse-path-tail #f)"
                     " (define syntax.parse-path #f)"
                     " (define syntax.parse-attr-value #f)"
                     " (define syntax.parse-attr-arg #f)"
                     " (define syntax.parse-attr-args-tail #f)"
                     " (define syntax.parse-attr-args #f)"
                     " (define syntax.parse-attrs #f)"
                     " (define syntax.parse-generic-tail #f)"
                     " (define syntax.parse-generic-params #f)"
                     " (define syntax.parse-where-tail #f)"
                     " (define syntax.parse-where #f)"
                     " (define syntax.parse-type #f)"
                     " (define syntax.parse-type-args #f)"
                     " (define syntax.parse-type-list-tail #f)"
                     " (define syntax.parse-type-list #f)"
                     " (define syntax.parse-params-tail #f)"
                     " (define syntax.parse-params #f)"
                     " (define syntax.parse-empty-params #f)"
                     " (define syntax.parse-effect-tail #f)"
                     " (define syntax.parse-effect-name #f)"
                     " (define syntax.parse-effect-set #f)"
                     " (define syntax.parse-optional-effects #f)"
                     " (define syntax.parse-expr-args-tail #f)"
                     " (define syntax.parse-expr-args #f)"

                     " (define syntax.parse-mul-op #f)"
                     " (define syntax.parse-add-op #f)"
                     " (define syntax.parse-compare-op #f)"
                     " (define syntax.parse-mul-tail #f)"
                     " (define syntax.parse-add-tail #f)"
                     " (define syntax.parse-compare-tail #f)"
                     " (define syntax.parse-unary-expr #f)"
                     " (define syntax.parse-mul-expr #f)"
                     " (define syntax.parse-add-expr #f)"
                     " (define syntax.parse-compare-expr #f)"
                     " (define syntax.parse-call-or-path #f)"
                     " (define syntax.parse-postfix-tail #f)"
                     " (define syntax.parse-inline-handler-operation #f)"
                     " (define syntax.parse-inline-handler-operations #f)"
                     " (define syntax.parse-builtin-expr-after-at #f)"
                     " (define syntax.parse-if-expr-after-if #f)"
                     " (define syntax.parse-expr-atom #f)"
                     " (define syntax.parse-expr #f)"
                     " (define syntax.parse-let-stmt #f)"
                     " (define syntax.parse-block-items #f)"
                     " (define syntax.parse-block #f)"
                     " (define syntax.parse-optional-fn-body #f)"
                     " (define core.lower-type #f)"
                     " (define core.lower-types #f)"
                     " (define core.lower-param-types #f)"
                     " (define core.bind-params #f)"
                     " (define core.lower-expr #f)"
                     " (define core.infer-expr-type #f)"
                     " (define core.lower-stmt #f)"
                     " (define core.lower-stmts #f)"
                     " (define core.lower-if-non-tail-core #f)"
                     " (define core.lower-if-branch-body #f)"
                     " (define effects.handler-entry-type #f)"
                     " (define effects.allocate-entry! #f)"
                     " (define effects.push-handler! #f)"
                     " (define effects.pop-handler! #f))");
}

static void native_load_meta_sources(sexp ctx, sexp env) {
  const char *search_paths[] = {"std/meta/", "../std/meta/", "../../std/meta/",
                                NULL};

  const char *meta_files[] = {"core/list.scm",
                              "core/record.scm",
                              "core/types.scm",
                              "core/literals.scm",
                              "core/call.scm",
                              "core/lower.scm",
                              "lang/fn.scm",
                              "syntax/common.scm",
                              "lang/struct.scm",
                              "lang/enum.scm",
                              "lang/effect.scm",
                              "lang/interface.scm",
                              "lang/impl.scm",
                              "lang/import.scm",
                              "lang/mod.scm",
                              "middle/common.scm",
                              "effects/throws.scm",
                              "effects/suspend.scm",
                              "effects/spawn.scm",
                              "effects/base.scm",
                              "operators/integer.scm",
                              "operators/question.scm",
                              NULL};

  for (int si = 0; search_paths[si]; si++) {
    int found = 0;
    for (int fi = 0; meta_files[fi]; fi++) {
      char path[512];
      snprintf(path, sizeof(path), "%s%s", search_paths[si], meta_files[fi]);
      FILE *test = fopen(path, "r");
      if (test) {
        fclose(test);
        native_load_file(ctx, env, path);
        found = 1;
      }
    }
    if (found)
      return;
  }
  fprintf(stderr, "Warning: could not find meta sources\n");
}

// ============================================================================
// 12. Top-Level API Functions (called from Lain via @foreign)
// ============================================================================

// File buffer for read_file result
static uint8_t *g_file_buffer = NULL;
static uint32_t g_file_len = 0;

const uint8_t *native_read_file(const char *path) {
  if (g_file_buffer) {
    free(g_file_buffer);
    g_file_buffer = NULL;
  }
  FILE *f = fopen(path, "rb");
  if (!f) {
    g_file_len = 0;
    return NULL;
  }
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  g_file_buffer = malloc(len + 1);
  fread(g_file_buffer, 1, len, f);
  g_file_buffer[len] = '\0';
  fclose(f);
  g_file_len = (uint32_t)len;
  return g_file_buffer;
}

uint32_t native_file_len(void) { return g_file_len; }

// Environment variable access
const char *native_getenv(const char *name) { return getenv(name); }

// Lex + Group: takes source pointer and length, returns L1Token* (root group)
void *native_lex_and_group(const uint8_t *src, uint32_t len) {
  RawToken *raw = malloc(sizeof(RawToken) * 4096);
  uint32_t total = 0;
  uint32_t pos = 0;

  while (1) {
    RawToken t = lex_one_token_from_mem(src, &pos, len);
    if (t.kind == T_EOF)
      break;
    raw[total++] = t;
  }

  uint32_t idx = 0;
  return group_tokens_recursive(raw, total, &idx, GRP_ROOT);
}

// Initialize Scheme environment, register all FFI functions, load meta passes
void *native_init_scheme(void) {
  // Set CHIBI_MODULE_PATH so (import (scheme ...)) can find libraries
  {
    char cwd[2048];
    if (getcwd(cwd, sizeof(cwd))) {
      char project_root[2048];
      strcpy(project_root, cwd);
      // Strip /compiler or /bootstrap suffix to find project root
      char *p;
      if ((p = strstr(project_root, "/compiler")))
        *p = '\0';
      else if ((p = strstr(project_root, "/bootstrap")))
        *p = '\0';
      char module_path[2048];
      snprintf(module_path, sizeof(module_path),
               "%s/bootstrap/chibi-scheme/lib", project_root);
      setenv("CHIBI_MODULE_PATH", module_path, 1);
    }
  }

  sexp ctx = sexp_make_eval_context(NULL, NULL, NULL, 0, 0);
  fprintf(stderr, "[init] 1: ctx created\n");
  sexp_load_standard_env(ctx, NULL, SEXP_SEVEN);
  fprintf(stderr, "[init] 2: std env loaded\n");
  sexp env = sexp_context_env(ctx);

  // Inject all polyfills (must happen before meta source loading)
  native_inject_all_polyfills(ctx, env);
  fprintf(stderr, "[init] 3: polyfills injected\n");
  {
    sexp r = sexp_eval_string(ctx,
                              "(begin (define-pass (form-parser test-pass "
                              "form) unit) (null? __lain-passes))",
                              -1, env);
    if (r == SEXP_FALSE)
      fprintf(stderr, "[diag] define-pass works!\n");
    else if (r == SEXP_TRUE)
      fprintf(stderr, "[diag] define-pass did NOT populate __lain-passes\n");
    else {
      fprintf(stderr, "[diag] define-pass test exception: ");
      sexp_print_exception(ctx, r, sexp_current_error_port(ctx));
      fprintf(stderr, "\n");
    }
  }

  // Register core FFI functions
#define REG(name, args, fn) sexp_define_foreign(ctx, env, name, args, fn)
  REG("core.make-bits", 1, sexp_core_make_bits);
  REG("core.make-addr", 0, sexp_core_make_addr);
  REG("core.make-unit", 0, sexp_core_make_unit);
  REG("type.registered-raw", 1, sexp_type_registered);
  REG("core.make-set", 2, sexp_core_make_set);
  REG("core.make-proc", 4, sexp_core_make_proc);
  REG("core.const-bits!", 3, sexp_core_const_bits);
  REG("core.const-string!", 3, sexp_core_const_string);
  REG("core.load!", 3, sexp_core_load);
  REG("core.store!", 3, sexp_core_store);
  REG("core.begin-function!", 3, sexp_core_begin_function);
  REG("core.function-by-name", 1, sexp_core_function_by_name);
  REG("core.append-block!", 1, sexp_core_append_block);
  REG("core.return-value!", 2, sexp_core_return_value);
  REG("core.return-none!", 1, sexp_core_return_none);
  REG("core.function-return-type", 1, sexp_core_function_return_type);
  REG("core.function-param-types", 1, sexp_core_function_param_types);
  REG("core.call!", 3, sexp_core_call);
  REG("core.primitive!", 4, sexp_core_primitive);
  REG("core.local-alloc!", 3, sexp_core_local_alloc);
  REG("core.param", 2, sexp_core_param);
  REG("core.block-function", 1, sexp_core_block_function);
  REG("core.branch!", 2, sexp_core_branch);
  REG("core.cond-branch!", 4, sexp_core_cond_branch);
  REG("core.phi!", 6, sexp_core_phi);
  REG("core.type-is-void!", 1, sexp_core_type_is_void);
  REG("core.declare-struct-name!", 1, sexp_core_declare_struct_name);
  REG("core.declare-struct!", 2, sexp_core_declare_struct);
  REG("core.struct-type", 1, sexp_core_struct_type);
  REG("core.struct-field-type", 2, sexp_core_struct_field_type);
  REG("core.struct-field-type-from-type", 2,
      sexp_core_struct_field_type_from_type);
  REG("core.struct-field-index", 2, sexp_core_struct_field_index);
  REG("core.struct-field-index-from-type", 2,
      sexp_core_struct_field_index_from_type);
  REG("core.aggregate!", 3, sexp_core_aggregate);
  REG("core.field!", 5, sexp_core_field);
  REG("core.call-indirect!", 4, sexp_core_call_indirect);
  REG("core.declare-extern-function!", 4, sexp_core_declare_extern_function);
  REG("core.call-expr!", 3, sexp_core_call_expr);
  REG("core.assign-temp!", 2, sexp_core_assign_temp);

  // Register syntax cursor FFI functions
  REG("syntax.group-cursor", 1, sexp_syntax_group_cursor);
  REG("syntax.group-kind", 1, sexp_syntax_group_kind);
  REG("syntax.cursor-eof-raw?", 1, sexp_cursor_eof_raw);
  REG("syntax.cursor-expect-eof-raw!", 1, sexp_cursor_expect_eof_raw);
  REG("syntax.cursor-match-punct-raw!", 2, sexp_cursor_match_punct);
  REG("syntax.cursor-expect-punct-raw!", 2, sexp_cursor_expect_punct_raw);
  REG("syntax.cursor-match-ident-raw!", 2, sexp_cursor_match_ident);
  REG("syntax.cursor-expect-ident-raw!", 1, sexp_cursor_expect_ident);
  REG("syntax.cursor-match-string-raw!", 1, sexp_cursor_match_string);
  REG("syntax.cursor-match-number-raw!", 1, sexp_cursor_match_number);
  REG("syntax.cursor-expect-number-raw!", 1, sexp_cursor_expect_number_raw);
  REG("syntax.cursor-match-group-raw!", 2, sexp_cursor_match_group);
  REG("syntax.cursor-expect-group-raw!", 2, sexp_cursor_expect_group_raw);
  REG("syntax.cursor-get-index", 1, sexp_cursor_get_index);
  REG("syntax.cursor-set-index!", 2, sexp_cursor_set_index);
#undef REG
  fprintf(stderr, "[init] 4: FFI registered\n");

  // Load meta sources (define-pass registrations happen here)
  native_load_meta_sources(ctx, env);
  fprintf(stderr, "[init] 5: meta sources loaded\n");

  // Load the bootstrap driver (pipeline orchestration)
  const char *driver_path = "bootstrap/bootstrap_driver.scm";
  FILE *f = fopen(driver_path, "r");
  if (!f) {
    driver_path = "../bootstrap/bootstrap_driver.scm";
    f = fopen(driver_path, "r");
  }
  if (f) {
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(len + 1);
    fread(buf, 1, len, f);
    buf[len] = '\0';
    fclose(f);

    // MINIMAL TEST: eval a simple define and verify it sticks
    {
      sexp tr = sexp_eval_string(ctx, "(define (test-hello) 42)", -1, env);
      check_exception(ctx, tr);
      sexp ts = sexp_intern(ctx, "test-hello", -1);
      sexp tv = sexp_env_ref(ctx, env, ts, SEXP_FALSE);
      fprintf(stderr, "[init] 5b: test-hello after eval_string %s\n",
              tv == SEXP_FALSE ? "MISSING" : "FOUND");
    }

    // Load entire driver wrapped in (begin ...)
    {
      const char *driver_wrapped = "(begin\n\
;; ===========================================================================\n\
;; bootstrap_driver.scm — Lain Bootstrap 编译管线驱动器 (100% Scheme)\n\
;; ===========================================================================\n\
\n\
\n\
(define (syntax.form-cursor form)\n\
  (syntax.group-cursor form))\n\
\n\
(define (cfg.target-os) '|linux|)\n\
\n\
(define (pipeline.rule stage kind)\n\
  (let loop ((passes __lain-passes))\n\
    (if (null? passes)\n\
        (lambda args unit)  ;; 默认无操作 — 未注册的 pass 静默跳过\n\
        (let* ((entry (car passes))\n\
               (e-stage (car entry))\n\
               (e-kind (car (cdr entry)))\n\
               (e-body (car (cdr (cdr entry)))))\n\
          (if (and (equal? e-stage stage) (equal? e-kind kind))\n\
              e-body\n\
              (loop (cdr passes)))))))\n\
\n\
;; ---------------------------------------------------------------------------\n\
;; 2. 驱动管线\n\
;; ---------------------------------------------------------------------------\n\
\n\
;; Helper: 尝试查找 form-parser，找不到返回 #f\n\
(define (driver.lookup-form-parser kind)\n\
  (let loop ((passes __lain-passes))\n\
    (if (null? passes)\n\
        #f\n\
        (let* ((entry (car passes))\n\
               (e-stage (car entry))\n\
               (e-kind (car (cdr entry))))\n\
          (if (and (equal? e-stage 'form-parser) (equal? e-kind kind))\n\
              (car (cdr (cdr entry)))\n\
              (loop (cdr passes)))))))\n\
\n\
;; Phase 1: Peek root group 的第一个标识符，然后分发 form-parser\n\
;; 关键：form-parser 接收原始的 root group（不是 cursor！）\n\
;; form-parser 内部会调用 syntax.form-cursor 创建新的 cursor 从零开始解析\n\
(define (driver.parse-and-declare root-group)\n\
  (set! *lain-declarations* (list))\n\
  (let* ((peek-cursor (syntax.group-cursor root-group))\n\
         (head (syntax.cursor-match-ident! peek-cursor)))\n\
    (if (optional.none? head)\n\
        unit\n\
        (let* ((kind (optional.value head))\n\
               (parser (driver.lookup-form-parser kind)))\n\
          (if parser\n\
              (begin\n\
                (parser root-group)\n\
                (if (null? *lain-declarations*)\n\
                    (error \"form-parser produced no declarations\")\n\
                    unit))\n\
              (driver.parse-as-implicit-main root-group))))))\n\
\n\
;; Helper: 当 root group 不是已知 form 时，当做隐式 main 函数体处理\n\
(define (driver.parse-as-implicit-main root-group)\n\
  (let* ((block (syntax.parse-block root-group))\n\
         (sig (raw.node! '|fn.sig|\n\
                (record '|fn.sig|\n\
                  (record.field '|attrs| (list))\n\
                  (record.field '|generics| (list))\n\
                  (record.field '|params| (list))\n\
                  (record.field '|return|\n\
                    (raw.node! '|ty.path|\n\
                      (record '|ty.path|\n\
                        (record.field '|name| '|i32|))))\n\
                  (record.field '|where| #f)\n\
                  (record.field '|effects| #f)\n\
                  (record.field '|body| (optional.some block))))))\n\
    (decl.define! '|fn| '|main| sig)))\n\
\n\
;; Phase 2: 遍历 *lain-declarations*，对每条声明调用 raw-normalizer。\n\
(define (driver.normalize-decls)\n\
  (let loop ((decls *lain-declarations*) (acc (list)))\n\
    (if (null? decls)\n\
        (list.reverse acc)\n\
        (let* ((decl (car decls))\n\
               (kind (list-ref decl 1))\n\
               (normalizer (pipeline.rule 'raw-normalizer kind))\n\
               (middle-item (normalizer decl)))\n\
          (loop (cdr decls) (list.cons middle-item acc))))))\n\
\n\
;; Phase 3: 对每个 middle item 调用 core-declarer 注册函数签名。\n\
(define (driver.declare-core middle-items)\n\
  (for-each\n\
    (lambda (item)\n\
      (let* ((kind (middle.kind item))\n\
             (declarer (pipeline.rule 'core-declarer kind)))\n\
        (declarer item)))\n\
    middle-items))\n\
\n\
;; Phase 4: 对每个 middle item 调用 core-lowerer 生成 L1 指令。\n\
(define (driver.lower-core middle-items)\n\
  (for-each\n\
    (lambda (item)\n\
      (let* ((kind (middle.kind item))\n\
             (lowerer (pipeline.rule 'core-lowerer kind)))\n\
        (lowerer item)))\n\
    middle-items))\n\
\n\
;; ---------------------------------------------------------------------------\n\
;; 3. 主入口: compile-group-to-core\n\
;; ---------------------------------------------------------------------------\n\
\n\
(define (compile-group-to-core root-group)\n\
  (driver.parse-and-declare root-group)\n\
  (let* ((middle-items (driver.normalize-decls)))\n\
    (driver.declare-core middle-items)\n\
    (driver.lower-core middle-items))\n\
  0)\n\
\n\
)";
      fprintf(stderr,
              "[init] 6-drv: evaluating driver with begin wrapper...\n");
      sexp res = sexp_eval_string(ctx, driver_wrapped, -1, env);
      if (sexp_exceptionp(res)) {
        fprintf(stderr, "[init] 6-drv ERROR: ");
        sexp_print_exception(ctx, res, sexp_current_error_port(ctx));
        fprintf(stderr, "\n");
      } else {
        fprintf(stderr, "[init] 6-drv OK\n");
      }
    }

    free(buf);
    // Verify driver functions are defined immediately
    {
      sexp sym = sexp_intern(ctx, "compile-group-to-core", -1);
      sexp val = sexp_env_ref(ctx, env, sym, SEXP_FALSE);
      fprintf(stderr, "[init] 6a: compile-group-to-core %s\n",
              val == SEXP_FALSE      ? "MISSING"
              : sexp_procedurep(val) ? "is proc"
                                     : "is defined but not proc");
    }
  } else {
    fprintf(stderr, "Error: cannot find bootstrap_driver.scm\n");
    exit(1);
  }

  return ctx;
}

// Run the Scheme pipeline on a token tree
int32_t native_run_pipeline(void *ctx_ptr, void *root_group) {
  sexp ctx = (sexp)ctx_ptr;
  g_subroutines_head = NULL;

  sexp env = sexp_context_env(ctx);

  // Clear previous declarations
  sexp_eval_string(ctx, "(set! *lain-declarations* (list))", -1, env);

  // Split root group into individual forms
  uint32_t form_count = 0;
  L1Token **form_groups = split_root_group((L1Token *)root_group, &form_count);

  sexp compile_sym = sexp_intern(ctx, "compile-group-to-core", -1);
  sexp proc = sexp_env_ref(ctx, env, compile_sym, SEXP_FALSE);

  for (uint32_t fi = 0; fi < form_count; fi++) {
    sexp group_arg =
        sexp_make_cpointer(ctx, SEXP_CPOINTER, form_groups[fi], SEXP_FALSE, 0);
    sexp result = sexp_apply(ctx, proc, sexp_cons(ctx, group_arg, SEXP_NULL));

    if (sexp_exceptionp(result)) {
      fprintf(stderr, "[pipeline ERROR] ");
      sexp_print_exception(ctx, result, sexp_current_error_port(ctx));
      fprintf(stderr, "\n");
      return 1;
    }
  }

  return 0;
}

// Emit all subroutines to a file
void native_emit_module_to_file(void *subs_ptr, const char *output_path) {
  FILE *out = fopen(output_path, "w");
  if (!out) {
    fprintf(stderr, "Error: cannot open output file: %s\n", output_path);
    return;
  }

  fprintf(out, "#include <stdint.h>\n");
  fprintf(out, "#include <string.h>\n");
  fprintf(out, "#include <alloca.h>\n\n");

  // Hardcoded native runtime forward declarations
  // (needed because the bootstrap pipeline skips @foreign forms)
  fprintf(out, "// Native runtime forward declarations\n");
  fprintf(out, "void native_set_args(int argc, char **argv);\n");
  fprintf(out, "int32_t native_get_arg_count(void);\n");
  fprintf(out, "const char *native_get_arg(int32_t idx);\n");
  fprintf(out, "const uint8_t *native_read_file(const char *path);\n");
  fprintf(out, "uint32_t native_file_len(void);\n");
  fprintf(out,
          "void *native_lex_and_group(const uint8_t *src, uint32_t len);\n");
  fprintf(out, "void *native_init_scheme(void);\n");
  fprintf(out, "int32_t native_run_pipeline(void *ctx, void *root_group);\n");
  fprintf(out, "void native_emit_module_to_file(void *subs, const char "
               "*output_path);\n");
  fprintf(out, "void *native_get_subroutines(void);\n\n");

  // Emit forward declarations for all subroutines
  // Skip those already declared in the native runtime section
  static const char *native_funcs[] = {"native_set_args",
                                       "native_get_arg_count",
                                       "native_get_arg",
                                       "native_read_file",
                                       "native_file_len",
                                       "native_lex_and_group",
                                       "native_init_scheme",
                                       "native_run_pipeline",
                                       "native_emit_module_to_file",
                                       "native_get_subroutines",
                                       NULL};
  L1Subroutine *sub = g_subroutines_head;
  while (sub) {
    if (sub->blocks || sub->is_extern) {
      // Skip if already declared as native runtime function
      int is_native = 0;
      for (int i = 0; native_funcs[i]; i++) {
        if (strcmp(sub->name, native_funcs[i]) == 0 ||
            (sub->link_name && strcmp(sub->link_name, native_funcs[i]) == 0)) {
          is_native = 1;
          break;
        }
      }
      if (!is_native) {
        // Special-case: main with 0 params becomes int main(int, char**)
        if (strcmp(sub->name, "main") == 0 && sub->param_count == 0) {
          fprintf(out, "int main(int argc, char **argv);\n");
        } else {
          emit_c_type(sub->ret_ty, out);
          fprintf(out, " %s(", sub->link_name ? sub->link_name : sub->name);
          for (uint32_t i = 0; i < sub->param_count; i++) {
            if (sub->param_tys[i]) {
              emit_c_type(sub->param_tys[i], out);
            } else {
              fprintf(out, "void*");
            }
            fprintf(out, " arg%d%s", i,
                    (i == sub->param_count - 1) ? "" : ", ");
          }
          if (sub->param_count == 0)
            fprintf(out, "void");
          fprintf(out, ");\n");
        }
      }
    }
    sub = sub->next;
  }
  fprintf(out, "\n");

  // Emit each subroutine
  sub = g_subroutines_head;
  while (sub) {
    emit_c_subroutine(sub, out);
    sub = sub->next;
  }

  fclose(out);
}

// Get subroutine list head (for Lain code to inspect)
void *native_get_subroutines(void) { return g_subroutines_head; }

// ── Command-line argument access ─────────────────────────────────────────────

static int g_native_argc = 0;
static char **g_native_argv = NULL;

void native_set_args(int argc, char **argv) {
  g_native_argc = argc;
  g_native_argv = argv;
}

int32_t native_get_arg_count(void) { return g_native_argc; }

const char *native_get_arg(int32_t idx) {
  if (idx < 0 || idx >= g_native_argc)
    return NULL;
  return g_native_argv[idx];
}
