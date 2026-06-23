(meta-source "path/lower")

;; ═══════════════════════════════════════════════════════════
;; Path 表达式降级 — path.access → 常量 / 局部变量 / 导入函数
;; ═══════════════════════════════════════════════════════════

(define (core.path-leaf path)
  (if (list.empty? (list.rest path))
      (list.first path)
      (core.path-leaf (list.rest path))))

(define-pass (core-expr-lowerer |path.access| block expr expected-ty locals)
  (let* ((payload (middle.payload expr))
         (path (optional.value
                 (record.get payload '|path|)))
         (imported (import.resolve-qualified-symbol path)))
    (if imported
        (core.function-by-name imported)
        ;; Check compile-time constant table for top-level let bindings
        (let* ((leaf (core.path-leaf path))
               (const (const-table.lookup leaf)))
          (if const
              (let* ((const-ty (cadr const))
                     (const-val (caddr const)))
                (core.const-bits! block const-ty const-val))
              (core.local-lookup locals leaf))))))
