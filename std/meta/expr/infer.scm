(meta-source "expr/infer")

;; ═══════════════════════════════════════════════════════════
;; 表达式类型推导 — core-expr-inferer 通道
;; 从 middle IR 表达式推导其类型
;; ═══════════════════════════════════════════════════════════

(define-pass* 'core-expr-inferer '|path.access| (lambda (expr locals)
  (let* ((payload (middle.payload expr))
         (path (optional.value (record.get payload '|path|)))
         (imported (import.resolve-qualified-symbol path)))
    (if imported
        (core.function-return-type (core.function-by-name imported))
        (core.local-type locals (core.path-leaf path))))))

(define-pass* 'core-expr-inferer '|call.fn| (lambda (expr locals)
  (let* ((payload (middle.payload expr))
         (callee (optional.value (record.get payload '|callee|)))
         (callee-payload (middle.payload callee))
         (path (optional.value (record.get callee-payload '|path|)))
         (fn-name (core.path-fn-name path)))
    (core.function-return-type (core.function-by-name fn-name)))))

(define-pass* 'core-expr-inferer '|call.method| (lambda (expr locals)
  (let* ((payload (middle.payload expr))
         (method (optional.value (record.get payload '|method|))))
    (core.function-return-type (core.function-by-name method)))))

(define-pass* 'core-expr-inferer '|call.builtin| (lambda (expr locals)
  (let* ((payload (middle.payload expr))
         (args (optional.value (record.get payload '|args|))))
    (core.infer-expr-type (list.first args) locals))))

(define-pass* 'core-expr-inferer '|call.tail| (lambda (expr locals)
  (let* ((payload (middle.payload expr))
         (call (optional.value (record.get payload '|call|))))
    (core.infer-expr-type call locals))))

(define-pass* 'core-expr-inferer '|call.indirect| (lambda (expr locals)
  (let* ((payload (middle.payload expr))
         (ret-ty (optional.value (record.get payload '|ret-ty|))))
    (core.lower-type ret-ty))))
