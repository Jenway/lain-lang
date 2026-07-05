(meta-source "effects/infer")

;; ═══════════════════════════════════════════════════════════
;; 效应系统 — 表达式类型推导 (perform / resume / handle)
;; 当前均为 unsupported stub
;; ═══════════════════════════════════════════════════════════

(define-pass* 'core-expr-inferer '|effects.perform| (lambda (expr locals)
  (type.unsupported '|perform-expression-type|)))

(define-pass* 'core-expr-inferer '|effects.resume| (lambda (expr locals)
  (type.unsupported '|resume-expression-type|)))

(define-pass* 'core-expr-inferer '|effects.handle| (lambda (expr locals)
  (type.unsupported '|handle-expression-type|)))
