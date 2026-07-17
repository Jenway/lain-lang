#include "lainir.h"

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
  TK_KW_PRIMITIVE,
  TK_KW_ALLOCA,
  TK_KW_FIELD,
  TK_KW_LEA,
  TK_KW_LOAD,
  TK_KW_ADD,
  TK_KW_SUB_OP,
  TK_KW_EQ_OP,
  TK_KW_NE_OP,
  TK_KW_LT_OP,
  TK_KW_LE_OP,
  TK_KW_GT_OP,
  TK_KW_GE_OP,
  TK_KW_CALL_INDIRECT,
  TK_KW_EVAL
} TokenKind;

typedef struct {
  TokenKind kind;
  const char *text;
  int len;
  int line;
} Token;

typedef struct {
  const char *src;
  int pos;
  int line;
  Token current;
  char **param_names;
  uint32_t param_count;
  uint32_t param_cap;
  jmp_buf failure;
  L1Diagnostic *diagnostic;
  L1Subroutine *module_head;
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
  if (!copy) {
    fprintf(stderr, "lainir parser: out of memory\n");
    exit(1);
  }
  memcpy(copy, token.text, len);
  copy[len] = '\0';
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
  if (len == 9 && memcmp(text, "primitive", 9) == 0) return TK_KW_PRIMITIVE;
  if (len == 6 && memcmp(text, "alloca", 6) == 0) return TK_KW_ALLOCA;
  if (len == 5 && memcmp(text, "field", 5) == 0) return TK_KW_FIELD;
  if (len == 3 && memcmp(text, "lea", 3) == 0) return TK_KW_LEA;
  if (len == 4 && memcmp(text, "load", 4) == 0) return TK_KW_LOAD;
  if (len == 3 && memcmp(text, "add", 3) == 0) return TK_KW_ADD;
  if (len == 3 && memcmp(text, "sub", 3) == 0) return TK_KW_SUB_OP;
  if (len == 2 && memcmp(text, "eq", 2) == 0) return TK_KW_EQ_OP;
  if (len == 2 && memcmp(text, "ne", 2) == 0) return TK_KW_NE_OP;
  if (len == 2 && memcmp(text, "lt", 2) == 0) return TK_KW_LT_OP;
  if (len == 2 && memcmp(text, "le", 2) == 0) return TK_KW_LE_OP;
  if (len == 2 && memcmp(text, "gt", 2) == 0) return TK_KW_GT_OP;
  if (len == 2 && memcmp(text, "ge", 2) == 0) return TK_KW_GE_OP;
  if (len == 13 && memcmp(text, "call_indirect", 13) == 0) return TK_KW_CALL_INDIRECT;
  if (len == 4 && memcmp(text, "eval", 4) == 0) return TK_KW_EVAL;
  return TK_IDENT;
}

static void next_token(Parser *p) {
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

  p->current.text = p->src + p->pos;
  p->current.line = p->line;
  p->current.len = 1;

  if (!p->src[p->pos]) {
    p->current.kind = TK_EOF;
    p->current.len = 0;
    return;
  }

  if (isdigit((unsigned char)p->src[p->pos])) {
    int start = p->pos;
    while (isdigit((unsigned char)p->src[p->pos]))
      p->pos++;
    p->current.kind = TK_NUMBER;
    p->current.text = p->src + start;
    p->current.len = p->pos - start;
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
  L1Type *ty;

  if (token.kind == TK_HASH_UNIT) {
    next_token(p);
    return lainir_new_type(TY_UNIT, 0);
  }
  if (token.kind == TK_HASH_NEVER) {
    next_token(p);
    return lainir_new_type(TY_NEVER, 0);
  }
  if (token.kind != TK_IDENT)
    parse_fail(p, "expected type");

  text = token_string(token);
  next_token(p);

  if (strcmp(text, "addr") == 0) {
    free(text);
    return lainir_new_type(TY_ADDR, 64);
  }
  if (text[0] == 'i' && isdigit((unsigned char)text[1])) {
    ty = lainir_new_type(TY_BITS, (uint32_t)strtoul(text + 1, NULL, 10));
    free(text);
    return ty;
  }

  free(text);
  parse_fail(p, "unknown type");
  return NULL;
}

static L1Expr *parse_expr(Parser *p);
static L1Block *parse_block_instructions(Parser *p);

static L1Expr *new_const_expr(int64_t value) {
  L1Expr *expr = lainir_new_expr(EXPR_CONST);
  expr->data.const_val = value;
  return expr;
}

static L1Expr *new_var_expr(const char *name) {
  L1Expr *expr = lainir_new_expr(EXPR_VAR);
  expr->data.var.name = strdup(name);
  expr->data.var.ty = NULL;
  return expr;
}

static L1Expr *new_arg_expr(uint32_t index) {
  L1Expr *expr = lainir_new_expr(EXPR_ARG);
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
      return new_arg_expr((uint32_t)param_index);
    }
  }

  if (strncmp(name, "arg", 3) == 0) {
    index = strtoul(name + 3, &end, 10);
    if (name[3] && *end == '\0') {
      free(name);
      return new_arg_expr((uint32_t)index);
    }
  }

  expr = new_var_expr(name);
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
    expr = lainir_new_expr(EXPR_LOAD);
    expr->data.load.addr = args[0];
    expr->data.load.ty = NULL;
    free(args);
    return expr;
  }
  if (kind == TK_KW_ADD || kind == TK_KW_SUB_OP ||
      kind == TK_KW_EQ_OP || kind == TK_KW_NE_OP ||
      kind == TK_KW_LT_OP || kind == TK_KW_LE_OP ||
      kind == TK_KW_GT_OP || kind == TK_KW_GE_OP) {
    if (count != 2)
      parse_fail(p, "binary op expects two operands");
    L1ExprKind expr_kind = EXPR_ADD;
    if (kind == TK_KW_SUB_OP) expr_kind = EXPR_SUB;
    else if (kind == TK_KW_EQ_OP) expr_kind = EXPR_EQ;
    else if (kind == TK_KW_NE_OP) expr_kind = EXPR_NE;
    else if (kind == TK_KW_LT_OP) expr_kind = EXPR_LT;
    else if (kind == TK_KW_LE_OP) expr_kind = EXPR_LE;
    else if (kind == TK_KW_GT_OP) expr_kind = EXPR_GT;
    else if (kind == TK_KW_GE_OP) expr_kind = EXPR_GE;
    expr = lainir_new_expr(expr_kind);
    expr->data.bin.left = args[0];
    expr->data.bin.right = args[1];
    free(args);
    return expr;
  }
  if (kind == TK_KW_CALL_INDIRECT) {
    expr = lainir_new_expr(EXPR_CALL_INDIRECT);
    if (count == 0)
      parse_fail(p, "call_indirect expects at least one operand");
    expr->data.call_indirect.fn_ptr = args[0];
    expr->data.call_indirect.arg_count = count - 1;
    expr->data.call_indirect.param_count = count - 1;
    expr->data.call_indirect.ret_ty = lainir_new_type(TY_BITS, 64);
    expr->data.call_indirect.args = NULL;
    expr->data.call_indirect.param_tys = NULL;
    if (count > 1) {
      expr->data.call_indirect.args = calloc(count - 1, sizeof(L1Expr *));
      expr->data.call_indirect.param_tys = calloc(count - 1, sizeof(L1Type *));
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

  expr = new_var_expr(name);
  free(name);
  return expr;
}

static L1Expr *parse_field_expr(Parser *p) {
  Token token;
  char *text;
  L1Expr *expr = lainir_new_expr(EXPR_FIELD);

  expect(p, TK_LBRACK);
  token = expect(p, TK_NUMBER);
  text = token_string(token);
  expr->data.field.field_index = (uint32_t)strtoul(text, NULL, 10);
  free(text);
  expr->data.field.field_ty = NULL;
  expect(p, TK_RBRACK);
  expect(p, TK_LPAREN);

  expr->data.field.base = parse_expr(p);
  expr->data.field.struct_ty = NULL;

  expect(p, TK_RPAREN);
  if (p->current.kind == TK_COLON) {
    next_token(p);
    expr->data.field.field_ty = parse_type(p);
  }
  return expr;
}

static L1Expr *parse_lea_expr(Parser *p) {
  L1Expr *base = NULL;
  L1Expr *idx = new_const_expr(0);
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
    L1Expr *expr = lainir_new_expr(EXPR_LEA);
    expr->data.lea.base = base;
    expr->data.lea.idx = idx;
    expr->data.lea.scale = scale;
    expr->data.lea.offset = offset;
    return expr;
  }
}

static L1Expr *parse_expr(Parser *p) {
  Token token;
  char *text;
  uint32_t count;
  L1Expr **args;
  L1Expr *expr;

  if (p->current.kind == TK_KW_FIELD) {
    next_token(p);
    return parse_field_expr(p);
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
    expr = lainir_new_expr(EXPR_LOAD);
    expr->data.load.addr = addr;
    expr->data.load.ty = load_ty;
    return expr;
  }

  switch (p->current.kind) {
  case TK_NUMBER:
    token = expect(p, TK_NUMBER);
    text = token_string(token);
    expr = new_const_expr(strtoll(text, NULL, 10));
    free(text);
    return expr;
  case TK_STRING:
    token = expect(p, TK_STRING);
    expr = lainir_new_expr(EXPR_STRING);
    expr->data.str_val.content = token_string(token);
    expr->data.str_val.ty = lainir_new_type(TY_ADDR, 64);
    return expr;
  case TK_PERCENT:
    return parse_percent_ref(p);
  case TK_KW_EVAL:
    next_token(p);
    token = expect(p, TK_IDENT);
    text = token_string(token);
    expect(p, TK_LPAREN);
    args = parse_expr_list(p, &count);
    expect(p, TK_RPAREN);
    expr = lainir_new_expr(EXPR_EVAL);
    expr->data.eval.fn_name = strdup(text);
    expr->data.eval.args = args;
    expr->data.eval.arg_count = count;
    expr->data.eval.ret_ty = NULL;
    free(text);
    return expr;

  case TK_KW_CALL:
    next_token(p);
    token = expect(p, TK_IDENT);
    text = token_string(token);
    expect(p, TK_LPAREN);
    args = parse_expr_list(p, &count);
    expect(p, TK_RPAREN);
    expr = lainir_new_expr(EXPR_CALL);
    expr->data.call.fn_name = strdup(text);
    expr->data.call.args = args;
    expr->data.call.arg_count = count;
    expr->data.call.ret_ty = NULL;
    free(text);
    return expr;
  case TK_KW_PRIMITIVE:
    next_token(p);
    token = expect(p, TK_IDENT);
    text = token_string(token);
    expect(p, TK_LPAREN);
    args = parse_expr_list(p, &count);
    expect(p, TK_RPAREN);
    expr = lainir_new_expr(EXPR_PRIMITIVE);
    expr->data.primitive.opcode = strdup(text);
    expr->data.primitive.operands = args;
    expr->data.primitive.operand_count = count;
    expr->data.primitive.result_ty = NULL;
    free(text);
    return expr;
  case TK_KW_ALLOCA:
    next_token(p);
    expect(p, TK_LPAREN);
    expr = lainir_new_expr(EXPR_ALLOCA);
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
    expr->data.alloca.result_ty = lainir_new_type(TY_ADDR, 64);
    expect(p, TK_RPAREN);
    return expr;
  case TK_KW_LOAD:
  case TK_KW_ADD:
  case TK_KW_SUB_OP:
  case TK_KW_EQ_OP:
  case TK_KW_NE_OP:
  case TK_KW_LT_OP:
  case TK_KW_LE_OP:
  case TK_KW_GT_OP:
  case TK_KW_GE_OP:
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

static L1Instruction *parse_instruction_list(Parser *p) {
  L1Instruction *head = NULL;
  L1Instruction *tail = NULL;

  while (p->current.kind != TK_RBRACE && p->current.kind != TK_EOF) {
    L1Instruction *inst = NULL;

    if (p->current.kind == TK_KW_LET) {
      next_token(p);
      /* Canonical spelling is `#let %name: type = value`.  The percent and
         type annotation were absent from the first text grammar, so retain
         both legacy spellings as input during the migration. */
      if (p->current.kind == TK_PERCENT)
        next_token(p);
      Token token = expect(p, TK_IDENT);
      char *name = token_string(token);
      L1Type *ty = NULL;
      if (p->current.kind == TK_COLON) {
        next_token(p);
        ty = parse_type(p);
      }
      expect(p, TK_EQ);
      inst = lainir_new_instruction(INST_LET);
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

      inst = lainir_new_instruction(INST_SET);
      inst->data.set.name = name;
      inst->data.set.ty = ty;
      inst->data.set.val = parse_expr(p);
    } else if (p->current.kind == TK_KW_STORE) {
      next_token(p);
      inst = lainir_new_instruction(INST_STORE);
      inst->data.store.val = parse_expr(p);
      expect(p, TK_COMMA);
      inst->data.store.dest = parse_expr(p);
      inst->data.store.store_ty = NULL;
    } else if (p->current.kind == TK_KW_CALL) {
      inst = lainir_new_instruction(INST_CALL);
      inst->data.call_inst.expr = parse_expr(p);
    } else if (p->current.kind == TK_KW_RETURN) {
      next_token(p);
      inst = lainir_new_instruction(INST_RETURN);
      if (p->current.kind != TK_RBRACE && p->current.kind != TK_SEMICOLON) {
        inst->data.ret.val = parse_expr(p);
      }
    } else if (p->current.kind == TK_KW_IF) {
      next_token(p);
      inst = lainir_new_instruction(INST_IF);
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
      inst = lainir_new_instruction(INST_LOOP);
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
      inst = lainir_new_instruction(INST_BREAK);
      inst->data.jump.label = NULL;
      if (p->current.kind == TK_IDENT) {
        Token label = expect(p, TK_IDENT);
        inst->data.jump.label = token_string(label);
      }
    } else if (p->current.kind == TK_KW_CONTINUE) {
      next_token(p);
      inst = lainir_new_instruction(INST_CONTINUE);
      inst->data.jump.label = NULL;
      if (p->current.kind == TK_IDENT) {
        Token label = expect(p, TK_IDENT);
        inst->data.jump.label = token_string(label);
      }
    } else {
      break;
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
  L1Block *block = lainir_new_block();
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
  block = lainir_new_block();
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

  sub->param_tys = calloc(cap, sizeof(L1Type *));
  while (1) {
    if (sub->param_count == cap) {
      cap *= 2;
      sub->param_tys = realloc(sub->param_tys, sizeof(L1Type *) * cap);
    }
    sub->param_tys[sub->param_count++] = parse_type(p);
    expect(p, TK_PERCENT);
    {
      Token token = expect(p, TK_IDENT);
      char *name = token_string(token);
      parser_add_param_name(p, name);
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
    sub = lainir_new_subroutine(name);
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
  sub = lainir_new_subroutine(name);
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

int lainir_parse_module_checked(const char *src, L1Subroutine **out_module,
                                L1Diagnostic *diagnostic) {
  Parser p = {.src = src, .pos = 0, .line = 1, .diagnostic = diagnostic};
  L1Subroutine *head = NULL;
  L1Subroutine *tail = NULL;

  if (out_module)
    *out_module = NULL;
  if (diagnostic)
    memset(diagnostic, 0, sizeof(*diagnostic));
  if (setjmp(p.failure)) {
    parser_reset_subroutine_context(&p);
    lainir_free_subroutines(p.module_head);
    return 0;
  }

  next_token(&p);
  while (p.current.kind != TK_EOF) {
    L1Subroutine *sub = parse_subroutine(&p);
    if (!head)
      head = sub;
    else
      tail->next = sub;
    tail = sub;
    p.module_head = head;
  }
  parser_reset_subroutine_context(&p);
  if (out_module)
    *out_module = head;
  return 1;
}

L1Subroutine *lainir_parse_module(const char *src) {
  L1Subroutine *module = NULL;
  L1Diagnostic diagnostic;
  if (!lainir_parse_module_checked(src, &module, &diagnostic)) {
    fprintf(stderr, "line %d: %s\n", diagnostic.line, diagnostic.message);
    return NULL;
  }
  return module;
}
