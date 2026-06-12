(meta-source "core/literals")

(define-pass (middle-normalizer |expr.number| raw-expr)
  (let* ((payload (raw.payload raw-expr)))
    (middle.node! '|middle.expr.number|
      (record '|middle.expr.number|
        (record.field '|raw|
          (optional.value
            (record.get payload '|raw|)))))))

(define-pass (middle-normalizer |expr.string| raw-expr)
  (let* ((payload (raw.payload raw-expr)))
    (middle.node! '|middle.expr.string|
      (record '|middle.expr.string|
        (record.field '|raw|
          (optional.value
            (record.get payload '|raw|)))))))

(define-pass (middle-normalizer |expr.bool| raw-expr)
  (let* ((payload (raw.payload raw-expr)))
    (middle.node! '|middle.expr.bool|
      (record '|middle.expr.bool|
        (record.field '|value|
          (optional.value
            (record.get payload '|value|)))))))

(define-pass (core-expr-lowerer |middle.expr.number| block expr expected-ty locals)
  (let* ((payload (middle.payload expr)))
    (core.const-bits!
      block
      expected-ty
      (optional.value
        (record.get payload '|raw|)))))

(define-pass (core-expr-lowerer |middle.expr.string| block expr expected-ty locals)
  (let* ((payload (middle.payload expr)))
    (core.const-string!
      block
      expected-ty
      (optional.value
        (record.get payload '|raw|)))))

(define-pass (core-expr-lowerer |middle.expr.bool| block expr expected-ty locals)
  (let* ((payload (middle.payload expr))
         (value (optional.value
                  (record.get payload '|value|))))
    (core.const-bits!
      block
      expected-ty
      (if value 1 0))))

;; ---------------------------------------------------------------------------
;; 表达式类型推导: 字面量类型
;; ---------------------------------------------------------------------------

(define-pass (core-expr-inferer |middle.expr.bool| expr locals)
  (type.registered '|bool| (list)))

(define-pass (core-expr-inferer |middle.expr.string| expr locals)
  (type.registered '|addr| (list)))

(define-pass (core-expr-inferer |middle.expr.number| expr locals)
  (type.unsupported '|inferred-expression-type|))
