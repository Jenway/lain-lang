(meta-source "control/infer")

;; ═══════════════════════════════════════════════════════════
;; 控制流 — 表达式类型推导
;; ═══════════════════════════════════════════════════════════

(define-pass (core-expr-inferer |control.if| expr locals)
  (type.unsupported '|if-expression-type|))
