(meta-source "fn/parse")

;; ═══════════════════════════════════════════════════
;; 函数语法解析 — 树结构版本
;; 辅助函数（tree-parse-type, tree-parse-params 等）在 surface/tree.scm
;; ═══════════════════════════════════════════════════

;; ── fn 签名提取 ──

(define (tree-extract-fn-name-and-generics node)
  (cond
   ((tree.ident? node) (values (tree.ident-sym node) (list)))
   ((and (pair? node) (eq? (car node) '|<|))
    (let* ((left (tree.left node))
           (name (if (tree.ident? left) (tree.ident-sym left)
                     (if (tree.juxt? left)
                         (tree.ident-sym (car (tree.flatten-juxt left)))
                         (tree.ident-sym left))))
           (generics (tree-collect-generic-idents (tree.right node))))
      (values name generics)))
   (else (values '_unknown (list)))))

(define (tree-extract-fn-parts tree)
  (let* ((flat (tree.flatten-juxt tree)))
    (let ((skip-kw #f))
  (set! skip-kw (lambda (parts)
      (if (null? parts)
          (values '_noname (list) (list) (lain-quote '(type-unit))
                  (optional.none) (optional.none) (list))
          (let ((first (car parts)) (rest (cdr parts)))
            (if (and (tree.ident? first)
                     (memv (tree.ident-sym first) '(|fn| |pub| |comptime|)))
                (skip-kw rest)
                (tree-extract-fn-parts-inner parts))))))
  (skip-kw flat))))

(define (tree-extract-fn-parts-inner parts)
  (let ((node (car parts)) (rest (cdr parts)))
    (cond
     ;; -> 节点包含 call + return+body
     ((and (pair? node) (eq? (car node) '|->|))
      (let* ((left (tree.left node)) (right (tree.right node)))
        (let-values (((name generics params) (tree-extract-call-info left)))
          (let-values (((ret body-node remaining) (tree-extract-ret-and-body right rest)))
            (let-values (((effects remaining2) (tree-find-effects remaining)))
              (let-values (((where remaining3) (tree-parse-where remaining2)))
                (let ((body (tree-resolve-body body-node remaining3)))
                  (values name generics params ret body effects where))))))))
     ;; call 节点 (无 ->)
     ((tree.call? node)
      (let-values (((name generics params) (tree-extract-call-info node)))
        (let-values (((effects remaining) (tree-find-effects rest)))
          (let-values (((where remaining2) (tree-parse-where remaining)))
            (let ((body (tree-find-body remaining2)))
              (values name generics params (lain-quote '(type-unit)) body effects where))))))
     ;; name 后面跟 paren
     (else
      (let-values (((name generics) (tree-extract-fn-name-and-generics node)))
        (if (and (pair? rest) (tree.paren? (car rest)))
            (let* ((params (tree-parse-params (car rest)))
                   (after-params (cdr rest)))
              (let-values (((ret body-node remaining) (tree-extract-ret-from-parts after-params)))
                (let-values (((effects remaining2) (tree-find-effects remaining)))
                  (let-values (((where remaining3) (tree-parse-where remaining2)))
                    (let ((body (tree-resolve-body body-node remaining3)))
                      (values name generics params ret body effects where))))))
            (values name generics (list) (lain-quote '(type-unit))
                    (optional.none) (optional.none) (list))))))))

(define (tree-extract-call-info call-node)
  (let* ((callee (tree.call-callee call-node))
         (args-paren (tree.call-args call-node)))
    (let-values (((name generics) (tree-extract-fn-name-and-generics callee)))
      (values name generics (tree-parse-params args-paren)))))

(define (tree-extract-ret-and-body right-of-arrow rest)
  (cond
   ((tree.juxt? right-of-arrow)
    (let ((loop #f))
  (set! loop (lambda (parts type-parts)
      (if (null? parts)
          (values (tree-parts-to-type (reverse type-parts)) #f rest)
          (if (tree.brace? (car parts))
              (values (tree-parts-to-type (reverse type-parts)) (car parts)
                      (append (cdr parts) rest))
              (loop (cdr parts) (cons (car parts) type-parts))))))
  (loop (tree.flatten-juxt right-of-arrow) '())))
   ((tree.brace? right-of-arrow)
    (values (lain-quote '(type-unit)) right-of-arrow rest))
   (else
    (values (tree-parse-type right-of-arrow) #f rest))))

(define (tree-parts-to-type parts)
  (if (null? parts) (lain-quote '(type-unit))
      (if (null? (cdr parts)) (tree-parse-type (car parts))
          (tree-parse-type (tree-rebuild-juxt parts)))))

(define (tree-extract-ret-from-parts parts)
  (if (null? parts) (values (lain-quote '(type-unit)) #f (list))
      (let ((first (car parts)) (rest (cdr parts)))
        (cond
         ((and (pair? first) (eq? (car first) '|->|))
          (tree-extract-ret-and-body (tree.right first) rest))
         ((tree.brace? first) (values (lain-quote '(type-unit)) first rest))
         (else (tree-extract-ret-from-parts rest))))))

(define (tree-find-body remaining)
  (let ((loop #f))
  (set! loop (lambda (ps)
    (if (null? ps) (optional.none)
        (cond
         ((tree.brace? (car ps))
          (optional.some (tree-parse-block (car ps))))
         ((and (tree.sep? (car ps)) (eq? (tree.sep-sym (car ps)) '|;|))
          (optional.none))
         (else (loop (cdr ps)))))))
  (loop remaining)))

(define (tree-resolve-body body-node remaining)
  (if body-node
      (optional.some (tree-parse-block body-node))
      (tree-find-body remaining)))

;; ═══════════════════════════════════════════════════
;; Form Parsers
;; ═══════════════════════════════════════════════════

(define-pass* 'form-parser '|fn| (lambda (form)
  (let* ((attrs (tree-parse-attrs form))
         (tree (form.tree form)))
    (let-values (((name generics params ret body effects where)
                  (tree-extract-fn-parts tree)))
      (let* ((sig-payload (record '|fn.sig|
                            (record.field '|attrs| attrs)
                            (record.field '|generics| generics)
                            (record.field '|params| params)
                            (record.field '|return| ret)
                            (record.field '|where| where)
                            (record.field '|effects| effects)
                            (record.field '|body| body)))
             (unified (raw.node! '|let|
                        (record '|let|
                          (record.field '|name| name)
                          (record.field '|type-kind| '|fn|)
                          (record.field '|payload| sig-payload)))))
        (if (fn.cfg-enabled? attrs) (decl.define-dup-checked! '|let| name unified) unit))))))

(define-pass* 'form-parser '|pub| (lambda (form)
  (let* ((attrs (tree-parse-attrs form))
         (tree (form.tree form))
         (flat (tree.flatten-juxt tree)))
    (let ((skip-pub #f))
  (set! skip-pub (lambda (parts)
      (if (null? parts) unit
          (let ((first (car parts)))
            (if (and (tree.ident? first) (eq? (tree.ident-sym first) '|pub|))
                (let ((after-pub (cdr parts)))
                  (if (null? after-pub) unit
                      (let ((kw (car after-pub)))
                        (cond
                         ;; pub fn
                         ((and (tree.ident? kw) (eq? (tree.ident-sym kw) '|fn|))
                          (let ((fn-tree (tree-rebuild-juxt after-pub)))
                            (let-values (((name generics params ret body effects where)
                                          (tree-extract-fn-parts fn-tree)))
                              (let* ((sig (record '|fn.sig|
                                            (record.field '|public| #t) (record.field '|attrs| attrs)
                                            (record.field '|generics| generics) (record.field '|params| params)
                                            (record.field '|return| ret) (record.field '|where| where)
                                            (record.field '|effects| effects) (record.field '|body| body)))
                                     (unified (raw.node! '|let|
                                                (record '|let|
                                                  (record.field '|name| name)
                                                  (record.field '|type-kind| '|fn|)
                                                  (record.field '|payload| sig)))))
                                (if (fn.cfg-enabled? attrs) (decl.define-dup-checked! '|let| name unified) unit)))))
                         ;; pub struct
                         ((and (tree.ident? kw) (eq? (tree.ident-sym kw) '|struct|))
                          (let* ((struct-parts (cdr after-pub))
                                 (first-s (if (null? struct-parts) #f (car struct-parts))))
                            (if (not first-s) unit
                                (let* ((ng (tree-extract-name-and-generics first-s))
                                       (name (car ng)) (generics (cdr ng))
                                       (after-name (cdr struct-parts)))
                                  (let-values (((where after-where) (tree-parse-where after-name)))
                                    (let* ((brace-node (tree-find-brace after-where))
                                           (fields (if brace-node
                                                       (struct.parse-fields-tree (tree.group-children brace-node) (list))
                                                       (list)))
                                           (payload (record '|struct|
                                                      (record.field '|public| #t) (record.field '|attrs| attrs)
                                                      (record.field '|generics| generics) (record.field '|where| where)
                                                      (record.field '|fields| fields)))
                                           (unified (raw.node! '|let|
                                                      (record '|let|
                                                        (record.field '|name| name)
                                                        (record.field '|type-kind| '|struct|)
                                                        (record.field '|payload| payload)))))
                                      (if (fn.cfg-enabled? attrs) (decl.define-dup-checked! '|let| name unified) unit)))))))
                         (else unit)))))
                (skip-pub (cdr parts)))))))
  (skip-pub flat)))))

(define-pass* 'form-parser '|comptime| (lambda (form)
  (let* ((attrs (tree-parse-attrs form))
         (tree (form.tree form)))
    (let-values (((name generics params ret body effects where)
                  (tree-extract-fn-parts tree)))
      (let* ((sig (record '|fn.sig|
                    (record.field '|comptime| #t)
                    (record.field '|attrs| attrs) (record.field '|generics| generics)
                    (record.field '|params| params) (record.field '|return| ret)
                    (record.field '|where| where) (record.field '|effects| effects)
                    (record.field '|body| body)))
             (unified (raw.node! '|let|
                        (record '|let|
                          (record.field '|name| name)
                          (record.field '|type-kind| '|fn|)
                          (record.field '|payload| sig)))))
        (if (fn.cfg-enabled? attrs) (decl.define-dup-checked! '|let| name unified) unit))))))
