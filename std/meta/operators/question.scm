(meta-source "operators/question")

(register-operator! '|?| '|throws-question-operator|)

;; ── ? 操作符 lowering: check flag → propagate or unwrap ──
;; Uses the Throws effect layout from effects/layout.scm.
;; Layout: {flag: u8, value: T, error: E} — flag at index 0, value at index 1.
(define-pass (core-expr-lowerer |operators.question| block expr expected-ty locals)
  (let* ((payload (middle.payload expr))
         (inner-expr (optional.value (record.get payload '|expr|)))
         (call-expr (core.lower-expr block inner-expr expected-ty locals))
         ;; Query Throws layout for flag field info
         (throws-layout (effect.lookup-layout '|Throws| '|throw|))
         (offsets    (if throws-layout (cadr throws-layout) #f))
         (flag-index (if throws-layout (caddr throws-layout) 0))
         ;; flag field at flag-index
         (flag-offset-ty (if offsets (list-ref offsets flag-index) (cons 0 (core.make-bits 8))))
         (flag-offset (car flag-offset-ty))
         (flag-ty     (cdr flag-offset-ty))
         ;; value field at index 1
         (value-offset-ty (if offsets (list-ref offsets 1) (cons 4 (core.make-bits 32))))
         (value-offset (car value-offset-ty))
         (flag-val (core.field-offset! block call-expr flag-offset flag-ty))
         (flag-zero (core.const-bits! block flag-ty 0))
         (is-ok (ir.expr.eq block flag-val flag-zero))
         ;; Structured if — create then/else blocks
         (pair (core.begin-if! block is-ok))
         (then-b (car pair))
         (else-b (cadr pair)))
    ;; Else path: return the product (propagate error)
    (core.set-current-block! else-b)
    (core.return-value! else-b call-expr)
    ;; Then path: extract value field
    (core.set-current-block! then-b)
    (let* ((value-ty (core.product-value-type expected-ty))
           (value-val (core.field-offset! then-b call-expr value-offset value-ty)))
      ;; Finalize if — append INST_IF to parent block
      (core.end-if! block is-ok then-b else-b)
      value-val)))
