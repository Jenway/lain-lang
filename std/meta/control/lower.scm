(meta-source "control/lower")

;; ── Helper: detect if ret-ty is a product type ──
(define (core.return-is-product? ret-ty)
  (let* ((fields (type.product-field-types ret-ty)))
    (not (null? fields))))

;; ── Helper: get the value field type from a throws product (field index 1) ──
(define (core.product-value-type ret-ty)
  (let* ((fields (type.product-field-types ret-ty)))
    (if (and (not (null? fields)) (not (null? (cdr fields))))
        (car (cdr fields))
        ret-ty)))

;; ── Helper: get field type by index ──
(define (core.product-field-type-at ret-ty idx)
  (let* ((fields (type.product-field-types ret-ty))
         (len (length fields)))
    (if (< idx len) (list-ref fields idx) ret-ty)))

;; ── Helper: wrap a value as throws aggregate {flag:0, value, error:0} ──
(define (core.wrap-throws-return block ret-ty value-expr)
  (let* ((flag-ty (core.product-field-type-at ret-ty 0))
         (error-ty (core.product-field-type-at ret-ty 2))
         (flag-zero (core.const-bits! block flag-ty 0))
         (error-zero (core.const-bits! block error-ty 0)))
    (core.aggregate! block ret-ty (list flag-zero value-expr error-zero))))

;; ── 语句降级: pipeline stage = core-stmt-lowerer ──

(define-pass (core-stmt-lowerer |middle.stmt.return| block stmt ret-ty locals)
  (let* ((payload (middle.payload stmt)) (value (optional.value (record.get payload '|value|))))
    (if (optional.none? value) (core.return-none! block)
        (let* ((inner-ty (if (core.return-is-product? ret-ty)
                             (core.product-value-type ret-ty)
                             ret-ty))
               (lowered (core.lower-expr block (optional.value value) inner-ty locals))
               (wrapped (if (core.return-is-product? ret-ty)
                            (core.wrap-throws-return block ret-ty lowered)
                            lowered)))
          (core.return-value! block wrapped)))
    locals))

(define-pass (core-stmt-lowerer |middle.stmt.tail| block stmt ret-ty locals)
  (let* ((payload (middle.payload stmt)) (expr (optional.value (record.get payload '|expr|))))
    (cond
      ((symbol=? (middle.kind expr) '|middle.expr.if|)
       (core.lower-if-tail-expr block expr ret-ty locals))
      ((symbol=? (middle.kind expr) '|middle.expr.handle|)
       (core.lower-handle-tail-expr block expr ret-ty locals))
      (else
       (if (type.unit? ret-ty)
           (begin (core.lower-expr block expr ret-ty locals) (core.return-none! block))
           (if (core.is-effect-expr? expr)
               ;; Effect expressions (perform/resume/handle): use full product type, no wrapping
               (core.return-value! block (core.lower-expr block expr ret-ty locals))
               ;; Normal expressions: lower with inner type, then wrap
               (let* ((inner-ty (if (core.return-is-product? ret-ty)
                                    (core.product-value-type ret-ty)
                                    ret-ty))
                      (lowered (core.lower-expr block expr inner-ty locals))
                      (wrapped (if (core.return-is-product? ret-ty)
                                   (core.wrap-throws-return block ret-ty lowered)
                                   lowered)))
                 (core.return-value! block wrapped))))))
    locals))

(define-pass (core-stmt-lowerer |middle.stmt.expr| block stmt ret-ty locals)
  (let* ((payload (middle.payload stmt)) (expr (optional.value (record.get payload '|expr|))))
    (cond
      ((symbol=? (middle.kind expr) '|middle.expr.if|)
       (core.lower-if-tail-expr block expr ret-ty locals)
       (core.set-current-block! block))
      ((symbol=? (middle.kind expr) '|middle.expr.handle|)
       (core.lower-handle-tail-expr block expr ret-ty locals)
       (core.set-current-block! block))
      (else
       (core.lower-expr block expr (core.infer-expr-type expr locals) locals)))
    locals))

(define-pass (core-stmt-lowerer |middle.stmt.let| block stmt ret-ty locals)
  (let* ((payload (middle.payload stmt))
         (ty-option (optional.value (record.get payload '|type|)))
         (name (optional.value (record.get payload '|name|)))
         (mutable (optional.value (record.get payload '|mutable|)))
         (value-expr (optional.value (record.get payload '|value|)))
         (ty (if (optional.none? ty-option) (core.infer-expr-type value-expr locals)
                 (core.lower-type (optional.value ty-option))))
         (value (core.lower-expr block value-expr ty locals)))
    (list.cons (record '|local| (record.field '|name| name) (record.field '|type| ty)
                 (record.field '|mutable| mutable) (record.field '|value| value)) locals)))

(define-pass (core-stmt-lowerer |middle.stmt.assign| block stmt ret-ty locals)
  (let* ((payload (middle.payload stmt)) (target (optional.value (record.get payload '|target|)))
         (value-expr (optional.value (record.get payload '|value|))))
    (if (symbol=? (middle.kind target) '|middle.expr.path|)
        (let* ((target-payload (middle.payload target)) (path (optional.value (record.get target-payload '|path|)))
               (name (list.first path)))
          (if (core.local-mutable? locals name)
              (let* ((ty (core.local-type locals name)) (value (core.lower-expr block value-expr ty locals)))
                (list.cons (record '|local| (record.field '|name| name) (record.field '|type| ty)
                             (record.field '|mutable| #t) (record.field '|value| value)) locals))
              (type.unsupported '|immutable-assignment|)))
        (type.unsupported '|assignment-target|))))

(define (core.lower-stmt block stmt ret-ty locals)
  (let* ((kind (middle.kind stmt)) (lowerer (pipeline.lookup '|core-stmt-lowerer| kind)))
    (if lowerer (lowerer block stmt ret-ty locals) locals)))

(define (core.lower-stmts block stmts ret-ty locals)
  (if (list.empty? stmts) (core.return-none! block)
      (let* ((next-locals (core.lower-stmt block (list.first stmts) ret-ty locals))
             ;; Follow g_current_block — stmt lowerers may create branches and switch blocks
             (current (core.get-current-block)))
        (if (list.empty? (list.rest stmts)) unit
            (core.lower-stmts current (list.rest stmts) ret-ty next-locals)))))
