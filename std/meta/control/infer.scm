(meta-source "control/infer")

;; ═══════════════════════════════════════════════════════════
;; 控制流 — 表达式类型推导
;; ═══════════════════════════════════════════════════════════

(define (core.infer-block-value-type block locals)
  (let* ((payload (middle.payload block))
         (items (optional.value (record.get payload '|items|))))
    (core.infer-stmt-list-value-type items locals)))

(define (core.infer-stmt-list-value-type items locals)
  (if (list.empty? items)
      (type.unsupported '|empty-if-branch|)
      (if (list.empty? (list.rest items))
          (core.infer-final-stmt-value-type (list.first items) locals)
          (let* ((stmt (list.first items))
                 (next-locals (core.infer-stmt-locals stmt locals)))
            (core.infer-stmt-list-value-type (list.rest items) next-locals)))))

(define (core.infer-stmt-locals stmt locals)
  (let* ((kind (middle.kind stmt)))
    (if (symbol=? kind '|let.bind|)
        (let* ((payload (middle.payload stmt))
               (ty-option (optional.value (record.get payload '|type|)))
               (name (optional.value (record.get payload '|name|)))
               (mutable (optional.value (record.get payload '|mutable|)))
               (value-expr (optional.value (record.get payload '|value|)))
               (ty (if (optional.none? ty-option)
                       (core.infer-expr-type value-expr locals)
                       (core.lower-type (optional.value ty-option)))))
          (list.cons
            (record '|local|
              (record.field '|name| name)
              (record.field '|type| ty)
              (record.field '|mutable| mutable)
              (record.field '|value| #f))
            locals))
        locals)))

(define (core.infer-final-stmt-value-type stmt locals)
  (let* ((kind (middle.kind stmt))
         (payload (middle.payload stmt)))
    (cond
      ((symbol=? kind '|control.tail|)
       (core.infer-expr-type (optional.value (record.get payload '|expr|)) locals))
      ((symbol=? kind '|control.expr|)
       (core.infer-expr-type (optional.value (record.get payload '|expr|)) locals))
      ((symbol=? kind '|control.return|)
       (let* ((value (optional.value (record.get payload '|value|))))
         (if (optional.none? value)
             (type.unit)
             (core.infer-expr-type (optional.value value) locals))))
      (else
       (type.unsupported '|if-branch-value|)))))

(define-pass* 'core-expr-inferer '|control.if| (lambda (expr locals)
  (let* ((payload (middle.payload expr))
         (then-block (optional.value (record.get payload '|then|))))
    (core.infer-block-value-type then-block locals))))
