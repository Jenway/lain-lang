(meta-source "control/match")

;; ── match 表达式解析（树 API 版） ──

(define (tree-parse-match-pattern node)
  ;; node: (ident _) → wildcard
  ;;        (ident Name) → variant without binding
  ;;        (call (ident Name) (paren (ident binding))) → variant with binding
  (cond
   ((and (tree.ident? node) (eq? (tree.ident-sym node) '|_|))
    (raw.node! '|match.pattern.wildcard| (record '|match.pattern.wildcard|)))
   ((tree.ident? node)
    (raw.node! '|match.pattern.variant|
      (record '|match.pattern.variant|
        (record.field '|name| (tree.ident-sym node))
        (record.field '|binding| (optional.none)))))
   ((tree.call? node)
    (let* ((name (tree.ident-sym (tree.call-callee node)))
           (args (tree-filter-comma (tree.group-children (tree.call-args node))))
           (binding (if (null? args) (optional.none)
                        (optional.some (tree.ident-sym (car args))))))
      (raw.node! '|match.pattern.variant|
        (record '|match.pattern.variant|
          (record.field '|name| name)
          (record.field '|binding| binding)))))
   (else
    (raw.node! '|match.pattern.wildcard| (record '|match.pattern.wildcard|)))))

(define (tree-parse-match-arm node)
  ;; node: (=> pattern (brace body...))
  (if (and (pair? node) (eq? (car node) '|=>|))
      (let* ((pattern (tree-parse-match-pattern (tree.left node)))
             (body-node (tree.right node))
             (body (if (tree.brace? body-node)
                       (tree-parse-block body-node)
                       (lain-quote `(block (tail ,(tree-lower-expr body-node)))))))
        (raw.node! '|match.arm|
          (record '|match.arm|
            (record.field '|pattern| pattern)
            (record.field '|body| body))))
      ;; fallback
      (raw.node! '|match.arm|
        (record '|match.arm|
          (record.field '|pattern| (raw.node! '|match.pattern.wildcard| (record '|match.pattern.wildcard|)))
          (record.field '|body| (lain-quote '(block (tail (number 0)))))))))

(define (tree-parse-match-arms children acc)
  ;; children of brace: arm1 (sep ,) arm2 (sep ,) ...
  (if (null? children) (list.reverse acc)
      (let ((child (car children))
            (rest (cdr children)))
        (if (and (tree.sep? child) (eq? (tree.sep-sym child) '|,|))
            (tree-parse-match-arms rest acc)
            (tree-parse-match-arms rest (list.cons (tree-parse-match-arm child) acc))))))

;; ── match normalizer ──

(define (middle.normalize-match-arms-helper raw-arms acc)
  (if (list.empty? raw-arms)
      (list.reverse acc)
      (let* ((raw-arm (list.first raw-arms))
             (arm-payload (raw.payload raw-arm))
             (pattern (optional.value (record.get arm-payload '|pattern|)))
             (body-raw (optional.value (record.get arm-payload '|body|)))
             (body-mid (middle.normalize-block body-raw))
             (pattern-kind (raw.kind pattern))
             (pattern-payload (raw.payload pattern))
             (pname (if (symbol=? pattern-kind '|match.pattern.variant|)
                        (optional.value (record.get pattern-payload '|name|))
                        '|_|))
             (pbinding (if (symbol=? pattern-kind '|match.pattern.variant|)
                           (record.get pattern-payload '|binding|)
                           (optional.none))))
        (middle.normalize-match-arms-helper
         (list.rest raw-arms)
         (list.cons (record '|middle.match.arm|
                      (record.field '|pattern-kind| pattern-kind)
                      (record.field '|pattern-name| pname)
                      (record.field '|pattern-binding| pbinding)
                      (record.field '|body| body-mid))
                    acc)))))

(define-pass* 'middle-normalizer '|expr.match| (lambda (raw-expr)
  (let* ((payload (raw.payload raw-expr))
         (scrutinee (middle.normalize-expr (optional.value (record.get payload '|scrutinee|))))
         (raw-arms (optional.value (record.get payload '|arms|))))
    (middle.node! '|control.match|
      (record '|control.match|
        (record.field '|scrutinee| scrutinee)
        (record.field '|arms| (middle.normalize-match-arms-helper raw-arms (list))))))))

(define-pass* 'core-expr-lowerer '|control.match| (lambda (block expr expected-ty locals)
  (core.const-bits! block (core.make-bits 32) 0)))
