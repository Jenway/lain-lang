(meta-source "module/parse")

;; ===========================================================================
;; Module / Signature / Export parsing (tree API)
;;
;; First implementation goal:
;; - support top-level `let name = import("a::b");`
;; - reserve top-level `let name = signature { ... };`
;; - reserve top-level `let name = module { ... };`
;; - support `export { a, b, c }`
;;
;; This pass only builds raw/middle placeholders so the unified top-level
;; pipeline can carry these declarations. Real module/signature semantics
;; will be implemented in later phases.
;; ===========================================================================

(define (module.split-import-path str)
  (let* ((len (string-length str)))
    (let ((loop #f))
  (set! loop (lambda (i start acc)
      (if (>= i len)
          (let* ((segment (substring str start len)))
            (list.reverse
              (list.cons (string->symbol segment) acc)))
          (if (and (< (+ i 1) len)
                   (char=? (string-ref str i) #\:)
                   (char=? (string-ref str (+ i 1)) #\:))
              (let* ((segment (substring str start i)))
                (loop (+ i 2) (+ i 2)
                      (list.cons (string->symbol segment) acc)))
              (loop (+ i 1) start acc)))))
  (loop 0 0 (list)))))

;; ── import binding: let name = import("path"); ──
;; rhs-node: (call (ident import) (paren (string path-str)))
(define (module.parse-import-binding-tree attrs name rhs-node)
  (let* ((args-paren (tree.call-args rhs-node))
         (children (tree.group-children args-paren))
         (path-str-node (list.first children)))
    (if (not (tree.string? path-str-node))
        (error "import(...) expects a string literal path")
        (let* ((path (module.split-import-path
                       (symbol->string (tree.string-val path-str-node))))
               (inner-payload (record '|import.binding|
                                (record.field '|attrs| attrs)
                                (record.field '|path| path)))
               (unified (raw.node! '|let|
                          (record '|let|
                            (record.field '|name| name)
                            (record.field '|type-kind| '|import-binding|)
                            (record.field '|payload| inner-payload)))))
          (decl.define-dup-checked! '|let| name unified)))))

;; ── signature binding: let name = signature { ... }; ──
;; rhs-node: (juxt (ident signature) (brace ...))
(define (module.parse-signature-binding-tree attrs name rhs-node)
  (let* ((parts (tree.flatten-juxt rhs-node))
         ;; parts: ((ident signature) (brace ...))
         (body (list.first (list.rest parts)))
         (inner-payload (record '|signature|
                          (record.field '|attrs| attrs)
                          (record.field '|body| body)))
         (unified (raw.node! '|let|
                    (record '|let|
                      (record.field '|name| name)
                      (record.field '|type-kind| '|signature|)
                      (record.field '|payload| inner-payload)))))
    (decl.define-dup-checked! '|let| name unified)))

;; ── module binding: let name = module { ... }; ──
;; rhs-node: (juxt (ident module) (brace ...))
(define (module.parse-module-binding-tree attrs name rhs-node)
  (let* ((parts (tree.flatten-juxt rhs-node))
         (body (list.first (list.rest parts)))
         (inner-payload (record '|module|
                          (record.field '|attrs| attrs)
                          (record.field '|body| body)))
         (unified (raw.node! '|let|
                    (record '|let|
                      (record.field '|name| name)
                      (record.field '|type-kind| '|module|)
                      (record.field '|payload| inner-payload)))))
    (decl.define-dup-checked! '|let| name unified)))

;; ── meta-alias binding: let name = some::path; ──
;; rhs-node: (:: ...) path tree
(define (module.parse-meta-alias-binding-tree attrs name rhs-node)
  (let* ((path-nodes (tree.flatten-path rhs-node))
         (path (map (lambda (n) (tree.ident-sym n)) path-nodes))
         (inner-payload (record '|meta.alias|
                          (record.field '|attrs| attrs)
                          (record.field '|path| path)))
         (unified (raw.node! '|let|
                    (record '|let|
                      (record.field '|name| name)
                      (record.field '|type-kind| '|meta-alias|)
                      (record.field '|payload| inner-payload)))))
    (decl.define-dup-checked! '|let| name unified)))

;; ── expr binding: let name = <expr>; ──
;; RHS is not import/signature/module/path — parse as expression.
;; The expression will be comptime-evaluated during lowering.
(define (module.parse-expr-binding-tree attrs name rhs-node)
  (let* ((expr (expr.lower rhs-node '()))
         (inner-payload (record '|expr.binding|
                          (record.field '|attrs| attrs)
                          (record.field '|value| expr)))
         (unified (raw.node! '|let|
                    (record '|let|
                      (record.field '|name| name)
                      (record.field '|type-kind| '|expr-binding|)
                      (record.field '|payload| inner-payload)))))
    (decl.define-dup-checked! '|let| name unified)))

;; ── Helper: extract the first ident from a rhs-node for dispatch ──
(define (module.rhs-first-ident rhs-node)
  (cond
   ((tree.ident? rhs-node) (tree.ident-sym rhs-node))
   ((tree.juxt? rhs-node)  (module.rhs-first-ident (tree.left rhs-node)))
   ((tree.call? rhs-node)  (module.rhs-first-ident (tree.call-callee rhs-node)))
   (else #f)))

;; ── Helper: check if a node is a :: path ──
(define (module.path-node? node)
  (and (pair? node) (eq? (car node) '|::|)))

(define-pass* 'form-parser '|let| (lambda (form)
  (let* ((tree (form.tree form))
         (attrs (form.decorators form))
         ;; tree: (juxt (ident let) (= name-node rhs-node))
         ;;   or: (juxt (ident let) (= (: name-node type-node) rhs-node))
         (parts (tree.flatten-juxt tree))
         ;; parts: ((ident let) assign-node)
         (assign-node (list.first (list.rest parts)))
         ;; assign-node: (= lhs rhs)
         (lhs (tree.left assign-node))
         (rhs (tree.right assign-node))
         ;; lhs could be (ident name) or (: (ident name) type)
         (name (if (tree.ident? lhs)
                   (tree.ident-sym lhs)
                   ;; (: (ident name) type) — name with type annotation
                   (tree.ident-sym (tree.left lhs))))
         (rhs-kw (module.rhs-first-ident rhs)))
    (cond
      ((and rhs-kw (symbol=? rhs-kw '|import|))
       (module.parse-import-binding-tree attrs name rhs))
      ((and rhs-kw (symbol=? rhs-kw '|signature|))
       (module.parse-signature-binding-tree attrs name rhs))
      ((and rhs-kw (symbol=? rhs-kw '|module|))
       (module.parse-module-binding-tree attrs name rhs))
      ;; Bool literals and numeric expressions → expr binding
      ((and rhs-kw (symbol=? rhs-kw '|true|))
       (module.parse-expr-binding-tree attrs name rhs))
      ((and rhs-kw (symbol=? rhs-kw '|false|))
       (module.parse-expr-binding-tree attrs name rhs))
      ;; :: path → meta-alias
      ((module.path-node? rhs)
       (module.parse-meta-alias-binding-tree attrs name rhs))
      ;; Bare ident that's not a keyword → also meta-alias (single-segment path)
      ((and rhs-kw (not (tree.call? rhs)) (not (tree.juxt? rhs))
            (tree.ident? rhs))
       (module.parse-meta-alias-binding-tree attrs name rhs))
      ;; Everything else → general expression
      (else
       (module.parse-expr-binding-tree attrs name rhs))))))

;; ── export { names... } ──

(define (module.parse-export-names-tree children acc)
  ;; children: list of (ident name) and (sep ,)
  (let ((loop #f))
  (set! loop (lambda (cs acc)
    (if (null? cs)
        (list.reverse acc)
        (let ((c (car cs)))
          (if (tree.sep? c)
              (loop (cdr cs) acc)
              (loop (cdr cs) (list.cons (tree.ident-sym c) acc)))))))
  (loop children acc)))

(define-pass* 'form-parser '|export| (lambda (form)
  (let* ((tree (form.tree form))
         (attrs (form.decorators form))
         ;; tree: (juxt (ident export) (brace ...))
         (parts (tree.flatten-juxt tree))
         ;; parts: ((ident export) (brace ...))
         (body (list.first (list.rest parts)))
         (names (module.parse-export-names-tree (tree.group-children body) (list)))
         (node (raw.node! '|export|
                 (record '|export|
                   (record.field '|attrs| attrs)
                   (record.field '|names| names)))))
    (decl.define! '|export| '|export| node))))

(define-pass* 'raw-normalizer '|export| (lambda (decl)
  (middle.normalize-plain-decl decl '|middle.export|)))

(define *module-registry* (list))
(define *signature-registry* (list))

(define (module.register-name! name registry)
  (if (list.member? registry name)
      registry
      (list.cons name registry)))

(define (module.register-module! name)
  (set! *module-registry*
    (module.register-name! name *module-registry*)))

(define (module.register-signature! name)
  (set! *signature-registry*
    (module.register-name! name *signature-registry*)))

(define (module.declare-export-names names)
  (if (list.empty? names)
      unit
      (let* ((name (list.first names)))
        ;; Mark as export for the interface emitter.
        ;; Link_name is set by @foreign(c) or pub fn; the .lci emitter
        ;; auto-generates one for non-pub exported fns that lack it.
        (core.mark-export! name)
        (module.declare-export-names (list.rest names)))))

(define-pass* 'core-declarer '|middle.export| (lambda (item)
  (meta.ensure-static-position! '|middle.export|)
  unit))

(define-pass* 'core-lowerer '|middle.export| (lambda (item)
  (meta.ensure-static-position! '|middle.export|)
  (let* ((payload (middle.payload item))
         (raw-inner (optional.value (record.get payload '|payload|)))
         (names (optional.value (record.get raw-inner '|names|))))
    (module.declare-export-names names))))

(define-pass* 'core-declarer '|middle.signature| (lambda (item)
  (meta.ensure-static-position! '|middle.signature|)
  (let* ((payload (middle.payload item))
         (name (optional.value (record.get payload '|name|))))
    (module.register-signature! name)
    (core.declare-signature! name))))

(define-pass* 'core-lowerer '|middle.signature| (lambda (item)
  (meta.ensure-static-position! '|middle.signature|)
  unit))

(define-pass* 'core-declarer '|middle.module| (lambda (item)
  (meta.ensure-static-position! '|middle.module|)
  (let* ((payload (middle.payload item))
         (name (optional.value (record.get payload '|name|))))
    (module.register-module! name)
    (core.declare-module! name))))

(define-pass* 'core-lowerer '|middle.module| (lambda (item)
  (meta.ensure-static-position! '|middle.module|)
  unit))

(define (module.declare-imported-fn-alias! alias fn-name)
  (let* ((sub (core.function-by-name fn-name))
         (ret-ty (core.function-return-type sub))
         (param-tys (core.function-param-types sub))
         (link-name (core.function-link-name sub)))
    (host.new-extern alias link-name param-tys ret-ty)))

(define-pass* 'core-declarer '|middle.meta-alias| (lambda (item)
  (let* ((payload (middle.payload item))
         (name (optional.value (record.get payload '|name|)))
         (raw-inner (optional.value (record.get payload '|payload|)))
         (path (optional.value (record.get raw-inner '|path|)))
         (fn-export (import.resolve-qualified-symbol path))
         (module-export (import.resolve-qualified-module-name path))
         (signature-export (import.resolve-qualified-signature-name path)))
    (cond
      (fn-export
       (module.declare-imported-fn-alias! name fn-export))
      (module-export
       (module.register-module! name)
       (core.declare-module! name))
      (signature-export
        (module.register-signature! name)
        (core.declare-signature! name))
      (else
       (error (string-append
                "unsupported top-level let binding path: "
                (symbol->string (list.first path)))))))))

(define-pass* 'core-lowerer '|middle.meta-alias| (lambda (item)
  unit))
