(meta-source "memory/lower")

;; ═══════════════════════════════════════════════════════════
;; 内存/指针 — 类型降级 + borrow 表达式降级
;; ═══════════════════════════════════════════════════════════

;; ── 类型降级 ──

(define-pass (core-type-lowerer |middle.ty.ref| ty)
  (let* ((payload (middle.payload ty))
         (mutable (optional.value (record.get payload '|mutable|)))
         (inner (core.lower-type
                  (optional.value (record.get payload '|inner|)))))
    (type.addr)))

(define-pass (core-type-lowerer |middle.ty.slice| ty)
  (let* ((kind (middle.kind ty)))
    (type.unsupported kind)))

(define-pass (core-type-lowerer |middle.ty.array| ty)
  (let* ((payload (middle.payload ty))
         (element (core.lower-type
                    (optional.value (record.get payload '|element|))))
         (len (optional.value (record.get payload '|len|))))
    (type.array element len)))

;; ── borrow 表达式降级 ──

(define-pass (core-expr-lowerer |middle.expr.borrow| block expr expected-ty locals)
  (core.unsupported-expr '|borrow-expression|))
