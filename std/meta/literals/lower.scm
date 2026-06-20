(meta-source "literals/lower")

(define (literal.number-value raw)
  (cond
    ((number? raw) raw)
    ((symbol? raw) (string->number (symbol->string raw)))
    ((string? raw) (string->number raw))
    (else raw)))

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

;; ── Array literal: [init; N] ──
(define-pass (middle-normalizer |expr.array| raw-expr)
  (let* ((payload (raw.payload raw-expr)))
    (middle.node! '|middle.expr.array|
      (record '|middle.expr.array|
        (record.field '|init|
          (optional.value
            (record.get payload '|init|)))
        (record.field '|len|
          (optional.value
            (record.get payload '|len|)))))))

(define-pass (core-expr-lowerer |middle.expr.number| block expr expected-ty locals)
  (let* ((payload (middle.payload expr))
         (raw (optional.value
                (record.get payload '|raw|))))
    (ir.expr.const
      block
      expected-ty
      (literal.number-value raw))))

(define-pass (core-expr-lowerer |middle.expr.string| block expr expected-ty locals)
  (let* ((payload (middle.payload expr)))
    (ir.expr.const-string
      block
      expected-ty
      (optional.value
        (record.get payload '|raw|)))))

(define-pass (core-expr-lowerer |middle.expr.bool| block expr expected-ty locals)
  (let* ((payload (middle.payload expr))
         (value (optional.value
                  (record.get payload '|value|))))
    (ir.expr.const
      block
      expected-ty
      (if value 1 0))))

;; ── Array literal lowering: alloca only ──
;; 先保证数组字面量能稳定通过 lowering。
;; 元素批量初始化后面再补。
(define-pass (core-expr-lowerer |middle.expr.array| block expr expected-ty locals)
  (let* ((payload (middle.payload expr))
         (init-expr (optional.value (record.get payload '|init|)))
         (len (literal.number-value
                (optional.value (record.get payload '|len|))))
         ;; Use i32 as element type (simplified — all numeric literals are i32)
         (elem-ty (ir.type.bits 32))
         ;; Allocate on stack: [i32 x len] → returns pointer expression
         (ptr (ir.expr.alloca block elem-ty (* len 4)))
         ;; Keep init expr reachable for future full initialization pass.
         (_init-val (core.lower-expr block init-expr elem-ty locals)))
    ptr))

;; ---------------------------------------------------------------------------
;; 表达式类型推导: 字面量类型
;; ---------------------------------------------------------------------------

(define-pass (core-expr-inferer |middle.expr.bool| expr locals)
  (type.registered '|bool| (list)))

(define-pass (core-expr-inferer |middle.expr.string| expr locals)
  (type.registered '|addr| (list)))

(define-pass (core-expr-inferer |middle.expr.number| expr locals)
  (type.unsupported '|inferred-expression-type|))

(define-pass (core-expr-inferer |middle.expr.array| expr locals)
  (type.registered '|addr| (list)))
