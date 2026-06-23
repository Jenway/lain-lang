(meta-source "expr/cast")

;; ═══════════════════════════════════════════════════
;; cast 表达式: expr as Type
;; ═══════════════════════════════════════════════════

;; ── 规范化 ──

(define-pass (middle-normalizer |expr.cast| raw-expr)
  (let* ((payload (raw.payload raw-expr))
         (inner (optional.value (record.get payload '|expr|)))
         (target-ty (optional.value (record.get payload '|ty|))))
    (middle.node! '|middle.expr.cast|
      (record '|middle.expr.cast|
        (record.field '|expr| (middle.normalize-expr inner))
        (record.field '|target| (middle.normalize-type target-ty))))))

;; ── 降级: 无运行时操作，只改类型 ──

(define-pass (core-expr-lowerer |middle.expr.cast| block expr expected-ty locals)
  (let* ((payload (middle.payload expr))
         (inner (optional.value (record.get payload '|expr|)))
         (target-ty (core.lower-type
                      (optional.value (record.get payload '|target|)))))
    (core.lower-expr block inner target-ty locals)))

;; ── 类型推导: 返回目标类型 ──

(define-pass (core-expr-inferer |middle.expr.cast| expr locals)
  (let* ((payload (middle.payload expr))
         (target (optional.value (record.get payload '|target|))))
    (core.lower-type target)))
