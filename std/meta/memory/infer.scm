(meta-source "memory/infer")

;; ═══════════════════════════════════════════════════════════
;; 内存/指针 — 表达式类型推导
;; ═══════════════════════════════════════════════════════════

(define-pass (core-expr-inferer |middle.expr.borrow| expr locals)
  (type.unsupported '|borrow-expression-type|))
