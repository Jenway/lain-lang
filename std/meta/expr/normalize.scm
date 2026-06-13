(meta-source "expr/normalize")

;; ═══════════════════════════════════════════════════════════
;; 表达式规范化 — middle-normalizer 通道
;; 将 raw AST 节点转换为 middle IR 节点
;; ═══════════════════════════════════════════════════════════

(define-pass (middle-normalizer |expr.path| raw-expr)
  (let* ((payload (raw.payload raw-expr)))
    (middle.node! '|middle.expr.path|
      (record '|middle.expr.path|
        (record.field '|path|
          (optional.value
            (record.get payload '|path|)))))))

(define-pass (middle-normalizer |expr.call| raw-expr)
  (let* ((payload (raw.payload raw-expr)))
    (middle.node! '|middle.expr.call|
      (record '|middle.expr.call|
        (record.field '|callee|
          (middle.normalize-expr
            (optional.value
              (record.get payload '|callee|))))
        (record.field '|type-args|
          (let* ((raw-type-args (record.get payload '|type-args|)))
            (if (optional.none? raw-type-args)
                (list)
                (middle.normalize-types
                  (optional.value raw-type-args)
                  (list)))))
        (record.field '|args|
          (middle.normalize-exprs
            (optional.value
              (record.get payload '|args|))
            (list)))))))

(define-pass (middle-normalizer |expr.method-call| raw-expr)
  (let* ((payload (raw.payload raw-expr)))
    (middle.node! '|middle.expr.method-call|
      (record '|middle.expr.method-call|
        (record.field '|receiver|
          (middle.normalize-expr
            (optional.value
              (record.get payload '|receiver|))))
        (record.field '|method|
          (optional.value
            (record.get payload '|method|)))
        (record.field '|args|
          (middle.normalize-exprs
            (optional.value
              (record.get payload '|args|))
            (list)))))))

(define-pass (middle-normalizer |expr.builtin| raw-expr)
  (let* ((payload (raw.payload raw-expr)))
    (middle.node! '|middle.expr.builtin|
      (record '|middle.expr.builtin|
        (record.field '|name|
          (optional.value
            (record.get payload '|name|)))
        (record.field '|args|
          (middle.normalize-exprs
            (optional.value
              (record.get payload '|args|))
            (list)))))))

(define-pass (middle-normalizer |expr.tail-call| raw-expr)
  (let* ((payload (raw.payload raw-expr)))
    (middle.node! '|middle.expr.tail-call|
      (record '|middle.expr.tail-call|
        (record.field '|call|
          (middle.normalize-expr
            (optional.value
              (record.get payload '|call|))))))))

(define-pass (middle-normalizer |expr.macro-call| raw-expr)
  (let* ((payload (raw.payload raw-expr))
         (name (optional.value
                 (record.get payload '|name|)))
         (args (optional.value
                 (record.get payload '|args|)))
         (expand (pipeline.rule '|expression-macro| name)))
    ;; 注意: macro 展开目前忽略 type-args (宏不支持泛型参数)
    ;; 后续可扩展为 (expand args type-args)
    (middle.normalize-expr (expand args))))

(define-pass (middle-normalizer |expr.call-indirect| raw-expr)
  (let* ((payload (raw.payload raw-expr)))
    (middle.node! '|middle.expr.call-indirect|
      (record '|middle.expr.call-indirect|
        (record.field '|fn-ptr|
          (middle.normalize-expr
            (optional.value
              (record.get payload '|fn-ptr|))))
        (record.field '|ret-ty|
          (middle.normalize-type
            (optional.value
              (record.get payload '|ret-ty|))))
        (record.field '|args|
          (middle.normalize-exprs
            (optional.value
              (record.get payload '|args|))
            (list)))))))

;; ── ? 操作符规范化 (postfix ?) ──
(define-pass (middle-normalizer |expr.question| raw-expr)
  (let* ((payload (raw.payload raw-expr)))
    (middle.node! '|middle.expr.question|
      (record '|middle.expr.question|
        (record.field '|expr|
          (middle.normalize-expr
            (optional.value
              (record.get payload '|expr|))))))))
