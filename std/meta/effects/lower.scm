(meta-source "effects/lower")

;; ═══════════════════════════════════════════════════════════
;; 效应系统 — 表达式降级 (perform / resume / handle)
;; ═══════════════════════════════════════════════════════════

;; ── Helper: extract the argument expression from a perform call ──
(define (perform.extract-error-expr perform-expr)
  ;; perform-expr is |middle.expr.perform| with |call| field
  ;; The call is Throws::throw(code) — extract the first argument
  (let* ((payload (middle.payload perform-expr))
         (call (optional.value (record.get payload '|call|)))
         (call-payload (middle.payload call))
         (args (optional.value (record.get call-payload '|args|))))
    (if (list.empty? args)
        #f
        (list.first args))))

;; ── Helper: check if expr is an effect expression ──
(define (core.is-effect-expr? expr)
  (let* ((kind (middle.kind expr)))
    (or (symbol=? kind '|middle.expr.perform|)
        (symbol=? kind '|middle.expr.resume|)
        (symbol=? kind '|middle.expr.handle|)
        (symbol=? kind '|middle.expr.question|))))

;; ── perform: Throws::throw(code) → error product {flag:1, value:0, error:code} ──
(define-pass (core-expr-lowerer |middle.expr.perform| block expr expected-ty locals)
  ;; expected-ty is the throws product type (or value type)
  ;; Build error aggregate: {flag: 1, value: 0, error: error_code}
  ;; Field types are always: i8 flag, i32 value, i32 error — layout computed in Scheme
  (let* ((error-expr (perform.extract-error-expr expr)))
    (if (not error-expr)
        (core.unsupported-expr '|perform-without-args|)
        (let* ((flag-ty (core.make-bits 8))
               (error-code (core.lower-expr block error-expr (core.make-bits 32) locals))
               (flag-one (core.const-bits! block flag-ty 1))
               (value-zero (core.const-bits! block (core.make-bits 32) 0))
               (field-types (list flag-ty (core.make-bits 32) (core.make-bits 32)))
               (layout (product-layout field-types))
               (total-size (car layout))
               (offsets (cdr layout))
               (field-values (list flag-one value-zero error-code))
               (pairs (map cons (map car offsets) field-values)))
          (core.aggregate-layout! block total-size pairs)))))

(define-pass (core-expr-lowerer |middle.expr.resume| block expr expected-ty locals)
  (core.unsupported-expr '|resume-expression|))

(define-pass (core-expr-lowerer |middle.expr.handle| block expr expected-ty locals)
  (core.unsupported-expr '|handle-expression|))
