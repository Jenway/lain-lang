#include "lainir/artifact.h"
#include "lainir/parse.h"

#include <ctype.h>
#include <stdio.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
  TK_EOF,
  TK_IDENT,
  TK_NUMBER,
  TK_STRING,
  TK_PERCENT,
  TK_LPAREN,
  TK_RPAREN,
  TK_LBRACE,
  TK_RBRACE,
  TK_LBRACK,
  TK_RBRACK,
  TK_COMMA,
  TK_COLON,
  TK_EQ,
  TK_ARROW,
  TK_SEMICOLON,
  TK_HASH_UNIT,
  TK_HASH_NEVER,
  TK_KW_PROC,
  TK_KW_EXTERN,
  TK_KW_RETURN,
  TK_KW_STORE,
  TK_KW_CALL,
  TK_KW_IF,
  TK_KW_ELSE,
  TK_KW_LOOP,
  TK_KW_BREAK,
  TK_KW_CONTINUE,
  TK_KW_LET,
  TK_KW_ALLOCA,
  TK_KW_LEA,
  TK_KW_LOAD,
  TK_KW_ADD,
  TK_KW_SUB_OP,
  TK_KW_MUL_OP,
  TK_KW_EQ_OP,
  TK_KW_NE_OP,
  TK_KW_CALL_INDIRECT,
  TK_KW_EVAL,
  TK_KW_DATA,
  TK_KW_DATA_ADDR,
  TK_HASH_BITS,
  TK_HASH_FLOAT,
  TK_HASH_ADDR,
  TK_LANGLE,
  TK_RANGLE,
  TK_KW_SDIV,
  TK_KW_UDIV,
  TK_KW_SLT,
  TK_KW_SLE,
  TK_KW_SGT,
  TK_KW_SGE,
  TK_KW_ULT,
  TK_KW_ULE,
  TK_KW_UGT,
  TK_KW_UGE
  ,TK_KW_ZEXT
  ,TK_KW_SEXT
  ,TK_KW_TRUNC
  ,TK_KW_PROC_ADDR
  ,TK_KW_BITCAST
  ,TK_KW_FADD
  ,TK_KW_FSUB
  ,TK_KW_FMUL
  ,TK_KW_FDIV
  ,TK_KW_FEQ
  ,TK_KW_FLT
  ,TK_KW_INT2PTR
  ,TK_KW_PTR2INT
} TokenKind;

typedef struct {
  TokenKind kind;
  const char *text;
  int len;
  int line;
  int start;
  int end;
} Token;

typedef struct {
  const char *src;
  int pos;
  int line;
  int last_token_end;
  Token current;
  char **param_names;
  uint32_t param_count;
  uint32_t param_cap;
  jmp_buf failure;
  L1Diagnostic *diagnostic;
  L1Builder *builder;
} Parser;

static int is_ident_start(int c) {
  return isalpha(c) || c == '_' || c == '.';
}

static int is_ident_char(int c) {
  return isalnum(c) || c == '_' || c == '.' || c == '-' || c == '!';
}

static void parse_fail(Parser *p, const char *message) {
  if (p->diagnostic) {
    p->diagnostic->code = 1001;
    p->diagnostic->line = p->current.line;
    p->diagnostic->column = 0;
    snprintf(p->diagnostic->message, sizeof(p->diagnostic->message),
             "%s near `%.*s`", message,
             p->current.len > 0 ? p->current.len : 0,
             p->current.text ? p->current.text : "");
  }
  longjmp(p->failure, 1);
}

static char *token_string(Token token) {
  size_t len = (size_t)token.len;
  char *copy = malloc(len + 1);
  if (!copy)
    abort();
  memcpy(copy, token.text, len);
  copy[len] = '\0';
  return copy;
}

static int hex_digit_value(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

static char *token_string_literal(Token token, size_t *decoded_length) {
  size_t input = 0;
  size_t output = 0;
  char *copy = malloc((size_t)token.len + 1);
  if (!copy)
    abort();
  while (input < (size_t)token.len) {
    char current = token.text[input++];
    if (current == '\\' && input < (size_t)token.len) {
      char escaped = token.text[input++];
      switch (escaped) {
      case 'n': current = '\n'; break;
      case 'r': current = '\r'; break;
      case 't': current = '\t'; break;
      case '\\': current = '\\'; break;
      case '"': current = '"'; break;
      case 'x': {
        int high = input < (size_t)token.len
                       ? hex_digit_value(token.text[input]) : -1;
        int low = input + 1 < (size_t)token.len
                      ? hex_digit_value(token.text[input + 1]) : -1;
        if (high >= 0 && low >= 0) {
          current = (char)((high << 4) | low);
          input += 2;
        } else {
          copy[output++] = '\\';
          current = escaped;
        }
        break;
      }
      default:
        copy[output++] = '\\';
        current = escaped;
        break;
      }
    }
    copy[output++] = current;
  }
  copy[output] = '\0';
  if (decoded_length) *decoded_length = output;
  return copy;
}

static void parser_reset_subroutine_context(Parser *p) {
  for (uint32_t i = 0; i < p->param_count; i++)
    free(p->param_names[i]);
  free(p->param_names);
  p->param_names = NULL;
  p->param_count = 0;
  p->param_cap = 0;
}

static void parser_add_param_name(Parser *p, const char *name) {
  if (p->param_count == p->param_cap) {
    p->param_cap = p->param_cap ? p->param_cap * 2 : 4;
    p->param_names = realloc(p->param_names, sizeof(char *) * p->param_cap);
  }
  p->param_names[p->param_count++] = strdup(name);
}

static int parser_lookup_param_index(Parser *p, const char *name) {
  for (uint32_t i = 0; i < p->param_count; i++) {
    if (strcmp(p->param_names[i], name) == 0)
      return (int)i;
  }
  return -1;
}

static TokenKind hash_keyword_kind(const char *text, int len) {
  if (len == 4 && memcmp(text, "unit", 4) == 0) return TK_HASH_UNIT;
  if (len == 5 && memcmp(text, "never", 5) == 0) return TK_HASH_NEVER;
  if (len == 4 && memcmp(text, "bits", 4) == 0) return TK_HASH_BITS;
  if (len == 5 && memcmp(text, "float", 5) == 0) return TK_HASH_FLOAT;
  if (len == 4 && memcmp(text, "addr", 4) == 0) return TK_HASH_ADDR;
  if (len == 4 && memcmp(text, "proc", 4) == 0) return TK_KW_PROC;
  if (len == 6 && memcmp(text, "extern", 6) == 0) return TK_KW_EXTERN;
  if (len == 6 && memcmp(text, "return", 6) == 0) return TK_KW_RETURN;
  if (len == 5 && memcmp(text, "store", 5) == 0) return TK_KW_STORE;
  if (len == 4 && memcmp(text, "call", 4) == 0) return TK_KW_CALL;
  if (len == 2 && memcmp(text, "if", 2) == 0) return TK_KW_IF;
  if (len == 4 && memcmp(text, "else", 4) == 0) return TK_KW_ELSE;
  if (len == 4 && memcmp(text, "loop", 4) == 0) return TK_KW_LOOP;
  if (len == 5 && memcmp(text, "break", 5) == 0) return TK_KW_BREAK;
  if (len == 8 && memcmp(text, "continue", 8) == 0) return TK_KW_CONTINUE;
  if (len == 3 && memcmp(text, "let", 3) == 0) return TK_KW_LET;
  if (len == 6 && memcmp(text, "alloca", 6) == 0) return TK_KW_ALLOCA;
  if (len == 3 && memcmp(text, "lea", 3) == 0) return TK_KW_LEA;
  if (len == 4 && memcmp(text, "load", 4) == 0) return TK_KW_LOAD;
  if (len == 3 && memcmp(text, "add", 3) == 0) return TK_KW_ADD;
  if (len == 3 && memcmp(text, "sub", 3) == 0) return TK_KW_SUB_OP;
  if (len == 3 && memcmp(text, "mul", 3) == 0) return TK_KW_MUL_OP;
  if (len == 4 && memcmp(text, "sdiv", 4) == 0) return TK_KW_SDIV;
  if (len == 4 && memcmp(text, "udiv", 4) == 0) return TK_KW_UDIV;
  if (len == 2 && memcmp(text, "eq", 2) == 0) return TK_KW_EQ_OP;
  if (len == 2 && memcmp(text, "ne", 2) == 0) return TK_KW_NE_OP;
  if (len == 3 && memcmp(text, "slt", 3) == 0) return TK_KW_SLT;
  if (len == 3 && memcmp(text, "sle", 3) == 0) return TK_KW_SLE;
  if (len == 3 && memcmp(text, "sgt", 3) == 0) return TK_KW_SGT;
  if (len == 3 && memcmp(text, "sge", 3) == 0) return TK_KW_SGE;
  if (len == 3 && memcmp(text, "ult", 3) == 0) return TK_KW_ULT;
  if (len == 3 && memcmp(text, "ule", 3) == 0) return TK_KW_ULE;
  if (len == 3 && memcmp(text, "ugt", 3) == 0) return TK_KW_UGT;
  if (len == 3 && memcmp(text, "uge", 3) == 0) return TK_KW_UGE;
  if (len == 4 && memcmp(text, "zext", 4) == 0) return TK_KW_ZEXT;
  if (len == 4 && memcmp(text, "sext", 4) == 0) return TK_KW_SEXT;
  if (len == 5 && memcmp(text, "trunc", 5) == 0) return TK_KW_TRUNC;
  if (len == 9 && memcmp(text, "proc_addr", 9) == 0) return TK_KW_PROC_ADDR;
  if (len == 7 && memcmp(text, "bitcast", 7) == 0) return TK_KW_BITCAST;
  if (len == 4 && memcmp(text, "fadd", 4) == 0) return TK_KW_FADD;
  if (len == 4 && memcmp(text, "fsub", 4) == 0) return TK_KW_FSUB;
  if (len == 4 && memcmp(text, "fmul", 4) == 0) return TK_KW_FMUL;
  if (len == 4 && memcmp(text, "fdiv", 4) == 0) return TK_KW_FDIV;
  if (len == 3 && memcmp(text, "feq", 3) == 0) return TK_KW_FEQ;
  if (len == 3 && memcmp(text, "flt", 3) == 0) return TK_KW_FLT;
  if (len == 7 && memcmp(text, "int2ptr", 7) == 0) return TK_KW_INT2PTR;
  if (len == 7 && memcmp(text, "ptr2int", 7) == 0) return TK_KW_PTR2INT;
  if (len == 13 && memcmp(text, "call_indirect", 13) == 0) return TK_KW_CALL_INDIRECT;
  if (len == 4 && memcmp(text, "eval", 4) == 0) return TK_KW_EVAL;
  if (len == 4 && memcmp(text, "data", 4) == 0) return TK_KW_DATA;
  if (len == 9 && memcmp(text, "data_addr", 9) == 0) return TK_KW_DATA_ADDR;
  return TK_IDENT;
}

static void next_token(Parser *p) {
  int token_start;
  p->last_token_end = p->current.end;
  while (p->src[p->pos]) {
    char c = p->src[p->pos];
    if (c == ' ' || c == '\t' || c == '\r') {
      p->pos++;
      continue;
    }
    if (c == '\n') {
      p->line++;
      p->pos++;
      continue;
    }
    if (c == '/' && p->src[p->pos + 1] == '/') {
      while (p->src[p->pos] && p->src[p->pos] != '\n')
        p->pos++;
      continue;
    }
    break;
  }

  token_start = p->pos;

  p->current.text = p->src + p->pos;
  p->current.line = p->line;
  p->current.start = token_start;
  p->current.len = 1;

  if (!p->src[p->pos]) {
    p->current.kind = TK_EOF;
    p->current.len = 0;
    p->current.end = p->pos;
    return;
  }

  if (isdigit((unsigned char)p->src[p->pos])) {
    int start = p->pos;
    while (isdigit((unsigned char)p->src[p->pos]))
      p->pos++;
    p->current.kind = TK_NUMBER;
    p->current.text = p->src + start;
    p->current.len = p->pos - start;
    p->current.start = start;
    p->current.end = p->pos;
    return;
  }

  if (p->src[p->pos] == '"') {
    int start;
    p->pos++;
    start = p->pos;
    while (p->src[p->pos] && p->src[p->pos] != '"') {
      if (p->src[p->pos] == '\\' && p->src[p->pos + 1])
        p->pos++;
      p->pos++;
    }
    p->current.kind = TK_STRING;
    p->current.text = p->src + start;
    p->current.len = p->pos - start;
    if (p->src[p->pos] == '"')
      p->pos++;
    p->current.start = start - 1;
    p->current.end = p->pos;
    return;
  }

  if (p->src[p->pos] == '#') {
    int start;
    TokenKind kind;
    p->pos++;
    if (!is_ident_start((unsigned char)p->src[p->pos]))
      parse_fail(p, "expected identifier after `#`");
    start = p->pos;
    while (is_ident_char((unsigned char)p->src[p->pos]))
      p->pos++;
    p->current.text = p->src + start;
    p->current.len = p->pos - start;
    kind = hash_keyword_kind(p->current.text, p->current.len);
    if (kind == TK_IDENT)
      parse_fail(p, "unknown `#` keyword");
    p->current.kind = kind;
    p->current.start = token_start;
    p->current.end = p->pos;
    return;
  }

  if (is_ident_start((unsigned char)p->src[p->pos])) {
    int start = p->pos;
    while (is_ident_char((unsigned char)p->src[p->pos]))
      p->pos++;
    p->current.text = p->src + start;
    p->current.len = p->pos - start;
    if (p->current.len == 4 && memcmp(p->current.text, "else", 4) == 0)
      p->current.kind = TK_KW_ELSE;
    else
      p->current.kind = TK_IDENT;
    p->current.start = start;
    p->current.end = p->pos;
    return;
  }

  switch (p->src[p->pos++]) {
  case '%': p->current.kind = TK_PERCENT; break;
  case '(': p->current.kind = TK_LPAREN; break;
  case ')': p->current.kind = TK_RPAREN; break;
  case '{': p->current.kind = TK_LBRACE; break;
  case '}': p->current.kind = TK_RBRACE; break;
  case '[': p->current.kind = TK_LBRACK; break;
  case ']': p->current.kind = TK_RBRACK; break;
  case ',': p->current.kind = TK_COMMA; break;
  case ':': p->current.kind = TK_COLON; break;
  case '=': p->current.kind = TK_EQ; break;
  case ';': p->current.kind = TK_SEMICOLON; break;
  case '<': p->current.kind = TK_LANGLE; break;
  case '>': p->current.kind = TK_RANGLE; break;
  case '-':
    if (p->src[p->pos] == '>') {
      p->pos++;
      p->current.kind = TK_ARROW;
      p->current.len = 2;
      break;
    }
    parse_fail(p, "unexpected `-`");
    break;
  default:
    parse_fail(p, "unexpected character");
    break;
  }
  p->current.end = p->pos;
}

static Token expect(Parser *p, TokenKind kind) {
  Token token;
  if (p->current.kind != kind)
    parse_fail(p, "unexpected token");
  token = p->current;
  next_token(p);
  return token;
}

static L1Type *parse_type(Parser *p) {
  Token token = p->current;
  char *text;
  if (token.kind == TK_HASH_UNIT) {
    next_token(p);
    return lainir_new_type(p->builder, TY_UNIT, 0);
  }
  if (token.kind == TK_HASH_NEVER) {
    next_token(p);
    return lainir_new_type(p->builder, TY_NEVER, 0);
  }
  if (token.kind == TK_HASH_ADDR) {
    next_token(p);
    return lainir_new_type(p->builder, TY_ADDR, 64);
  }
  if (token.kind == TK_HASH_BITS || token.kind == TK_HASH_FLOAT) {
    L1TypeKind kind =
        token.kind == TK_HASH_BITS ? TY_BITS : TY_FLOATS;
    uint32_t width;
    next_token(p);
    expect(p, TK_LANGLE);
    token = expect(p, TK_NUMBER);
    text = token_string(token);
    width = (uint32_t)strtoul(text, NULL, 10);
    free(text);
    if (!width)
      parse_fail(p, "physical type width must be non-zero");
    expect(p, TK_RANGLE);
    return lainir_new_type(p->builder, kind, width);
  }
  if (token.kind != TK_IDENT)
    parse_fail(p, "expected type");

  text = token_string(token);
  next_token(p);

  free(text);
  parse_fail(p, "unknown type");
  return NULL;
}

static L1Expr *parse_expr(Parser *p);
static L1Block *parse_block_instructions(Parser *p);

static L1Expr *new_const_expr(Parser *p, int64_t value) {
  L1Expr *expr = lainir_new_expr(p->builder, EXPR_CONST);
  expr->data.const_val = value;
  return expr;
}

static L1Expr *new_var_expr(Parser *p, const char *name) {
  L1Expr *expr = lainir_new_expr(p->builder, EXPR_VAR);
  expr->data.var.name = lainir_builder_copy_string(p->builder, name);
  expr->data.var.ty = NULL;
  return expr;
}

static L1Expr *new_arg_expr(Parser *p, uint32_t index) {
  L1Expr *expr = lainir_new_expr(p->builder, EXPR_ARG);
  expr->data.arg.index = index;
  expr->data.arg.ty = NULL;
  return expr;
}

static L1Expr **parse_expr_list(Parser *p, uint32_t *count) {
  uint32_t cap = 4;
  uint32_t len = 0;
  L1Expr **items = NULL;

  if (p->current.kind == TK_RPAREN) {
    *count = 0;
    return NULL;
  }

  items = calloc(cap, sizeof(L1Expr *));
  while (1) {
    if (len == cap) {
      cap *= 2;
      items = realloc(items, sizeof(L1Expr *) * cap);
    }
    items[len++] = parse_expr(p);
    if (p->current.kind != TK_COMMA)
      break;
    next_token(p);
  }
  *count = len;
  return items;
}

static L1Expr *parse_percent_ref(Parser *p) {
  Token token;
  char *name;
  char *end;
  unsigned long index;
  L1Expr *expr;

  expect(p, TK_PERCENT);
  token = expect(p, TK_IDENT);
  name = token_string(token);

  {
    int param_index = parser_lookup_param_index(p, name);
    if (param_index >= 0) {
      free(name);
      return new_arg_expr(p, (uint32_t)param_index);
    }
  }

  if (strncmp(name, "arg", 3) == 0) {
    index = strtoul(name + 3, &end, 10);
    if (name[3] && *end == '\0') {
      free(name);
      return new_arg_expr(p, (uint32_t)index);
    }
  }

  expr = new_var_expr(p, name);
  free(name);
  return expr;
}

static L1Expr *parse_special_hash_call(Parser *p, TokenKind kind) {
  uint32_t count;
  L1Expr **args;
  L1Expr *expr;

  expect(p, TK_LPAREN);
  args = parse_expr_list(p, &count);
  expect(p, TK_RPAREN);

  if (kind == TK_KW_LOAD) {
    if (count != 1)
      parse_fail(p, "load expects one operand");
    expr = lainir_new_expr(p->builder, EXPR_LOAD);
    expr->data.load.addr = args[0];
    expr->data.load.ty = NULL;
    free(args);
    return expr;
  }
  if (kind == TK_KW_ADD || kind == TK_KW_SUB_OP ||
      kind == TK_KW_MUL_OP ||
      kind == TK_KW_EQ_OP || kind == TK_KW_NE_OP ||
      kind == TK_KW_SDIV || kind == TK_KW_UDIV ||
      kind == TK_KW_SLT || kind == TK_KW_SLE ||
      kind == TK_KW_SGT || kind == TK_KW_SGE ||
      kind == TK_KW_ULT || kind == TK_KW_ULE ||
      kind == TK_KW_UGT || kind == TK_KW_UGE ||
      kind == TK_KW_FADD || kind == TK_KW_FSUB ||
      kind == TK_KW_FMUL || kind == TK_KW_FDIV ||
      kind == TK_KW_FEQ || kind == TK_KW_FLT) {
    if (count != 2)
      parse_fail(p, "binary op expects two operands");
    L1ExprKind expr_kind = EXPR_ADD;
    if (kind == TK_KW_SUB_OP) expr_kind = EXPR_SUB;
    else if (kind == TK_KW_MUL_OP) expr_kind = EXPR_MUL;
    else if (kind == TK_KW_EQ_OP) expr_kind = EXPR_EQ;
    else if (kind == TK_KW_NE_OP) expr_kind = EXPR_NE;
    else if (kind == TK_KW_SDIV) expr_kind = EXPR_SDIV;
    else if (kind == TK_KW_UDIV) expr_kind = EXPR_UDIV;
    else if (kind == TK_KW_SLT) expr_kind = EXPR_SLT;
    else if (kind == TK_KW_SLE) expr_kind = EXPR_SLE;
    else if (kind == TK_KW_SGT) expr_kind = EXPR_SGT;
    else if (kind == TK_KW_SGE) expr_kind = EXPR_SGE;
    else if (kind == TK_KW_ULT) expr_kind = EXPR_ULT;
    else if (kind == TK_KW_ULE) expr_kind = EXPR_ULE;
    else if (kind == TK_KW_UGT) expr_kind = EXPR_UGT;
    else if (kind == TK_KW_UGE) expr_kind = EXPR_UGE;
    else if (kind == TK_KW_FADD) expr_kind = EXPR_FADD;
    else if (kind == TK_KW_FSUB) expr_kind = EXPR_FSUB;
    else if (kind == TK_KW_FMUL) expr_kind = EXPR_FMUL;
    else if (kind == TK_KW_FDIV) expr_kind = EXPR_FDIV;
    else if (kind == TK_KW_FEQ) expr_kind = EXPR_FEQ;
    else if (kind == TK_KW_FLT) expr_kind = EXPR_FLT;
    expr = lainir_new_expr(p->builder, expr_kind);
    expr->data.bin.left = args[0];
    expr->data.bin.right = args[1];
    free(args);
    return expr;
  }
  if (kind == TK_KW_CALL_INDIRECT) {
    expr = lainir_new_expr(p->builder, EXPR_CALL_INDIRECT);
    if (count == 0)
      parse_fail(p, "call_indirect expects at least one operand");
    expr->data.call_indirect.fn_ptr = args[0];
    expr->data.call_indirect.arg_count = count - 1;
    expr->data.call_indirect.param_count = count - 1;
    expr->data.call_indirect.ret_ty = lainir_new_type(p->builder, TY_BITS, 64);
    expr->data.call_indirect.args = NULL;
    expr->data.call_indirect.param_tys = NULL;
    if (count > 1) {
      expr->data.call_indirect.args = lainir_builder_allocate(
          p->builder, (size_t)(count - 1) * sizeof(L1Expr *));
      expr->data.call_indirect.param_tys = lainir_builder_allocate(
          p->builder, (size_t)(count - 1) * sizeof(L1Type *));
      for (uint32_t i = 1; i < count; i++) {
        expr->data.call_indirect.args[i - 1] = args[i];
        expr->data.call_indirect.param_tys[i - 1] = infer_expr_type(args[i]);
      }
    }
    free(args);
    return expr;
  }

  parse_fail(p, "unknown special expression");
  return NULL;
}

static L1Expr *parse_ident_expr(Parser *p) {
  Token token = expect(p, TK_IDENT);
  char *name = token_string(token);
  L1Expr *expr;

  expr = new_var_expr(p, name);
  free(name);
  return expr;
}

static L1Expr *parse_lea_expr(Parser *p) {
  L1Expr *base = NULL;
  L1Expr *idx = new_const_expr(p, 0);
  uint32_t scale = 0;
  uint32_t offset = 0;

  expect(p, TK_LPAREN);
  while (1) {
    Token key = expect(p, TK_IDENT);
    char *name = token_string(key);

    expect(p, TK_EQ);
    if (strcmp(name, "base") == 0) {
      base = parse_expr(p);
    } else if (strcmp(name, "idx") == 0) {
      idx = parse_expr(p);
    } else if (strcmp(name, "scale") == 0 || strcmp(name, "offset") == 0) {
      Token number = expect(p, TK_NUMBER);
      char *text = token_string(number);
      if (strcmp(name, "scale") == 0)
        scale = (uint32_t)strtoul(text, NULL, 10);
      else
        offset = (uint32_t)strtoul(text, NULL, 10);
      free(text);
    } else {
      free(name);
      parse_fail(p, "unknown lea key");
    }
    free(name);

    if (p->current.kind != TK_COMMA)
      break;
    next_token(p);
  }
  expect(p, TK_RPAREN);

  if (!base)
    parse_fail(p, "lea missing base");

  {
    L1Expr *expr = lainir_new_expr(p->builder, EXPR_LEA);
    expr->data.lea.base = base;
    expr->data.lea.idx = idx;
    expr->data.lea.scale = scale;
    expr->data.lea.offset = offset;
    return expr;
  }
}

static L1Expr *parse_expr_inner(Parser *p) {
  Token token;
  char *text;
  uint32_t count;
  L1Expr **args;
  L1Expr *expr;

  if (p->current.kind == TK_KW_PROC_ADDR) {
    next_token(p);
    expect(p, TK_LPAREN);
    token = expect(p, TK_IDENT);
    text = token_string(token);
    expect(p, TK_RPAREN);
    expr = lainir_new_expr(p->builder, EXPR_PROC_ADDR);
    expr->data.proc_addr.fn_name = text;
    return expr;
  }
  if (p->current.kind == TK_KW_INT2PTR ||
      p->current.kind == TK_KW_PTR2INT) {
    TokenKind conversion_kind = p->current.kind;
    L1Expr *operand;
    next_token(p);
    expect(p, TK_LPAREN);
    operand = parse_expr(p);
    expect(p, TK_RPAREN);
    expr = lainir_new_expr(p->builder, 
        conversion_kind == TK_KW_INT2PTR ? EXPR_INT2PTR : EXPR_PTR2INT);
    expr->data.unary.operand = operand;
    return expr;
  }
  if (p->current.kind == TK_KW_CALL_INDIRECT) {
    next_token(p);
    if (p->current.kind != TK_LBRACK)
      return parse_special_hash_call(p, TK_KW_CALL_INDIRECT);
    next_token(p);
    expect(p, TK_LPAREN);
    {
      uint32_t param_cap = 4;
      uint32_t param_count = 0;
      L1Type **param_tys = NULL;
      L1Type *return_ty;
      if (p->current.kind != TK_RPAREN) {
        param_tys = lainir_builder_allocate(
            p->builder, (size_t)param_cap * sizeof(L1Type *));
        while (1) {
          if (param_count == param_cap) {
            size_t previous = (size_t)param_cap * sizeof(L1Type *);
            param_cap *= 2;
            param_tys = lainir_builder_grow(p->builder, param_tys, previous,
                                            (size_t)param_cap * sizeof(L1Type *));
          }
          param_tys[param_count++] = parse_type(p);
          if (p->current.kind != TK_COMMA) break;
          next_token(p);
        }
      }
      expect(p, TK_RPAREN);
      expect(p, TK_ARROW);
      return_ty = parse_type(p);
      expect(p, TK_RBRACK);
      expect(p, TK_LPAREN);
      args = parse_expr_list(p, &count);
      expect(p, TK_RPAREN);
      if (!count) parse_fail(p, "call_indirect requires a target");
      expr = lainir_new_expr(p->builder, EXPR_CALL_INDIRECT);
      expr->data.call_indirect.fn_ptr = args[0];
      expr->data.call_indirect.ret_ty = return_ty;
      expr->data.call_indirect.param_tys = param_tys;
      expr->data.call_indirect.param_count = param_count;
      expr->data.call_indirect.arg_count = count - 1;
      expr->data.call_indirect.args =
          count > 1 ? lainir_builder_allocate(
                          p->builder, (size_t)(count - 1) * sizeof(L1Expr *))
                    : NULL;
      for (uint32_t i = 1; i < count; i++)
        expr->data.call_indirect.args[i - 1] = args[i];
      free(args);
      return expr;
    }
  }
  if (p->current.kind == TK_KW_ZEXT ||
      p->current.kind == TK_KW_SEXT ||
      p->current.kind == TK_KW_TRUNC ||
      p->current.kind == TK_KW_BITCAST) {
    TokenKind conversion_kind = p->current.kind;
    next_token(p);
    expect(p, TK_LBRACK);
    L1Type *target_ty = parse_type(p);
    expect(p, TK_RBRACK);
    expect(p, TK_LPAREN);
    L1Expr *operand = parse_expr(p);
    expect(p, TK_RPAREN);
    expr = lainir_new_expr(p->builder, 
        conversion_kind == TK_KW_ZEXT ? EXPR_ZEXT :
        conversion_kind == TK_KW_SEXT ? EXPR_SEXT :
        conversion_kind == TK_KW_TRUNC ? EXPR_TRUNC : EXPR_BITCAST);
    expr->data.conversion.operand = operand;
    expr->data.conversion.target_ty = target_ty;
    return expr;
  }
  if (p->current.kind == TK_KW_LEA) {
    next_token(p);
    return parse_lea_expr(p);
  }
  if (p->current.kind == TK_KW_LOAD) {
    L1Type *load_ty = NULL;
    L1Expr *addr;
    next_token(p);
    if (p->current.kind == TK_LBRACK) {
      next_token(p);
      load_ty = parse_type(p);
      expect(p, TK_RBRACK);
    }
    expect(p, TK_LPAREN);
    addr = parse_expr(p);
    expect(p, TK_RPAREN);
    expr = lainir_new_expr(p->builder, EXPR_LOAD);
    expr->data.load.addr = addr;
    expr->data.load.ty = load_ty;
    return expr;
  }

  switch (p->current.kind) {
  case TK_NUMBER:
    token = expect(p, TK_NUMBER);
    text = token_string(token);
    expr = new_const_expr(p, strtoll(text, NULL, 10));
    free(text);
    return expr;
  case TK_STRING:
    token = expect(p, TK_STRING);
    expr = lainir_new_expr(p->builder, EXPR_STRING);
    expr->data.str_val.content = token_string_literal(token, NULL);
    expr->data.str_val.ty = lainir_new_type(p->builder, TY_ADDR, 64);
    return expr;
  case TK_PERCENT:
    return parse_percent_ref(p);
  case TK_KW_EVAL:
    next_token(p);
    expect(p, TK_LBRACE);
    expr = lainir_new_expr(p->builder, EXPR_EVAL);
    expr->data.eval.block = parse_block_instructions(p);
    expect(p, TK_RBRACE);
    expr->data.eval.ret_ty = NULL;
    return expr;
  case TK_KW_DATA_ADDR:
    next_token(p);
    expect(p, TK_LPAREN);
    token = expect(p, TK_IDENT);
    text = token_string(token);
    expect(p, TK_RPAREN);
    expr = lainir_new_expr(p->builder, EXPR_DATA_ADDR);
    expr->data.data_addr.name = text;
    expr->data.data_addr.ty = lainir_new_type(p->builder, TY_ADDR, 64);
    return expr;

  case TK_KW_CALL:
    next_token(p);
    token = expect(p, TK_IDENT);
    text = token_string(token);
    expect(p, TK_LPAREN);
    args = parse_expr_list(p, &count);
    expect(p, TK_RPAREN);
    expr = lainir_new_expr(p->builder, EXPR_CALL);
    expr->data.call.fn_name = lainir_builder_copy_string(p->builder, text);
    expr->data.call.args = args;
    expr->data.call.arg_count = count;
    expr->data.call.ret_ty = NULL;
    free(text);
    return expr;
  case TK_KW_ALLOCA:
    next_token(p);
    expect(p, TK_LPAREN);
    expr = lainir_new_expr(p->builder, EXPR_ALLOCA);
    if (p->current.kind == TK_NUMBER) {
      token = expect(p, TK_NUMBER);
      text = token_string(token);
      expr->data.alloca.byte_size = (uint32_t)strtoul(text, NULL, 10);
      expr->data.alloca.element_ty = NULL;
      free(text);
    } else {
      expr->data.alloca.element_ty = parse_type(p);
      expr->data.alloca.byte_size = 0;
    }
    expr->data.alloca.result_ty = lainir_new_type(p->builder, TY_ADDR, 64);
    expect(p, TK_RPAREN);
    return expr;
  case TK_KW_LOAD:
  case TK_KW_ADD:
  case TK_KW_SUB_OP:
  case TK_KW_MUL_OP:
  case TK_KW_EQ_OP:
  case TK_KW_NE_OP:
  case TK_KW_SDIV:
  case TK_KW_UDIV:
  case TK_KW_SLT:
  case TK_KW_SLE:
  case TK_KW_SGT:
  case TK_KW_SGE:
  case TK_KW_ULT:
  case TK_KW_ULE:
  case TK_KW_UGT:
  case TK_KW_UGE:
  case TK_KW_FADD:
  case TK_KW_FSUB:
  case TK_KW_FMUL:
  case TK_KW_FDIV:
  case TK_KW_FEQ:
  case TK_KW_FLT:
  case TK_KW_CALL_INDIRECT:
    {
      TokenKind kind = p->current.kind;
      next_token(p);
      return parse_special_hash_call(p, kind);
    }
  case TK_IDENT:
    return parse_ident_expr(p);
  default:
    parse_fail(p, "unsupported expression");
    break;
  }

  return NULL;
}

/* Keep source ownership at the expression boundary without duplicating span
 * bookkeeping in every expression constructor.  Recursive calls use this
 * wrapper too, so the outer expression receives the final token consumed by
 * its full subtree. */
static L1Expr *parse_expr(Parser *p) {
  int start = p->current.start;
  L1Expr *expr = parse_expr_inner(p);
  if (expr) {
    expr->source_start = (uint64_t)start;
    expr->source_end = (uint64_t)p->last_token_end;
  }
  return expr;
}

static L1Instruction *parse_instruction_list(Parser *p) {
  L1Instruction *head = NULL;
  L1Instruction *tail = NULL;

  while (p->current.kind != TK_RBRACE && p->current.kind != TK_EOF) {
    L1Instruction *inst = NULL;
    int instruction_line = p->current.line;
    size_t instruction_start = (size_t)p->current.start;

    if (p->current.kind == TK_KW_LET) {
      next_token(p);
      expect(p, TK_PERCENT);
      Token token = expect(p, TK_IDENT);
      char *name = token_string(token);
      expect(p, TK_COLON);
      L1Type *ty = parse_type(p);
      expect(p, TK_EQ);
      inst = lainir_new_instruction(p->builder, INST_LET);
      inst->data.let.name = name;
      inst->data.let.ty = ty;
      inst->data.let.val = parse_expr(p);
    } else if (p->current.kind == TK_PERCENT) {
      Token token;
      char *name;

      next_token(p);
      token = expect(p, TK_IDENT);
      name = token_string(token);
      L1Type *ty = NULL;
      if (p->current.kind == TK_COLON) {
        next_token(p);
        ty = parse_type(p);
      }
      expect(p, TK_EQ);

      inst = lainir_new_instruction(p->builder, INST_SET);
      inst->data.set.name = name;
      inst->data.set.ty = ty;
      inst->data.set.val = parse_expr(p);
    } else if (p->current.kind == TK_KW_STORE) {
      next_token(p);
      inst = lainir_new_instruction(p->builder, INST_STORE);
      inst->data.store.store_ty = NULL;
      if (p->current.kind == TK_LBRACK) {
        next_token(p);
        inst->data.store.store_ty = parse_type(p);
        expect(p, TK_RBRACK);
      }
      inst->data.store.val = parse_expr(p);
      expect(p, TK_COMMA);
      inst->data.store.dest = parse_expr(p);
    } else if (p->current.kind == TK_KW_CALL) {
      inst = lainir_new_instruction(p->builder, INST_CALL);
      inst->data.call_inst.expr = parse_expr(p);
    } else if (p->current.kind == TK_KW_RETURN) {
      next_token(p);
      inst = lainir_new_instruction(p->builder, INST_RETURN);
      if (p->current.kind != TK_RBRACE && p->current.kind != TK_SEMICOLON) {
        inst->data.ret.val = parse_expr(p);
      }
    } else if (p->current.kind == TK_KW_IF) {
      next_token(p);
      inst = lainir_new_instruction(p->builder, INST_IF);
      inst->data.if_stmt.condition = parse_expr(p);
      expect(p, TK_LBRACE);
      inst->data.if_stmt.then_body = parse_block_instructions(p);
      expect(p, TK_RBRACE);
      if (p->current.kind == TK_KW_ELSE) {
        next_token(p);
        expect(p, TK_LBRACE);
        inst->data.if_stmt.else_body = parse_block_instructions(p);
        expect(p, TK_RBRACE);
      }
    } else if (p->current.kind == TK_KW_LOOP) {
      next_token(p);
      inst = lainir_new_instruction(p->builder, INST_LOOP);
      inst->data.loop.label = NULL;
      if (p->current.kind == TK_IDENT) {
        Token label = expect(p, TK_IDENT);
        inst->data.loop.label = token_string(label);
      }
      expect(p, TK_LBRACE);
      inst->data.loop.body = parse_block_instructions(p);
      expect(p, TK_RBRACE);
    } else if (p->current.kind == TK_KW_BREAK) {
      next_token(p);
      inst = lainir_new_instruction(p->builder, INST_BREAK);
      inst->data.jump.label = NULL;
      if (p->current.kind == TK_IDENT) {
        Token label = expect(p, TK_IDENT);
        inst->data.jump.label = token_string(label);
      }
    } else if (p->current.kind == TK_KW_CONTINUE) {
      next_token(p);
      inst = lainir_new_instruction(p->builder, INST_CONTINUE);
      inst->data.jump.label = NULL;
      if (p->current.kind == TK_IDENT) {
        Token label = expect(p, TK_IDENT);
        inst->data.jump.label = token_string(label);
      }
    } else {
      break;
    }

    if (inst) {
      inst->line = instruction_line;
      inst->column = 1;
      inst->source_start = (uint64_t)instruction_start;
      inst->source_end = (uint64_t)p->last_token_end;
    }

    if (!head)
      head = inst;
    else
      tail->next = inst;
    tail = inst;
  }

  return head;
}

static L1Block *parse_block_instructions(Parser *p) {
  L1Block *block = lainir_new_block(p->builder);
  L1Instruction *insts = parse_instruction_list(p);
  if (insts) {
    block->body = insts;
    block->body_tail = insts;
    while (block->body_tail && block->body_tail->next)
      block->body_tail = block->body_tail->next;
  }
  return block;
}

static L1Block *parse_block(Parser *p) {
  Token label = expect(p, TK_IDENT);
  char *label_name = token_string(label);
  L1Block *block;

  expect(p, TK_COLON);
  block = lainir_new_block(p->builder);
  free(label_name);

  while (1) {
    if (p->current.kind == TK_IDENT ||
        p->current.kind == TK_RBRACE ||
        p->current.kind == TK_EOF)
      break;

    {
      L1Instruction *insts = parse_instruction_list(p);
      if (!insts)
        parse_fail(p, "unexpected token in block");
      if (!block->body) {
        block->body = insts;
        block->body_tail = insts;
      } else {
        block->body_tail->next = insts;
      }
      while (block->body_tail && block->body_tail->next)
        block->body_tail = block->body_tail->next;
    }
  }

  return block;
}

static void parse_typed_params(Parser *p, L1Subroutine *sub) {
  uint32_t cap = 4;

  if (p->current.kind == TK_RPAREN)
    return;

  sub->param_tys = lainir_builder_allocate(
      p->builder, (size_t)cap * sizeof(L1Type *));
  sub->param_names = lainir_builder_allocate(
      p->builder, (size_t)cap * sizeof(char *));
  while (1) {
    if (sub->param_count == cap) {
      size_t previous_tys = (size_t)cap * sizeof(L1Type *);
      size_t previous_names = (size_t)cap * sizeof(char *);
      cap *= 2;
      sub->param_tys = lainir_builder_grow(p->builder, sub->param_tys,
                                           previous_tys,
                                           sizeof(L1Type *) * cap);
      sub->param_names = lainir_builder_grow(p->builder, sub->param_names,
                                             previous_names,
                                             sizeof(char *) * cap);
    }
    sub->param_tys[sub->param_count++] = parse_type(p);
    expect(p, TK_PERCENT);
    {
      Token token = expect(p, TK_IDENT);
      char *name = token_string(token);
      parser_add_param_name(p, name);
      sub->param_names[sub->param_count - 1] = lainir_builder_copy_string(p->builder, name);
      free(name);
    }
    if (p->current.kind != TK_COMMA)
      break;
    next_token(p);
  }
}

static L1Subroutine *parse_subroutine(Parser *p) {
  Token name_token;
  char *name;
  L1Subroutine *sub;
  L1Block *tail = NULL;

  if (p->current.kind == TK_KW_EXTERN) {
    parser_reset_subroutine_context(p);
    next_token(p);
    expect(p, TK_KW_PROC);
    name_token = expect(p, TK_IDENT);
    name = token_string(name_token);
    sub = lainir_new_subroutine(p->builder, name);
    sub->is_extern = 1;
    free(name);
    expect(p, TK_LPAREN);
    parse_typed_params(p, sub);
    expect(p, TK_RPAREN);
    expect(p, TK_ARROW);
    sub->ret_ty = parse_type(p);
    expect(p, TK_SEMICOLON);
    return sub;
  }

  parser_reset_subroutine_context(p);
  expect(p, TK_KW_PROC);
  name_token = expect(p, TK_IDENT);
  name = token_string(name_token);
  sub = lainir_new_subroutine(p->builder, name);
  free(name);

  expect(p, TK_LPAREN);
  parse_typed_params(p, sub);
  expect(p, TK_RPAREN);
  expect(p, TK_ARROW);
  sub->ret_ty = parse_type(p);
  expect(p, TK_LBRACE);

  /* Canonical structured LAIN-IR writes instructions directly inside the
     procedure region. Legacy block_N: spellings remain accepted on input,
     but labels are not branch targets and are not emitted again. */
  if (p->current.kind != TK_IDENT && p->current.kind != TK_RBRACE) {
    L1Block *block = parse_block_instructions(p);
    block->parent = sub;
    sub->blocks = block;
    sub->blocks_tail = block;
    tail = block;
  }

  while (p->current.kind == TK_IDENT) {
    L1Block *block = parse_block(p);
    block->parent = sub;
    if (!sub->blocks)
      sub->blocks = block;
    else
      tail->next = block;
    tail = block;
    sub->blocks_tail = block;
  }

  expect(p, TK_RBRACE);
  return sub;
}

static L1Subroutine *parse_data(Parser *p) {
  Token token;
  char *name;
  char *text;
  char *content;
  size_t content_length;
  uint32_t size;
  uint32_t alignment;
  uintptr_t raw;
  uintptr_t aligned;
  L1Subroutine *data;

  expect(p, TK_KW_DATA);
  token = expect(p, TK_IDENT);
  name = token_string(token);
  expect(p, TK_LPAREN);
  token = expect(p, TK_NUMBER);
  text = token_string(token);
  size = (uint32_t)strtoul(text, NULL, 10);
  free(text);
  expect(p, TK_COMMA);
  token = expect(p, TK_NUMBER);
  text = token_string(token);
  alignment = (uint32_t)strtoul(text, NULL, 10);
  free(text);
  expect(p, TK_COMMA);
  token = expect(p, TK_STRING);
  content = token_string_literal(token, &content_length);
  expect(p, TK_RPAREN);
  expect(p, TK_SEMICOLON);
  if (!size) parse_fail(p, "#data size must be non-zero");
  if (!alignment || (alignment & (alignment - 1)) != 0)
    parse_fail(p, "#data alignment must be a power of two");
  if (content_length > size)
    parse_fail(p, "#data initializer exceeds declared size");

  data = lainir_new_subroutine(p->builder, name);
  data->is_data = 1;
  data->data_size = size;
  data->data_alignment = alignment;
  data->data_storage = lainir_builder_allocate(
      p->builder, (size_t)size + alignment - 1);
  if (!data->data_storage) parse_fail(p, "out of memory for #data");
  raw = (uintptr_t)data->data_storage;
  aligned = (raw + alignment - 1) & ~((uintptr_t)alignment - 1);
  data->data_bytes = (uint8_t *)aligned;
  memcpy(data->data_bytes, content, content_length);
  free(content);
  free(name);
  return data;
}

int lainir_parse_module_checked(L1Builder *builder,
                                const char *src,
                                L1Subroutine **out_module,
                                L1Diagnostic *diagnostic) {
  Parser p = {.src = src, .pos = 0, .line = 1,
              .diagnostic = diagnostic, .builder = builder};
  L1Subroutine *head = NULL;
  L1Subroutine *tail = NULL;

  if (out_module)
    *out_module = NULL;
  if (!builder)
    return 0;
  if (diagnostic)
    memset(diagnostic, 0, sizeof(*diagnostic));
  if (setjmp(p.failure)) {
    /* Every node built so far belongs to the builder; the caller releases it.
     * Nothing is published through *out_module. */
    parser_reset_subroutine_context(&p);
    return 0;
  }

  next_token(&p);
  while (p.current.kind != TK_EOF) {
    L1Subroutine *sub = p.current.kind == TK_KW_DATA
                            ? parse_data(&p) : parse_subroutine(&p);
    if (!head)
      head = sub;
    else
      tail->next = sub;
    tail = sub;
  }
  parser_reset_subroutine_context(&p);
  lainir_builder_set_root(builder, head);
  if (out_module)
    *out_module = head;
  return 1;
}

L1Subroutine *lainir_parse_module(L1Builder *builder, const char *src) {
  L1Subroutine *module = NULL;
  L1Diagnostic diagnostic;
  if (!lainir_parse_module_checked(builder, src, &module, &diagnostic))
    return NULL;
  return module;
}
