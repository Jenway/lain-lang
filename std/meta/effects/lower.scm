(meta-source "effects/lower")

;; ═══════════════════════════════════════════════════════════
;; 效应系统 — 表达式降级 (perform / resume / handle)
;; Phase 1: Unified product layout via effects/layout.scm
;; ═══════════════════════════════════════════════════════════

;; ── Helper: extract the effect name, operation name, and args ──
(define (perform.extract-effect-info perform-expr)
  (let* ((payload (middle.payload perform-expr))
         (call (optional.value (record.get payload '|call|)))
         (call-payload (middle.payload call))
         (callee (optional.value (record.get call-payload '|callee|)))
         (args (optional.value (record.get call-payload '|args|)))
         (callee-payload (middle.payload callee))
         (path (optional.value (record.get callee-payload '|path|))))
    (if (or (not path) (< (length path) 2))
        #f
        (let* ((effect-name (list.first path))
               (op-name (list.first (list.rest path))))
          (list effect-name op-name args)))))

;; ── Helper: check if expr is an effect expression ──
(define (core.is-effect-expr? expr)
  (let* ((kind (middle.kind expr)))
    (or (symbol=? kind '|middle.expr.perform|)
        (symbol=? kind '|middle.expr.resume|)
        (symbol=? kind '|middle.expr.handle|)
        (symbol=? kind '|middle.expr.question|))))

;; ── Helper: check if index is in a list (manual member) ──
(define (effect.index-in-list? idx lst)
  (if (null? lst)
      #f
      (if (= idx (car lst))
          #t
          (effect.index-in-list? idx (cdr lst)))))

;; ── Helper: get the position of index in list (0-based) ──
(define (effect.index-position idx lst)
  (let loop ((remaining lst) (pos 0))
    (if (null? remaining)
        -1
        (if (= idx (car remaining))
            pos
            (loop (cdr remaining) (+ pos 1))))))

;; ── perform lowering: unified via effect.lookup-layout ──
(define-pass (core-expr-lowerer |middle.expr.perform| block expr expected-ty locals)
  (let* ((info (perform.extract-effect-info expr)))
    (if (not info)
        (core.unsupported-expr '|perform-bad-format|)
        (let* ((effect-name (car info))
               (op-name (cadr info))
               (args (caddr info))
               ;; Record this effect for propagation checking
               (_ (propagate.record! effect-name))
               (layout (effect.lookup-layout effect-name op-name)))
          (if (not layout)
              (core.unsupported-expr '|perform-unknown-effect|)
              (let* ((total-size (car layout))
                     (offsets    (cadr layout))
                     (flag-index (caddr layout))
                     (arg-indices (cadddr layout))
                     (num-fields (length offsets)))
                ;; Build field values in order
                (let build ((i 0) (field-pairs '()))
                  (if (>= i num-fields)
                      (core.aggregate-layout! block total-size (reverse field-pairs))
                      (let* ((offset-type (list-ref offsets i))
                             (offset (car offset-type))
                             (field-ty (cdr offset-type))
                             (val
                              (if (= i flag-index)
                                  ;; Flag field: set to 1
                                  (core.const-bits! block field-ty 1)
                                  (let* ((arg-pos (effect.index-position i arg-indices)))
                                    (if (>= arg-pos 0)
                                        ;; This field holds an operation argument
                                        (core.lower-expr block
                                          (list-ref args arg-pos)
                                          field-ty locals)
                                        ;; Plain field: zero
                                        (core.const-bits! block field-ty 0))))))
                        (build (+ i 1) (cons (cons offset val) field-pairs)))))))))))

(define-pass (core-expr-lowerer |middle.expr.resume| block expr expected-ty locals)
  (core.unsupported-expr '|resume-expression|))

(define-pass (core-expr-lowerer |middle.expr.handle| block expr expected-ty locals)
  (core.unsupported-expr '|handle-expression|))
