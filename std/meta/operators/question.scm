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
         (function (core.block-function block))
         (cont-block (core.append-block! function))
         (err-block (core.append-block! function))
         (flag-zero (core.const-bits! block flag-ty 0))
         (is-ok (core.primitive! block '|integer.eq| (list flag-val flag-zero) (core.make-bits 1))))
    (core.cond-branch! block is-ok cont-block err-block)
    ;; Error path: return the product (propagate error)
    (core.set-current-block! err-block)
    (core.return-value! err-block call-expr)
    ;; Ok path: extract value field
    (core.set-current-block! cont-block)
    (let* ((value-ty (core.product-value-type expected-ty))
           (value-val (core.field-offset! cont-block call-expr value-offset value-ty)))
      value-val)))
