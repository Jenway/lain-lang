(meta-source "memory/normalize")

;; ═══════════════════════════════════════════════════════════
;; 内存/指针 — 类型构造注册 + 规范化 + intrinsic
;; ═══════════════════════════════════════════════════════════

;; ── 类型构造器注册 ──

(register-type-constructor! '|Ptr| 1 '(raw-ptr const))
(register-type-constructor! '|MutPtr| 1 '(raw-ptr mut))
(register-type-constructor! '|Ref| 1 '|ref-type|)
(register-type-constructor! '|MutRef| 1 '|mut-ref-type|)
(register-type-constructor! '|Slice| 1 '|slice-type|)

(register-raw-pointer-type! '|const| '|Ptr|)
(register-raw-pointer-type! '|mut| '|MutPtr|)
(register-raw-pointer-index-type! '|u64|)

;; ── intrinsic 通道 ──

(define-pass (intrinsic |load| block args expected-ty locals)
  (let* ((ptr-expr (list.first args))
         (ptr (core.lower-expr block ptr-expr expected-ty locals)))
    (ir.expr.load block ptr expected-ty)))

(define-pass (intrinsic |store| block args expected-ty locals)
  (let* ((ptr-expr (list.first args))
         (value-expr (list.first (list.rest args)))
         (ptr (core.lower-expr block ptr-expr expected-ty locals))
         (value (core.lower-expr block value-expr expected-ty locals)))
    (ir.inst.store block ptr value)
    unit))

(define-pass (intrinsic |raw-ptr-read| block args expected-ty locals)
  (let* ((ptr-expr (list.first args))
         (ptr (core.lower-expr block ptr-expr expected-ty locals)))
    (ir.expr.load block ptr expected-ty)))

(define-pass (intrinsic |raw-ptr-write| block args expected-ty locals)
  (let* ((ptr-expr (list.first args))
         (value-expr (list.first (list.rest args)))
         (ptr (core.lower-expr block ptr-expr expected-ty locals))
         (value (core.lower-expr block value-expr expected-ty locals)))
    (ir.inst.store block ptr value)
    unit))

;; ── borrow 表达式规范化 ──

(define-pass (middle-normalizer |expr.borrow| raw-expr)
  (let* ((payload (raw.payload raw-expr)))
    (middle.node! '|middle.expr.borrow|
      (record '|middle.expr.borrow|
        (record.field '|mutable|
          (optional.value
            (record.get payload '|mutable|)))
        (record.field '|operand|
          (middle.normalize-expr
            (optional.value
              (record.get payload '|operand|))))))))
