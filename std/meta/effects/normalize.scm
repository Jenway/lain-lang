(meta-source "effects/normalize")

;; ═══════════════════════════════════════════════════════════
;; 效应系统 — 表达式规范化 (perform / resume / handle)
;; ═══════════════════════════════════════════════════════════

(define-pass* 'middle-normalizer '|expr.perform| (lambda (raw-expr)
  (let* ((payload (raw.payload raw-expr)))
    (middle.node! '|effects.perform|
      (record '|effects.perform|
        (record.field '|call|
          (middle.normalize-expr
            (optional.value
              (record.get payload '|call|)))))))))

(define-pass* 'middle-normalizer '|expr.resume| (lambda (raw-expr)
  (let* ((payload (raw.payload raw-expr)))
    (middle.node! '|effects.resume|
      (record '|effects.resume|
        (record.field '|value|
          (let* ((value (optional.value
                         (record.get payload '|value|))))
            (if (optional.none? value)
                (optional.none)
                (optional.some
                  (middle.normalize-expr
                    (optional.value value)))))))))))

(define-pass* 'middle-normalizer '|expr.handle| (lambda (raw-expr)
  (let* ((payload (raw.payload raw-expr)))
    (middle.node! '|effects.handle|
      (record '|effects.handle|
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
              (record.get payload '|body|)))))))))
