(meta-source "effects/form")

;; ── 从 brace group 中解析 effect 操作签名（树 API） ──
;;
;; brace children 格式:
;;   (juxt (ident fn) (-> (call (ident name) (paren ...)) (ident ret-type)))
;;   (sep ;)
;;   ...
;; 或无 ->:
;;   (juxt (ident fn) (call (ident name) (paren ...)))
;;   (sep ;)

(define (effect.parse-operation-tree node)
  ;; node: (juxt (ident fn) rest)
  ;; rest: (-> (call (ident name) (paren params...)) ret-type)
  ;;    or (call (ident name) (paren params...))  [no return type]
  (let* ((parts (tree.flatten-juxt node))
         ;; parts: ((ident fn) rest-node)
         ;; skip the fn keyword
         (rest (list.first (list.rest parts))))
    ;; rest is either (-> call-node ret-type) or (call-node) directly
    ;; Actually rest could be (-> (call ...) ret-type) or just (call ...)
    ;; Also handle effects: (-> call ret !(brace ...)) via juxt
    (let* ((has-arrow (and (pair? rest) (eq? (car rest) '|->|)))
           (call-node (if has-arrow (tree.left rest) rest))
           (ret-and-effects (if has-arrow (tree.right rest) #f))
           ;; call-node: (call (ident name) (paren params...))
           (name (tree.ident-sym (tree.call-callee call-node)))
           (param-paren (tree.call-args call-node))
           (params (effect.parse-params-tree (tree.group-children param-paren))))
      ;; Parse return type and optional effects from ret-and-effects
      ;; ret-and-effects could be:
      ;;   (ident i32)                   — simple return, no effects
      ;;   (prefix ! (ident type))       — negation return (!)
      ;;   (juxt ret-type (! ... ...))   — return with effects (future)
      (let* ((ret (if ret-and-effects
                      (effect.tree->type ret-and-effects)
                      (lain-quote '(type-unit))))
             (effects (optional.none)))
        (raw.node! '|effect.operation|
          (record '|effect.operation|
            (record.field '|name| name)
            (record.field '|params| params)
            (record.field '|return| ret)
            (record.field '|effects| effects)))))))

(define (effect.tree->type node)
  ;; Convert a simple type tree node to a lain-quote type
  (cond
   ((tree.ident? node)
    (let ((sym (tree.ident-sym node)))
      (if (symbol=? sym '|!|)
          (lain-quote '(type-never))
          (lain-quote `(type-path ,sym)))))
   ;; prefix ! → never type (the ! token as return type)
   ((and (pair? node) (eq? (car node) 'prefix)
         (eq? (tree.prefix-op node) '|!|))
    (lain-quote '(type-never)))
   ;; (paren) → unit type
   ((and (tree.paren? node) (null? (tree.group-children node)))
    (lain-quote '(type-unit)))
   ;; type application: (< base-type type-arg) — e.g. Option<i32>
   ;; Actually in tree: (call (ident Option) (paren ...)) won't happen for types
   ;; For now, handle simple ident types
   (else
    (lain-quote `(type-path ,(if (tree.ident? node) (tree.ident-sym node) '|unknown|))))))

(define (effect.parse-params-tree children)
  ;; children: list of (: (ident name) (ident type)), (sep ,), ...
  ;; Filter out separators, parse each param
  (let ((loop #f))
  (set! loop (lambda (cs acc)
    (if (null? cs)
        (list.reverse acc)
        (let ((c (car cs)))
          (if (tree.sep? c)
              (loop (cdr cs) acc)
              ;; c should be (: (ident name) type-node)
              (let* ((name (tree.ident-sym (tree.left c)))
                     (ty (effect.tree->type (tree.right c)))
                     (param (lain-quote `(param ,name ,ty))))
                (loop (cdr cs) (list.cons param acc))))))))
  (loop children (list))))

(define (effect.parse-operations-tree children acc)
  ;; children: list of brace group children (operation nodes and (sep ;))
  (let ((loop #f))
  (set! loop (lambda (cs acc)
    (if (null? cs)
        (list.reverse acc)
        (let ((c (car cs)))
          (if (tree.sep? c)
              (loop (cdr cs) acc)
              (let ((op (effect.parse-operation-tree c)))
                (loop (cdr cs) (list.cons op acc))))))))
  (loop children acc)))

(define-pass* 'form-parser '|effect| (lambda (form)
  (let* ((tree (form.tree form))
         (attrs (form.decorators form))
         ;; tree: (juxt (ident effect) (juxt (ident Name) (brace ...)))
         ;; or: (juxt (ident effect) (ident Name)) for empty effect without brace?
         ;; Flatten juxt to get parts
         (parts (tree.flatten-juxt tree))
         ;; parts: ((ident effect) (ident Name) (brace ...))
         ;; or for effect Name<T>: ((ident effect) (< (ident Name) (ident T)) (brace ...))
         ;; skip keyword
         (rest (list.rest parts))
         (name-node (list.first rest))
         (name (if (tree.ident? name-node)
                   (tree.ident-sym name-node)
                   ;; Generic: name-node might be (< (ident Name) ...)
                   (tree.ident-sym (tree.left name-node))))
         (body-node (if (null? (list.rest rest))
                        #f
                        (list.first (list.rest rest))))
         (operations (if (and body-node (tree.brace? body-node))
                         (effect.parse-operations-tree
                           (tree.group-children body-node) (list))
                         (list)))
         (node (raw.node! '|effect|
                 (record '|effect|
                   (record.field '|attrs| attrs)
                   (record.field '|operations| operations)))))
    ;; Register in Scheme-side effect registry for validation
    (register-effect-ctor! name 0)
    ;; Phase 4: register a default product layout for this effect.
    ;; Default layout: {flag: u8, value: i32} — same structure as Throws
    ;; without an error/arg field. fn.make-effect-product-name will
    ;; substitute the function's actual return type for the value field.
    ;; Effects with declared operations will get richer layouts
    ;; from the operation's parameter types (future work).
    (if (null? operations)
        (effect.register-layout! name '|default|
          (list (core.make-bits 8) (core.make-bits 32))
          0    ;; flag-index: field 0 is the flag
          '()) ;; no arg indices
        unit)
    (decl.define-dup-checked! '|effect| name node))))

(define-pass* 'raw-normalizer '|effect| (lambda (decl)
  (middle.normalize-plain-decl decl '|middle.effect|)))
