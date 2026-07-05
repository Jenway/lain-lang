// lain_ast_parser.c — 纯拓扑 Pratt 解析器
//
// 零语义，零关键字，零定制拼装。
// 只出 5 种拓扑节点：ATOM / INFIX / PREFIX / POSTFIX / GROUP
//

#include "lain_ast.h"
#include <ctype.h>
#include <string.h>

// ── Token 类型 (无语义，纯词法) ────────────────────────────────────────

typedef enum {
  TOK_EOF,
  TOK_IDENT,
  TOK_INT,
  TOK_FLOAT,
  TOK_STRING,
  TOK_CHAR,
  TOK_LPAREN,       // (
  TOK_RPAREN,       // )
  TOK_LBRACKET,     // [
  TOK_RBRACKET,     // ]
  TOK_LBRACE,       // {
  TOK_RBRACE,       // }
  TOK_SEMICOLON,    // ;
  TOK_COMMA,        // ,
  TOK_COLON,        // :
  TOK_COLON_COLON,  // ::
  TOK_DOT,          // .
  TOK_ARROW,        // ->
  TOK_FAT_ARROW,    // =>
  TOK_HASH,         // #
  TOK_AT,           // @
  TOK_DOLLAR,       // $
  TOK_PLUS,         // +
  TOK_MINUS,        // -
  TOK_STAR,         // *
  TOK_SLASH,        // /
  TOK_PERCENT,      // %
  TOK_EQ,           // ==
  TOK_NE,           // !=
  TOK_LT,           // <
  TOK_GT,           // >
  TOK_LE,           // <=
  TOK_GE,           // >=
  TOK_AND_AND,      // &&
  TOK_OR_OR,        // ||
  TOK_AMP,          // &
  TOK_PIPE,         // |
  TOK_CARET,        // ^
  TOK_TILDE,        // ~
  TOK_BANG,         // !
  TOK_ASSIGN,       // =
  TOK_PLUS_ASSIGN,  // +=
  TOK_MINUS_ASSIGN, // -=
  TOK_STAR_ASSIGN,  // *=
  TOK_SLASH_ASSIGN, // /=
  TOK_PLUS_PLUS,    // ++
  TOK_MINUS_MINUS,  // --
} TokenType;

typedef struct {
  TokenType type;
  const char *start;
  uint32_t length;
  uint32_t line;
  uint32_t col;
} Token;

// ── 词法分析器 (无语义, 无关键字识别) ──────────────────────────────────

typedef struct {
  const char *src;
  uint32_t len;
  uint32_t pos;
  uint32_t line;
  uint32_t col;
} Lexer;

static char lexer_peek(const Lexer *l) {
  return l->pos < l->len ? l->src[l->pos] : '\0';
}

static char lexer_advance(Lexer *l) {
  char c = l->pos < l->len ? l->src[l->pos] : '\0';
  if (c == '\n') {
    l->line++;
    l->col = 1;
  } else {
    l->col++;
  }
  l->pos++;
  return c;
}

static void lexer_skip_whitespace_and_comments(Lexer *l) {
  while (l->pos < l->len) {
    char c = lexer_peek(l);
    if (isspace(c)) {
      lexer_advance(l);
    } else if (c == '/' && l->pos + 1 < l->len && l->src[l->pos + 1] == '/') {
      while (l->pos < l->len && lexer_peek(l) != '\n')
        lexer_advance(l);
    } else if (c == '/' && l->pos + 1 < l->len && l->src[l->pos + 1] == '*') {
      lexer_advance(l);
      lexer_advance(l);
      while (l->pos < l->len) {
        if (lexer_peek(l) == '*' && l->pos + 1 < l->len &&
            l->src[l->pos + 1] == '/') {
          lexer_advance(l);
          lexer_advance(l);
          break;
        }
        lexer_advance(l);
      }
    } else
      break;
  }
}

static Token lexer_next(Lexer *l) {
  lexer_skip_whitespace_and_comments(l);
  Token t;
  t.start = l->src + l->pos;
  t.line = l->line;
  t.col = l->col;

  if (l->pos >= l->len) {
    t.type = TOK_EOF;
    t.length = 0;
    return t;
  }

  char c = lexer_advance(l);

  if (c == '"') {
    while (l->pos < l->len && lexer_peek(l) != '"') {
      if (lexer_peek(l) == '\\')
        lexer_advance(l);
      lexer_advance(l);
    }
    if (l->pos < l->len)
      lexer_advance(l);
    t.type = TOK_STRING;
    t.length = (uint32_t)(l->src + l->pos - t.start);
    return t;
  }

  if (c == '\'') {
    if (lexer_peek(l) == '\\')
      lexer_advance(l);
    lexer_advance(l);
    if (l->pos < l->len)
      lexer_advance(l);
    t.type = TOK_CHAR;
    t.length = (uint32_t)(l->src + l->pos - t.start);
    return t;
  }

  if (isdigit(c)) {
    while (l->pos < l->len && (isdigit(lexer_peek(l)) || lexer_peek(l) == '.' ||
                               lexer_peek(l) == '_'))
      lexer_advance(l);
    t.type =
        (t.start[0] == '0' && (t.start[1] == 'x' || t.start[1] == 'X'))
            ? TOK_INT
            : (memchr(t.start, '.', l->pos - l->pos) ? TOK_FLOAT : TOK_INT);
    t.length = (uint32_t)(l->src + l->pos - t.start);
    return t;
  }

  // 标识符 — 无关键字识别，全部归为 TOK_IDENT
  // 注意: @ 和 $ 不作为 ident 起始，由下面 switch 处理为独立 punct
  if (isalpha(c) || c == '_') {
    while (l->pos < l->len && (isalnum(lexer_peek(l)) || lexer_peek(l) == '_'))
      lexer_advance(l);
    t.type = TOK_IDENT;
    t.length = (uint32_t)(l->src + l->pos - t.start);
    return t;
  }

  t.length = 1;
  switch (c) {
  case '(':
    t.type = TOK_LPAREN;
    break;
  case ')':
    t.type = TOK_RPAREN;
    break;
  case '[':
    t.type = TOK_LBRACKET;
    break;
  case ']':
    t.type = TOK_RBRACKET;
    break;
  case '{':
    t.type = TOK_LBRACE;
    break;
  case '}':
    t.type = TOK_RBRACE;
    break;
  case ';':
    t.type = TOK_SEMICOLON;
    break;
  case ',':
    t.type = TOK_COMMA;
    break;
  case ':':
    if (l->pos < l->len && lexer_peek(l) == ':') {
      lexer_advance(l);
      t.type = TOK_COLON_COLON;
      t.length = 2;
    } else {
      t.type = TOK_COLON;
    }
    break;
  case '.':
    t.type = TOK_DOT;
    break;
  case '#':
    t.type = TOK_HASH;
    break;
  case '@':
    t.type = TOK_AT;
    break;
  case '$':
    t.type = TOK_DOLLAR;
    break;
  case '~':
    t.type = TOK_TILDE;
    break;
  case '+':
    if (l->pos < l->len && lexer_peek(l) == '+') {
      lexer_advance(l);
      t.type = TOK_PLUS_PLUS;
      t.length = 2;
    } else if (l->pos < l->len && lexer_peek(l) == '=') {
      lexer_advance(l);
      t.type = TOK_PLUS_ASSIGN;
      t.length = 2;
    } else
      t.type = TOK_PLUS;
    break;
  case '-':
    if (l->pos < l->len && lexer_peek(l) == '>') {
      lexer_advance(l);
      t.type = TOK_ARROW;
      t.length = 2;
    } else if (l->pos < l->len && lexer_peek(l) == '-') {
      lexer_advance(l);
      t.type = TOK_MINUS_MINUS;
      t.length = 2;
    } else if (l->pos < l->len && lexer_peek(l) == '=') {
      lexer_advance(l);
      t.type = TOK_MINUS_ASSIGN;
      t.length = 2;
    } else
      t.type = TOK_MINUS;
    break;
  case '*':
    if (l->pos < l->len && lexer_peek(l) == '=') {
      lexer_advance(l);
      t.type = TOK_STAR_ASSIGN;
      t.length = 2;
    } else
      t.type = TOK_STAR;
    break;
  case '/':
    if (l->pos < l->len && lexer_peek(l) == '=') {
      lexer_advance(l);
      t.type = TOK_SLASH_ASSIGN;
      t.length = 2;
    } else
      t.type = TOK_SLASH;
    break;
  case '%':
    t.type = TOK_PERCENT;
    break;
  case '=':
    if (l->pos < l->len && lexer_peek(l) == '=') {
      lexer_advance(l);
      t.type = TOK_EQ;
      t.length = 2;
    } else if (l->pos < l->len && lexer_peek(l) == '>') {
      lexer_advance(l);
      t.type = TOK_FAT_ARROW;
      t.length = 2;
    } else
      t.type = TOK_ASSIGN;
    break;
  case '!':
    if (l->pos < l->len && lexer_peek(l) == '=') {
      lexer_advance(l);
      t.type = TOK_NE;
      t.length = 2;
    } else
      t.type = TOK_BANG;
    break;
  case '<':
    if (l->pos < l->len && lexer_peek(l) == '=') {
      lexer_advance(l);
      t.type = TOK_LE;
      t.length = 2;
    } else
      t.type = TOK_LT;
    break;
  case '>':
    if (l->pos < l->len && lexer_peek(l) == '=') {
      lexer_advance(l);
      t.type = TOK_GE;
      t.length = 2;
    } else
      t.type = TOK_GT;
    break;
  case '&':
    if (l->pos < l->len && lexer_peek(l) == '&') {
      lexer_advance(l);
      t.type = TOK_AND_AND;
      t.length = 2;
    } else
      t.type = TOK_AMP;
    break;
  case '|':
    if (l->pos < l->len && lexer_peek(l) == '|') {
      lexer_advance(l);
      t.type = TOK_OR_OR;
      t.length = 2;
    } else
      t.type = TOK_PIPE;
    break;
  case '^':
    t.type = TOK_CARET;
    break;
  default:
    t.type = TOK_EOF;
    break;
  }
  return t;
}

// ── Pratt Parser ────────────────────────────────────────────────────────

// 并列 (juxtaposition) 的绑定力 — 最松，所有显式操作符都比它紧。
#define PREC_JUXT 1

// 显式中缀操作符的右绑定力（赋值类，右结合）
static int infix_rbp(TokenType t) {
  switch (t) {
  case TOK_ASSIGN:
  case TOK_PLUS_ASSIGN:
  case TOK_MINUS_ASSIGN:
  case TOK_STAR_ASSIGN:
  case TOK_SLASH_ASSIGN:
    return 2;
  default:
    return 0;
  }
}

// 显式中缀操作符的左绑定力
static int infix_lbp(TokenType t) {
  switch (t) {
  case TOK_OR_OR:
    return 3;
  case TOK_AND_AND:
    return 4;
  case TOK_PIPE:
    return 5;
  case TOK_CARET:
    return 6;
  case TOK_AMP:
    return 7;
  case TOK_EQ:
  case TOK_NE:
    return 8;
  case TOK_LT:
  case TOK_GT:
  case TOK_LE:
  case TOK_GE:
    return 9;
  case TOK_PLUS:
  case TOK_MINUS:
    return 10;
  case TOK_STAR:
  case TOK_SLASH:
  case TOK_PERCENT:
    return 11;
  case TOK_COLON:
    return 12;
  case TOK_COLON_COLON:
    return 13;
  case TOK_DOT:
    return 13;
  case TOK_ARROW:
    return 2; // -> 右结合
  case TOK_FAT_ARROW:
    return 2; // => match arm
  default:
    return 0;
  }
}

// 前缀操作符的绑定力
static int prefix_bp(TokenType t) {
  switch (t) {
  case TOK_MINUS:
  case TOK_BANG:
  case TOK_STAR:
  case TOK_AMP:
  case TOK_TILDE:
    return 14;
  default:
    return 0;
  }
}

// 是否是表达式起始 token
static int is_expr_start(TokenType t) {
  return t == TOK_IDENT || t == TOK_INT || t == TOK_FLOAT || t == TOK_STRING ||
         t == TOK_CHAR || t == TOK_LPAREN || t == TOK_LBRACKET ||
         t == TOK_LBRACE || t == TOK_MINUS || t == TOK_BANG || t == TOK_STAR ||
         t == TOK_AMP || t == TOK_TILDE || t == TOK_HASH || t == TOK_AT ||
         t == TOK_DOLLAR;
}

typedef struct {
  AstArena *arena;
  Lexer lexer;
  Token current;
  Token previous;
} PrattParser;

static void pratt_advance(PrattParser *p) {
  p->previous = p->current;
  p->current = lexer_next(&p->lexer);
}

static int pratt_match(PrattParser *p, TokenType expected) {
  if (p->current.type == expected) {
    pratt_advance(p);
    return 1;
  }
  return 0;
}

static AstNodeId pratt_make_atom(PrattParser *p, const Token *t) {
  AstNodeId id = ast_alloc(p->arena, AST_ATOM, t->line, t->col);
  ast_set_text(p->arena, id, ast_intern(p->arena, t->start, t->length));
  return id;
}

static AstNodeId pratt_parse_expr(PrattParser *p, int min_bp);

// 解析 GROUP: 存储定界符到 op 字段为 Atom
static AstNodeId pratt_parse_group(PrattParser *p, TokenType close,
                                   const char *delim_text) {
  uint32_t line = p->previous.line;
  uint32_t col = p->previous.col;
  AstNodeId group = ast_alloc(p->arena, AST_GROUP, line, col);

  // 存储定界符 (如 "(", "{", "[") 到 op 字段
  if (delim_text) {
    AstNodeId delim_atom = ast_alloc(p->arena, AST_ATOM, line, col);
    ast_set_text(p->arena, delim_atom, ast_intern(p->arena, delim_text, 1));
    ast_set_op(p->arena, group, delim_atom);
  }

  AstNodeId *next_slot = &p->arena->nodes[group].left;
  while (p->current.type != close && p->current.type != TOK_EOF) {
    AstNodeId child = pratt_parse_expr(p, 0);
    if (child != AST_NULL) {
      *next_slot = child;
      next_slot = &p->arena->nodes[child].next;
    }
    if (p->current.type == TOK_SEMICOLON || p->current.type == TOK_COMMA) {
      // 保留分号/逗号作为 ATOM token (旧 lexer 也是如此)
      Token tok = p->current;
      AstNodeId punct = ast_alloc(p->arena, AST_ATOM, tok.line, tok.col);
      ast_set_text(p->arena, punct,
                   ast_intern(p->arena, tok.start, tok.length));
      *next_slot = punct;
      next_slot = &p->arena->nodes[punct].next;
      pratt_advance(p);
    }
  }
  pratt_match(p, close);
  return group;
}

// 表达式解析 (Pratt 算法 + 并列)
static AstNodeId pratt_parse_expr(PrattParser *p, int min_bp) {
  AstNodeId left;

  // 前缀
  Token tok = p->current;
  switch (tok.type) {
  case TOK_INT:
  case TOK_FLOAT:
  case TOK_STRING:
  case TOK_CHAR:
  case TOK_IDENT:
  case TOK_HASH:
  case TOK_AT:
  case TOK_DOLLAR:
    pratt_advance(p);
    left = pratt_make_atom(p, &tok);
    break;

  case TOK_LPAREN:
    pratt_advance(p);
    left = pratt_parse_group(p, TOK_RPAREN, "(");
    break;

  case TOK_LBRACKET:
    pratt_advance(p);
    left = pratt_parse_group(p, TOK_RBRACKET, "[");
    break;

  case TOK_LBRACE:
    pratt_advance(p);
    left = pratt_parse_group(p, TOK_RBRACE, "{");
    break;

  case TOK_MINUS:
  case TOK_BANG:
  case TOK_STAR:
  case TOK_AMP:
  case TOK_TILDE: {
    pratt_advance(p);
    AstNodeId op_node = pratt_make_atom(p, &tok);
    int pbp = prefix_bp(tok.type);
    AstNodeId operand = pratt_parse_expr(p, pbp);
    left = ast_alloc(p->arena, AST_PREFIX, tok.line, tok.col);
    ast_set_op(p->arena, left, op_node);
    ast_set_left(p->arena, left, operand);
    break;
  }

  default:
    return AST_NULL;
  }

  // 中缀 / 后缀 / 并列 循环
  for (;;) {
    // ── 后缀操作符 (++, --, (, ., :) ──
    switch (p->current.type) {
    case TOK_PLUS_PLUS:
    case TOK_MINUS_MINUS: {
      Token op = p->current;
      pratt_advance(p);
      AstNodeId op_node = pratt_make_atom(p, &op);
      AstNodeId post = ast_alloc(p->arena, AST_POSTFIX, op.line, op.col);
      ast_set_op(p->arena, post, op_node);
      ast_set_left(p->arena, post, left);
      left = post;
      continue;
    }

    case TOK_LPAREN: {
      // func(args)
      Token lp = p->current;
      pratt_advance(p);
      AstNodeId call = ast_alloc(p->arena, AST_POSTFIX, lp.line, lp.col);
      AstNodeId op_node = pratt_make_atom(p, &lp);
      ast_set_op(p->arena, call, op_node);
      ast_set_left(p->arena, call, left);
      AstNodeId args = pratt_parse_group(p, TOK_RPAREN, "(");
      ast_set_right(p->arena, call, args);
      left = call;
      continue;
    }

    case TOK_DOT: {
      // member access
      Token dot = p->current;
      pratt_advance(p);
      AstNodeId dot_node = pratt_make_atom(p, &dot);
      int lbp = infix_lbp(dot.type);
      if (lbp > min_bp) {
        AstNodeId right = pratt_parse_expr(p, lbp);
        AstNodeId inf = ast_alloc(p->arena, AST_INFIX, dot.line, dot.col);
        ast_set_op(p->arena, inf, dot_node);
        ast_set_left(p->arena, inf, left);
        ast_set_right(p->arena, inf, right);
        left = inf;
        continue;
      }
      break;
    }

    case TOK_COLON: {
      // 类型标注: expr : type
      Token col = p->current;
      pratt_advance(p);
      AstNodeId col_node = pratt_make_atom(p, &col);
      int lbp = infix_lbp(col.type);
      if (lbp > min_bp) {
        AstNodeId right = pratt_parse_expr(p, lbp);
        AstNodeId inf = ast_alloc(p->arena, AST_INFIX, col.line, col.col);
        ast_set_op(p->arena, inf, col_node);
        ast_set_left(p->arena, inf, left);
        ast_set_right(p->arena, inf, right);
        left = inf;
        continue;
      }
      break;
    }

    default:
      break;
    }

    // ── 并列 (Juxtaposition) ──
    // 下一个 token 可以开始表达式（不是显式操作符）→ 隐式 INFIX(" ")
    // 约束: 不跨越行边界（基于上一个消耗 token 的行号，而非 left 节点起始行号）
    // 这样 `if flag { ... } else { ... }` 中的 else 可以接续在 `}` 同行上
    if (is_expr_start(p->current.type) && PREC_JUXT > min_bp) {
      Token line_tok = p->current;
      // 如果下一 token 在上一个消耗 token 的行之后，不并列
      if (line_tok.line > p->previous.line) {
        break;
      }
      AstNodeId right = pratt_parse_expr(p, PREC_JUXT);
      AstNodeId inf =
          ast_alloc(p->arena, AST_INFIX, line_tok.line, line_tok.col);
      AstNodeId space_atom =
          ast_alloc(p->arena, AST_ATOM, line_tok.line, line_tok.col);
      ast_set_text(p->arena, space_atom, ast_intern(p->arena, " ", 1));
      ast_set_op(p->arena, inf, space_atom);
      ast_set_left(p->arena, inf, left);
      ast_set_right(p->arena, inf, right);
      left = inf;
      continue;
    }

    // ── 显式中缀操作符 ──
    {
      Token op = p->current;
      int rbp = infix_rbp(op.type);
      int lbp = infix_lbp(op.type);

      // 右结合 (rbp > 0) 用于 =, +=, -=, *=, /=
      if (rbp > 0 && min_bp <= rbp) {
        pratt_advance(p);
        AstNodeId op_node = pratt_make_atom(p, &op);
        AstNodeId right = pratt_parse_expr(p, rbp);
        AstNodeId inf = ast_alloc(p->arena, AST_INFIX, op.line, op.col);
        ast_set_op(p->arena, inf, op_node);
        ast_set_left(p->arena, inf, left);
        ast_set_right(p->arena, inf, right);
        left = inf;
        continue;
      }

      // 左结合
      if (lbp > 0 && min_bp < lbp) {
        pratt_advance(p);
        AstNodeId op_node = pratt_make_atom(p, &op);
        AstNodeId right = pratt_parse_expr(p, lbp);
        AstNodeId inf = ast_alloc(p->arena, AST_INFIX, op.line, op.col);
        ast_set_op(p->arena, inf, op_node);
        ast_set_left(p->arena, inf, left);
        ast_set_right(p->arena, inf, right);
        left = inf;
        continue;
      }
    }

    break;
  }

  return left;
}

// ── 公共 API ────────────────────────────────────────────────────────────

AstNodeId ast_parse(AstArena *arena, const char *src, uint32_t len) {
  PrattParser p;
  p.arena = arena;
  p.lexer.src = src;
  p.lexer.len = len;
  p.lexer.pos = 0;
  p.lexer.line = 1;
  p.lexer.col = 1;
  p.current = lexer_next(&p.lexer);
  p.previous.type = TOK_EOF;
  p.previous.line = 1;

  // 顶层: 分号分隔的表达式序列
  AstNodeId root = ast_alloc(arena, AST_GROUP, 1, 1);
  AstNodeId *next_slot = &arena->nodes[root].left;

  while (p.current.type != TOK_EOF) {
    AstNodeId expr = pratt_parse_expr(&p, 0);
    if (expr != AST_NULL) {
      *next_slot = expr;
      next_slot = &arena->nodes[expr].next;
    } else {
      pratt_advance(&p);
    }
    pratt_match(&p, TOK_SEMICOLON);
  }

  return root;
}
