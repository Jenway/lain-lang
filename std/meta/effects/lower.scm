(meta-source "effects/lower")

;; ═══════════════════════════════════════════════════════════
;; 效应系统 — 表达式降级 (perform / resume / handle)
;; 当前均为 unsupported stub
;; ═══════════════════════════════════════════════════════════

(define-pass (core-expr-lowerer |middle.expr.perform| block expr expected-ty locals)
  (core.unsupported-expr '|perform-expression|))

(define-pass (core-expr-lowerer |middle.expr.resume| block expr expected-ty locals)
  (core.unsupported-expr '|resume-expression|))

(define-pass (core-expr-lowerer |middle.expr.handle| block expr expected-ty locals)
  (core.unsupported-expr '|handle-expression|))
