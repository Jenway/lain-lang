// lainir/core.h

#ifndef LAINIR_CORE_H
#define LAINIR_CORE_H

#include <stddef.h>
#include <stdint.h>

/*
 * # Types
 *
 *  BITS    : integer register
 *  FLOATS  : float register
 *  SIMD    : SIMD register
 *  ADDR    : pointer / address
 *  UNIT    : no meaningful value (void)
 *  NEVER   : diverging (no return)
 */
typedef enum {
  TY_BITS,
  TY_FLOATS,
  TY_SIMD,
  TY_ADDR,
  TY_UNIT,
  TY_NEVER,
} L1TypeKind;

typedef struct L1Type {
  L1TypeKind kind;
  uint32_t width;
} L1Type;

/* -------------------------------------------------------------------------
 * Instructions
 * -------------------------------------------------------------------------
 *
 *  LET     : immutable binding  — let name = expr
 *  SET     : mutable assignment — set name = expr
 *  STORE   : memory write       — store val -> addr
 *  IF      : conditional        — if cond { then } [else { else }]
 *  LOOP    : infinite loop      — loop { body }  (exit via BREAK / RETURN)
 *  BREAK   : exit innermost (or labelled) loop
 *  CONTINUE: next iteration of innermost (or labelled) loop
 *  RETURN  : return from subroutine
 *  CALL    : call expression used as a statement (for side-effects)
 */
typedef enum {
  INST_LET,
  INST_SET,
  INST_STORE,
  INST_IF,
  INST_LOOP,
  INST_BREAK,
  INST_CONTINUE,
  INST_RETURN,
  INST_CALL,
} L1InstKind;

/* -------------------------------------------------------------------------
 * Memory Ordering
 * ------------------------------------------------------------------------- */
typedef enum {
  MEM_ORDER_RELAXED,
  MEM_ORDER_ACQUIRE,
  MEM_ORDER_RELEASE,
  MEM_ORDER_ACQREL,
  MEM_ORDER_SEQCST,
} L1MemOrder;

/* -------------------------------------------------------------------------
 * Expressions
 * -------------------------------------------------------------------------
 *
 *  VAR           : reference to a named variable / binding
 *  CONST         : integer constant
 *  ARG           : reference to a subroutine parameter by index
 *  LOAD          : memory read
 *  LEA           : address calculation  base + idx*scale + offset
 *  ADD / SUB     : arithmetic (other ops go through PRIMITIVE)
 *  MUL / DIV     : multiplication / division
 *  EQ / NE       : equality comparison
 *  LT / LE / GT / GE : ordering comparison
 *  POPCOUNT / CLZ / ROTL : bit operations
 *  INT2PTR / PTR2INT : type conversion
 *  FADD / FSUB / FMUL / FDIV : float arithmetic
 *  FEQ / FLT     : float comparison
 *  CALL          : direct call, yields a value
 *  CALL_INDIRECT : indirect call through a function pointer
 *  STRING        : string literal (address of read-only data)
 *  PRIMITIVE     : backend-specific / target intrinsic opcode
 *  ALLOCA        : stack allocation, yields an ADDR
 *  FIELD         : struct field access (sugar over LEA; lowerable)
 *  EVAL          : compile-time call , yields a runtime constant or UNIT
 */
typedef enum {
  EXPR_VAR,
  EXPR_CONST,
  EXPR_ARG,
  EXPR_LOAD,
  EXPR_LEA,
  EXPR_ADD,
  EXPR_SUB,
  EXPR_MUL,
  EXPR_DIV,
  EXPR_EQ,
  EXPR_NE,
  EXPR_LT,
  EXPR_LE,
  EXPR_GT,
  EXPR_GE,
  EXPR_POPCOUNT,
  EXPR_CLZ,
  EXPR_ROTL,
  EXPR_INT2PTR,
  EXPR_PTR2INT,
  EXPR_FADD,
  EXPR_FSUB,
  EXPR_FMUL,
  EXPR_FDIV,
  EXPR_FEQ,
  EXPR_FLT,
  EXPR_CALL,
  EXPR_CALL_INDIRECT,
  EXPR_STRING,
  EXPR_PRIMITIVE,
  EXPR_ALLOCA,
  EXPR_FIELD,
  EXPR_EVAL,
} L1ExprKind;

typedef struct L1Expr L1Expr;
typedef struct L1Instruction L1Instruction;
typedef struct L1Block L1Block;
typedef struct L1Subroutine L1Subroutine;

typedef struct {
  int code;
  int line;
  int column;
  char message[192];
} L1Diagnostic;

/* -------------------------------------------------------------------------
 * Expression node
 * ------------------------------------------------------------------------- */
struct L1Expr {
  L1ExprKind kind;
  union {

    /* EXPR_VAR */
    struct {
      char *name;
      L1Type *ty;
    } var;

    /* EXPR_CONST */
    int64_t const_val;

    /* EXPR_ARG */
    struct {
      uint32_t index;
      L1Type *ty; /* resolved from the owning procedure signature */
    } arg;

    /* EXPR_LOAD */
    struct {
      L1Expr *addr;
      L1Type *ty;
      L1MemOrder ordering;
    } load;

    /* EXPR_LEA: addr = base + idx*scale + offset */
    struct {
      L1Expr *base;
      L1Expr *idx; /* may be NULL */
      uint32_t scale;
      uint32_t offset;
    } lea;

    /* EXPR_ADD, EXPR_SUB, EXPR_MUL, EXPR_DIV, EXPR_EQ, EXPR_NE,
       EXPR_LT, EXPR_LE, EXPR_GT, EXPR_GE, EXPR_FADD, EXPR_FSUB,
       EXPR_FMUL, EXPR_FDIV, EXPR_FEQ, EXPR_FLT */
    struct {
      L1Expr *left;
      L1Expr *right;
    } bin;

    /* EXPR_POPCOUNT, EXPR_CLZ, EXPR_ROTL, EXPR_INT2PTR, EXPR_PTR2INT */
    struct {
      L1Expr *operand;
    } unary;

    /* EXPR_CALL */
    struct {
      char *fn_name;
      L1Expr **args;
      uint32_t arg_count;
      L1Type *ret_ty;
    } call;

    /* EXPR_CALL_INDIRECT */
    struct {
      L1Expr *fn_ptr;
      L1Type *ret_ty;
      L1Type **param_tys;
      uint32_t param_count;
      L1Expr **args;
      uint32_t arg_count;
    } call_indirect;

    /* EXPR_STRING */
    struct {
      char *content;
      L1Type *ty; /* TY_ADDR */
    } str_val;

    /* EXPR_PRIMITIVE: target-specific opcode, arbitrary operands */
    struct {
      char *opcode;
      L1Expr **operands;
      uint32_t operand_count;
      L1Type *result_ty;
    } primitive;

    /* EXPR_ALLOCA */
    struct {
      L1Type *element_ty;
      uint32_t byte_size;
      L1Type *result_ty; /* always TY_ADDR */
    } alloca;

    /* EXPR_FIELD: sugar over LEA; lower before backend */
    struct {
      L1Expr *base;
      L1Type *struct_ty;
      uint32_t field_index;
      L1Type *field_ty;
    } field;

    /*
     * EXPR_EVAL: compile-time call.
     *
     */
    struct {
      char *fn_name;
      L1Expr **args;
      uint32_t arg_count;
      L1Type *ret_ty;
    } eval;

  } data;
};

/* -------------------------------------------------------------------------
 * Instruction node
 * ------------------------------------------------------------------------- */
struct L1Instruction {
  L1InstKind kind;
  union {

    /* INST_LET: immutable binding */
    struct {
      char *name;
      /* Declared result type.  Textual L1 bindings are typed; NULL is only
         retained while accepting legacy input, before verification infers it.
       */
      L1Type *ty;
      L1Expr *val;
    } let;

    /* INST_SET: mutable assignment */
    struct {
      char *name;
      L1Type *ty;
      L1Expr *val;
    } set;

    /* INST_STORE */
    struct {
      L1Expr *dest;
      L1Expr *val;
      L1Type *store_ty;
      L1MemOrder ordering;
    } store;

    /* INST_IF */
    struct {
      L1Expr *condition;
      L1Block *then_body;
      L1Block *else_body; /* NULL if no else branch */
    } if_stmt;

    /* INST_LOOP: body is an instruction sequence; exited by BREAK / RETURN */
    struct {
      L1Block *body;
      char *label; /* NULL for anonymous; used by BREAK / CONTINUE */
    } loop;

    /* INST_BREAK / INST_CONTINUE */
    struct {
      char *label; /* NULL = innermost loop */
    } jump;

    /* INST_RETURN */
    struct {
      L1Expr *val; /* NULL for UNIT return */
    } ret;

    /* INST_CALL: call used as a statement */
    struct {
      L1Expr *expr; /* must be EXPR_CALL or EXPR_CALL_INDIRECT */
    } call_inst;

  } data;
  L1Instruction *next;
};

/* -------------------------------------------------------------------------
 * Block: a flat, ordered sequence of instructions.
 * No id, no terminator — this is not a CFG basic block.
 * ------------------------------------------------------------------------- */
struct L1Block {
  L1Instruction *body;
  L1Instruction *body_tail;
  L1Subroutine *parent;
  struct L1Block *next;
};

/* -------------------------------------------------------------------------
 * Subroutine
 * ------------------------------------------------------------------------- */
struct L1Subroutine {
  char *name;
  char *link_name;
  L1Type *ret_ty;
  uint32_t param_count;
  L1Type **param_tys;
  L1Block *blocks;
  L1Block *blocks_tail;
  int is_extern;
  L1Subroutine *next;
};

/* -------------------------------------------------------------------------
 * Module-level name lists
 * ------------------------------------------------------------------------- */
typedef struct L1ExportName {
  char *name;
  struct L1ExportName *next;
} L1ExportName;

/* -------------------------------------------------------------------------
 * Global state
 * ------------------------------------------------------------------------- */
extern L1Subroutine *g_subroutines_head;
extern L1Subroutine *g_current_sub;
extern L1Block *g_current_block;
extern uint32_t g_temp_counter;
extern L1Block *g_scratch_blocks[8];
extern int g_scratch_count;
extern L1ExportName *g_export_names_head;
extern L1ExportName *g_declared_module_names_head;
extern L1ExportName *g_declared_signature_names_head;
extern int g_has_explicit_exports;

/* -------------------------------------------------------------------------
 * Constructors
 * ------------------------------------------------------------------------- */
L1Type *lainir_new_type(L1TypeKind kind, uint32_t width);
L1Expr *lainir_new_expr(L1ExprKind kind);
L1Instruction *lainir_new_instruction(L1InstKind kind);
L1Block *lainir_new_block(void);
L1Subroutine *lainir_new_subroutine(const char *name);

/* -------------------------------------------------------------------------
 * Module utilities
 * ------------------------------------------------------------------------- */
void lainir_reset_module_state(void);
void lainir_free_subroutines(L1Subroutine *head);

void append_instruction(L1Subroutine *sub, L1Instruction *inst);
void append_inst_to_block(L1Block *block, L1Instruction *inst);
L1Type *infer_expr_type(L1Expr *expr);

/* -------------------------------------------------------------------------
 * Native / export declarations
 * ------------------------------------------------------------------------- */
void native_declare_module(const char *name);
void native_declare_signature(const char *name);
void native_mark_export(const char *name);
int native_has_explicit_exports(void);
int native_is_export_marked(const char *name);

#endif /* LAINIR_CORE_H */
