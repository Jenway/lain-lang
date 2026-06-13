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
#include <unistd.h>
#include "l1_types.h"

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

typedef struct {
  L1Token **tokens;
  uint32_t count;
  uint32_t index;
} L1Cursor;

// ══════════════════════════════════════════════════════════════════════════════
// 2. read_byte_at — single byte from raw pointer (used by lexer.lain)
// ══════════════════════════════════════════════════════════════════════════════

uint8_t read_byte_at(const uint8_t *ptr, size_t offset) { return ptr[offset]; }

// ══════════════════════════════════════════════════════════════════════════════
// 3-5. C-Side Lexer (legacy — used during bootstrap, replaced by lexer.lain)
// ══════════════════════════════════════════════════════════════════════════════

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

// ── Token accumulator for Lain-side lexer → C grouping bridge ──────────────

static RawToken *g_raw_tokens = NULL;
static uint32_t g_raw_token_count = 0;
static uint32_t g_raw_token_cap = 0;

void native_push_token(const uint8_t *src, int32_t kind,
                       size_t start, size_t len, int64_t int_val) {
  if (g_raw_token_count >= g_raw_token_cap) {
    g_raw_token_cap = g_raw_token_cap ? g_raw_token_cap * 2 : 256;
    g_raw_tokens = realloc(g_raw_tokens, sizeof(RawToken) * g_raw_token_cap);
  }
  RawToken *t = &g_raw_tokens[g_raw_token_count++];
  t->kind = (RawTokenKind)kind;
  t->int_val = int_val;
  if (kind == T_IDENT || kind == T_STRING || kind == T_PUNCT)
    t->val = strndup((const char *)(src + start), len);
  else
    t->val = NULL;
}

void *native_finish_grouping(void) {
  if (g_raw_token_count >= g_raw_token_cap) {
    g_raw_token_cap = g_raw_token_cap ? g_raw_token_cap * 2 : 256;
    g_raw_tokens = realloc(g_raw_tokens, sizeof(RawToken) * g_raw_token_cap);
  }
  g_raw_tokens[g_raw_token_count].kind = T_EOF;
  g_raw_tokens[g_raw_token_count].val = NULL;
  g_raw_tokens[g_raw_token_count].int_val = 0;
  g_raw_token_count++;

  uint32_t idx = 0;
  void *root = group_tokens_recursive(g_raw_tokens, g_raw_token_count,
                                      &idx, GRP_ROOT);
  g_raw_token_count = 0;
  return root;
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


// ══════════════════════════════════════════════════════════════════════════════
// 5.5. Token Tree → Scheme S-Expression
// ══════════════════════════════════════════════════════════════════════════════

// ============================================================================
// 5.5. Token Tree → Scheme S-Expression Converter
// ============================================================================
//
// Converts the C-side L1Token* tree into a Scheme list of records,
// eliminating the need for cursor FFI. Scheme code can traverse the
// result with standard car/cdr/match.
//
// Format:
//   (ident "name")   (number 42)   (string "hello")   (punct "->")
//   (paren ...)      (bracket ...)  (brace ...)         (root ...)

static sexp token_to_sexp(sexp ctx, L1Token *tok) {
  if (!tok) return SEXP_NULL;
  
  if (tok->kind == TOK_RAW) {
    RawToken *r = &tok->data.raw;
    switch (r->kind) {
    case T_IDENT: {
      sexp tag = sexp_intern(ctx, "ident", -1);
      sexp val = sexp_c_string(ctx, r->val ? r->val : "", -1);
      return sexp_list2(ctx, tag, val);
    }
    case T_NUMBER: {
      sexp tag = sexp_intern(ctx, "number", -1);
      sexp val = sexp_make_integer(ctx, r->int_val);
      return sexp_list2(ctx, tag, val);
    }
    case T_STRING: {
      sexp tag = sexp_intern(ctx, "string", -1);
      sexp val = sexp_c_string(ctx, r->val ? r->val : "", -1);
      return sexp_list2(ctx, tag, val);
    }
    case T_PUNCT:
    case T_LPAREN: case T_RPAREN:
    case T_LBRACKET: case T_RBRACKET:
    case T_LBRACE: case T_RBRACE: {
      sexp tag = sexp_intern(ctx, "punct", -1);
      const char *s = r->val;
      if (!s) {
        switch (r->kind) {
        case T_LPAREN: s = "("; break;
        case T_RPAREN: s = ")"; break;
        case T_LBRACKET: s = "["; break;
        case T_RBRACKET: s = "]"; break;
        case T_LBRACE: s = "{"; break;
        case T_RBRACE: s = "}"; break;
        default: s = ""; break;
        }
      }
      sexp val = sexp_c_string(ctx, s, -1);
      return sexp_list2(ctx, tag, val);
    }
    default:
      return sexp_intern(ctx, "eof", -1);
    }
  }
  
  // TOK_GROUP
  const char *tag_str = "root";
  switch (tok->data.group.kind) {
  case GRP_PAREN:   tag_str = "paren"; break;
  case GRP_BRACKET: tag_str = "bracket"; break;
  case GRP_BRACE:   tag_str = "brace"; break;
  default: break;
  }
  sexp tag = sexp_intern(ctx, tag_str, -1);
  
  // Build children list
  sexp children = SEXP_NULL;
  for (int i = (int)tok->data.group.count - 1; i >= 0; i--) {
    children = sexp_cons(ctx, token_to_sexp(ctx, tok->data.group.children[i]), children);
  }
  return sexp_cons(ctx, tag, children);
}

void *native_lex_to_sexp(void *ctx_ptr, const uint8_t *src, uint32_t len) {
  sexp ctx = (sexp)ctx_ptr;
  
  // Lex
  RawToken *raw = malloc(sizeof(RawToken) * 4096);
  uint32_t total = 0;
  uint32_t pos = 0;
  while (1) {
    RawToken t = lex_one_token_from_mem(src, &pos, len);
    if (t.kind == T_EOF) break;
    raw[total++] = t;
  }
  
  // Group
  uint32_t idx = 0;
  L1Token *root = group_tokens_recursive(raw, total, &idx, GRP_ROOT);
  
  // Convert to sexp
  sexp result = token_to_sexp(ctx, root);
  
  free(raw);
  return result;
}


// ══════════════════════════════════════════════════════════════════════════════
// 7-8. L1 IR Builder FFI (→ l1_builder.c)
// ══════════════════════════════════════════════════════════════════════════════
#include "l1_builder.c"

// ══════════════════════════════════════════════════════════════════════════════
// 9. C Code Emission (→ l1_emit_c.c)
// ══════════════════════════════════════════════════════════════════════════════
#include "l1_emit_c.c"

// ══════════════════════════════════════════════════════════════════════════════
// 9.5. L1 IR Text Dump (→ l1_emit_text.c)
// ══════════════════════════════════════════════════════════════════════════════
#include "l1_emit_text.c"

// ══════════════════════════════════════════════════════════════════════════════
// 10. Syntax Cursor FFI
// ══════════════════════════════════════════════════════════════════════════════

// 10. Syntax Cursor FFI Functions
// ============================================================================

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


// ══════════════════════════════════════════════════════════════════════════════
// 11. Scheme Initialization + FFI Registration
// ══════════════════════════════════════════════════════════════════════════════

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
                     "(begin (define unit #f) (define (meta-source x) #f)"
                     " (define *error-count* 0))");

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
      ctx, env,
      "(define type.product (lambda (types) (core.make-product-type! types)))");
  native_eval_string(
      ctx, env,
      "(define type.product-field-type (lambda (product field-name)"
      "  (core.struct-field-type-from-type product field-name)))");
  native_eval_string(ctx, env, "(define type.product-field-types (lambda (product) '()))");
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

  // 15. Syntax cursor high-level wrappers — NOW DEFINED IN tree.scm
  // (Removed from C side; pure Scheme tree API replaces cursor FFI)

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
  // Load the single driver entry point instead of maintaining a duplicate
  // hardcoded file list. driver.scm has the authoritative load order.
  const char *search_paths[] = {"std/meta/driver.scm", "../std/meta/driver.scm",
                                "../../std/meta/driver.scm", NULL};
  for (int si = 0; search_paths[si]; si++) {
    FILE *test = fopen(search_paths[si], "r");
    if (test) {
      fclose(test);
      native_load_file(ctx, env, search_paths[si]);
      return;
    }
  }
  fprintf(stderr, "Warning: could not find std/meta/driver.scm\n");
}

// ============================================================================

// ══════════════════════════════════════════════════════════════════════════════
// 12. Top-Level API (called from Lain via @foreign)
// ══════════════════════════════════════════════════════════════════════════════

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
  REG("core.make-product-type!", 1, sexp_core_make_product_type);
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
  REG("core.type-is-void!", 1, sexp_core_type_is_void);
  REG("core.type-size-in-bytes!", 1, sexp_core_type_size);
  REG("core.field-offset!", 4, sexp_core_field_offset);
  REG("core.aggregate-layout!", 3, sexp_core_aggregate_layout);
  REG("core.call-indirect!", 4, sexp_core_call_indirect);
  REG("core.declare-extern-function!", 4, sexp_core_declare_extern_function);
  REG("core.call-expr!", 3, sexp_core_call_expr);
  REG("core.set-current-block!", 1, sexp_core_set_current_block);
  REG("core.get-current-block", 0, sexp_core_get_current_block);
  REG("core.begin-if!", 2, sexp_core_begin_if);
  REG("core.end-if!", 4, sexp_core_end_if);
  REG("core.assign-temp!", 2, sexp_core_assign_temp);
  REG("core.emit-l1!", 2, sexp_core_emit_l1);
  REG("core.lex-to-sexp!", 2, sexp_lex_to_sexp);

  // Register syntax cursor FFI functions (raw versions only)
  // High-level wrappers (syntax.cursor-*) are defined in tree.scm
  // syntax.group-cursor and syntax.group-kind are also defined in tree.scm
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

  // bootstrap_driver.scm is already loaded by driver.scm (via native_load_meta_sources).
  // Smoke test: lex-to-sexp produces correct S-expression
  {
    const char *test_src = "fn main() -> i32 { 42 }";
    sexp tree = (sexp)native_lex_to_sexp(ctx, (const uint8_t *)test_src, strlen(test_src));
    fprintf(stderr, "[init] 6: lex-to-sexp smoke test: %s\n",
            sexp_exceptionp(tree) ? "FAILED" : "OK");
    if (!sexp_exceptionp(tree)) {
      sexp out = sexp_open_output_string(ctx);
      sexp_write(ctx, tree, out);
      sexp str = sexp_get_output_string(ctx, out);
      fprintf(stderr, "[init] 6:   => %s\n", sexp_string_data(str));
      sexp_close_port(ctx, out);
    }
  }
  // Just verify the key entry point is available.
  {
    sexp sym = sexp_intern(ctx, "compile-group-to-core", -1);
    sexp val = sexp_env_ref(ctx, env, sym, SEXP_FALSE);
    fprintf(stderr, "[init] 6a: compile-group-to-core %s\n",
            val == SEXP_FALSE      ? "MISSING"
            : sexp_procedurep(val) ? "is proc"
                                   : "is defined but not proc");
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

  // Split root group into individual forms (C-side, handles ; separators)
  uint32_t form_count = 0;
  L1Token **form_groups = split_root_group((L1Token *)root_group, &form_count);

  sexp compile_sym = sexp_intern(ctx, "compile-group-to-core", -1);
  sexp proc = sexp_env_ref(ctx, env, compile_sym, SEXP_FALSE);

  for (uint32_t fi = 0; fi < form_count; fi++) {
    // Convert C token tree to Scheme S-expression tree
    sexp form_tree = token_to_sexp(ctx, form_groups[fi]);
    sexp result = sexp_apply(ctx, proc, sexp_cons(ctx, form_tree, SEXP_NULL));

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
  fprintf(out,
          "void native_push_token(const uint8_t *src, int32_t kind,"
          " size_t start, size_t len, int64_t int_val);\n");
  fprintf(out, "void *native_finish_grouping(void);\n");
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
