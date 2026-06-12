(meta-source "effects/normalize")

;; ═══════════════════════════════════════════════════════════
;; 效应系统 — 表达式规范化 (perform / resume / handle)
;; ═══════════════════════════════════════════════════════════

(define-pass (middle-normalizer |expr.perform| raw-expr)
  (let* ((payload (raw.payload raw-expr)))
    (middle.node! '|middle.expr.perform|
      (record '|middle.expr.perform|
        (record.field '|call|
          (middle.normalize-expr
            (optional.value
              (record.get payload '|call|))))))))

(define-pass (middle-normalizer |expr.resume| raw-expr)
  (let* ((payload (raw.payload raw-expr)))
    (middle.node! '|middle.expr.resume|
      (record '|middle.expr.resume|
        (record.field '|value|
          (let* ((value (optional.value
                         (record.get payload '|value|))))
            (if (optional.none? value)
                (optional.none)
                (optional.some
                  (middle.normalize-expr
                    (optional.value value))))))))))

(define-pass (middle-normalizer |expr.handle| raw-expr)
  (let* ((payload (raw.payload raw-expr)))
    (middle.node! '|middle.expr.handle|
      (record '|middle.expr.handle|
        (record.field '|effect|
          (optional.value
            (record.get payload '|effect|)))
        (record.field '|effect-args|
          (middle.normalize-types
            (optional.value
              (record.get payload '|effect-args|))
            (list)))
        (record.field '|handler|
          (optional.value
            (record.get payload '|handler|)))
        (record.field '|body|
          (middle.normalize-block
            (optional.value
              (record.get payload '|body|))))))))
