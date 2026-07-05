;; ============================================================================
;; std/meta/surface/tree.scm — 结构化树 API + 共享解析工具
;;
;; 供 form parsers 直接操作 canonicalize.scm 输出的树节点。
;;
;; 树节点格式（参见 canonicalize.scm）：
;;   (ident sym)       (number n)       (string s)    (sep sym)
;;   (juxt l r)        (OP l r)         (prefix OP x) (postfix x OP-or-group)
;;   (paren ...)       (brace ...)      (bracket ...)
;;
;; form 格式：(root node) 或 (root deco-node decl-node)
;; ============================================================================

(meta-source "surface/tree")

;; ══════════════════════════════════════════════════════════════════
;; 1. 节点类型查询
;; ══════════════════════════════════════════════════════════════════

(define (tree.kind node)
  (if (pair? node) (car node) #f))

(define (tree.ident? node)
  (and (pair? node) (eq? (car node) 'ident)))

(define (tree.number? node)
  (and (pair? node) (eq? (car node) 'number)))

(define (tree.string? node)
  (and (pair? node) (eq? (car node) 'string)))

(define (tree.sep? node)
  (and (pair? node) (eq? (car node) 'sep)))

(define (tree.juxt? node)
  (and (pair? node) (eq? (car node) 'juxt)))

(define (tree.call? node)
  (and (pair? node)
       (or (eq? (car node) 'call) ;; legacy compatibility
           (and (eq? (car node) 'postfix)
                (tree.paren? (caddr node))))))

(define (tree.paren? node)
  (and (pair? node) (eq? (car node) 'paren)))

(define (tree.brace? node)
  (and (pair? node) (eq? (car node) 'brace)))

(define (tree.bracket? node)
  (and (pair? node) (eq? (car node) 'bracket)))

;; ══════════════════════════════════════════════════════════════════
;; 2. 叶节点值提取
;; ══════════════════════════════════════════════════════════════════

(define (tree.ident-sym node) (cadr node))
(define (tree.number-val node) (cadr node))
(define (tree.string-val node) (cadr node))
(define (tree.sep-sym node) (cadr node))

;; ══════════════════════════════════════════════════════════════════
;; 3. 复合节点字段提取
;; ══════════════════════════════════════════════════════════════════

(define (tree.left node) (cadr node))
(define (tree.right node) (caddr node))
(define (tree.call-callee node) (cadr node))
(define (tree.call-args node) (caddr node))
(define (tree.prefix-op node) (cadr node))
(define (tree.prefix-operand node) (caddr node))
(define (tree.postfix-operand node) (cadr node))
(define (tree.postfix-op node) (caddr node))

(define (tree.group-children node) (cdr node))

;; ══════════════════════════════════════════════════════════════════
;; 4. 结构工具
;; ══════════════════════════════════════════════════════════════════

(define (tree.flatten-juxt node)
  (define (collect node acc)
    (if (tree.juxt? node)
        (collect (tree.left node)
                 (collect (tree.right node) acc))
        (cons node acc)))
  (collect node '()))

(define (tree.flatten-path node)
  (define (collect node acc)
    (if (and (pair? node) (eq? (car node) '|::|))
        (collect (tree.left node)
                 (cons (tree.right node) acc))
        (cons node acc)))
  (collect node '()))

(define (tree-rebuild-juxt parts)
  (if (null? (cdr parts))
      (car parts)
      (list 'juxt (car parts) (tree-rebuild-juxt (cdr parts)))))

(define (tree-find-brace parts)
  (let ((loop #f))
  (set! loop (lambda (ps)
    (if (null? ps) #f
        (if (tree.brace? (car ps)) (car ps)
            (loop (cdr ps))))))
  (loop parts)))

;; ══════════════════════════════════════════════════════════════════
;; 5. form 层工具
;; ══════════════════════════════════════════════════════════════════

(define (form.tree form)
  (let ((children (cdr form)))
    (if (null? children) '()
        (car (reverse children)))))

(define (form.decorators form)
  (let ((children (cdr form)))
    (if (or (null? children) (null? (cdr children)))
        '()
        (reverse (cdr (reverse children))))))

(define (form.keyword form)
  (define (first-ident node)
    (cond
     ((tree.ident? node) (tree.ident-sym node))
     ((tree.juxt? node)  (first-ident (tree.left node)))
     ((tree.call? node)  (first-ident (tree.call-callee node)))
     (else #f)))
  (let ((decl (form.tree form)))
    (first-ident decl)))

;; ══════════════════════════════════════════════════════════════════
;; 6. 类型解析: 树节点 → lain-quote 类型 AST
;; ══════════════════════════════════════════════════════════════════

(define (tree-parse-type node)
  (cond
   ((tree.ident? node)
    (lain-quote `(type-path ,(tree.ident-sym node))))

   ((tree.paren? node)
    (if (null? (tree.group-children node))
        (lain-quote '(type-unit))
        (tree-parse-type (car (tree.group-children node)))))

   ;; &T / &mut T
   ((and (pair? node) (eq? (car node) 'prefix) (eq? (cadr node) '|&|))
    (let* ((inner (caddr node))
           (flat (tree.flatten-juxt inner)))
      (if (and (pair? flat) (tree.ident? (car flat))
               (eq? (tree.ident-sym (car flat)) '|mut|))
          (lain-quote `(type-ref #t ,(tree-parse-type
                                       (if (null? (cdr flat)) '(ident unit)
                                           (if (null? (cddr flat))
                                               (cadr flat)
                                               (tree-rebuild-juxt (cdr flat)))))))
          (lain-quote `(type-ref #f ,(tree-parse-type inner))))))

   ;; *mut T / *const T
   ((and (pair? node) (eq? (car node) 'prefix) (eq? (cadr node) '|*|))
    (let* ((inner (caddr node))
           (flat (tree.flatten-juxt inner)))
      (if (and (pair? flat) (tree.ident? (car flat))
               (eq? (tree.ident-sym (car flat)) '|mut|))
          (lain-quote `(type-raw-ptr #t ,(tree-parse-type
                                           (if (null? (cdr flat)) '(ident unit)
                                               (if (null? (cddr flat))
                                                   (cadr flat)
                                                   (tree-rebuild-juxt (cdr flat)))))))
          (lain-quote `(type-raw-ptr #f ,(tree-parse-type
                                           (if (null? (cdr flat)) inner
                                               (if (null? (cddr flat))
                                                   (cadr flat)
                                                   (tree-rebuild-juxt (cdr flat))))))))))

   ;; [T] / [T; N]
   ((tree.bracket? node)
    (tree-parse-bracket-type (tree.group-children node)))

   ;; Name<T, U> → (type-app Name args)
   ((and (pair? node) (eq? (car node) '|<|))
    (let* ((left (tree.left node))
           (right (tree.right node))
           (name (tree.ident-sym left))
           (args (tree-collect-type-args right)))
      (lain-quote `(type-app ,name ,args))))

   ;; fn(T) -> U
   ((tree.juxt? node)
    (let* ((flat (tree.flatten-juxt node)))
      (if (and (pair? flat) (tree.ident? (car flat))
               (eq? (tree.ident-sym (car flat)) '|fn|))
          (tree-parse-fn-type (cdr flat))
          (tree-parse-type (car flat)))))

   ;; -> in type context
   ((and (pair? node) (eq? (car node) '|->|))
    (tree-parse-type (tree.left node)))

   (else
    (lain-quote '(type-unit)))))

(define (tree-parse-bracket-type children)
  (let ((loop #f))
  (set! loop (lambda (kids before-semi after-semi)
    (if (null? kids)
        (if after-semi
            (lain-quote `(type-array ,(tree-parse-type (car (reverse before-semi)))
                                     ,(tree.number-val after-semi)))
            (if (null? before-semi)
                (lain-quote '(type-slice (type-unit)))
                (lain-quote `(type-slice ,(tree-parse-type (car (reverse before-semi)))))))
        (let ((kid (car kids)))
          (if (and (tree.sep? kid) (eq? (tree.sep-sym kid) '|;|))
              (loop (cdr kids) before-semi
                    (if (null? (cdr kids)) #f (cadr kids)))
              (if after-semi
                  (loop (cdr kids) before-semi kid)
                  (loop (cdr kids) (cons kid before-semi) after-semi)))))))
  (loop children '() #f)))

(define (tree-parse-fn-type rest-parts)
  (let* ((paren-node (car rest-parts))
         (params (tree-parse-type-list-from-paren paren-node))
         (after-paren (cdr rest-parts)))
    (if (null? after-paren)
        (lain-quote `(type-fn ,params ,(lain-quote '(type-unit)) ,(optional.none)))
        (let* ((arrow-or-rest (car after-paren)))
          (if (and (pair? arrow-or-rest) (eq? (car arrow-or-rest) '|->|))
              (lain-quote `(type-fn ,params ,(tree-parse-type (tree.right arrow-or-rest)) ,(optional.none)))
              (lain-quote `(type-fn ,params ,(tree-parse-type arrow-or-rest) ,(optional.none))))))))

(define (tree-parse-type-list-from-paren paren-node)
  (let ((loop #f))
  (set! loop (lambda (kids acc)
    (if (null? kids) (list.reverse acc)
        (let ((kid (car kids)))
          (if (and (tree.sep? kid) (eq? (tree.sep-sym kid) '|,|))
              (loop (cdr kids) acc)
              (loop (cdr kids) (list.cons (tree-parse-type kid) acc)))))))
  (loop (tree.group-children paren-node) (list))))

(define (tree-collect-type-args node)
  (cond
   ((and (pair? node) (eq? (car node) '|,|))
    (append (tree-collect-type-args (tree.left node))
            (tree-collect-type-args (tree.right node))))
   ((and (pair? node) (eq? (car node) '|>|))
    (tree-collect-type-args (tree.left node)))
   (else
    (list (tree-parse-type node)))))

;; ══════════════════════════════════════════════════════════════════
;; 7. 泛型参数解析
;; ══════════════════════════════════════════════════════════════════

(define (tree-extract-name-and-generics node)
  ;; 返回 (name . generics-list)
  (cond
   ((tree.ident? node)
    (cons (tree.ident-sym node) (list)))
   ((and (pair? node) (eq? (car node) '|<|))
    (let* ((left (tree.left node))
           (name (if (tree.ident? left) (tree.ident-sym left)
                     (if (tree.juxt? left)
                         (tree.ident-sym (car (tree.flatten-juxt left)))
                         (tree.ident-sym left))))
           (generics (tree-collect-generic-idents (tree.right node))))
      (cons name generics)))
   (else (cons '_unknown (list)))))

(define (tree-collect-generic-idents node)
  (cond
   ((tree.ident? node) (list (tree.ident-sym node)))
   ((and (pair? node) (eq? (car node) '|,|))
    (append (tree-collect-generic-idents (tree.left node))
            (tree-collect-generic-idents (tree.right node))))
   ((and (pair? node) (eq? (car node) '|>|))
    (tree-collect-generic-idents (tree.left node)))
   ((tree.juxt? node)
    (let ((loop #f))
  (set! loop (lambda (parts acc)
      (if (null? parts) (list.reverse acc)
          (let ((p (car parts)))
            (if (tree.ident? p)
                (loop (cdr parts) (list.cons (tree.ident-sym p) acc))
                (loop (cdr parts) acc))))))
  (loop (tree.flatten-juxt node) (list))))
   (else (list))))

;; ══════════════════════════════════════════════════════════════════
;; 8. 参数解析: (paren (: name type) (sep ,) ...) → param list
;; ══════════════════════════════════════════════════════════════════

(define (tree-parse-params paren-node)
  (let ((loop #f))
  (set! loop (lambda (children acc)
    (if (null? children) (list.reverse acc)
        (let* ((child (car children))
               (rest (cdr children)))
          (cond
           ((and (tree.sep? child) (eq? (tree.sep-sym child) '|,|))
            (loop rest acc))
           ((and (pair? child) (eq? (car child) '|:|))
            (let* ((name (tree.ident-sym (tree.left child)))
                   (ty (tree-parse-type (tree.right child)))
                   (param (lain-quote `(param ,name ,ty))))
              (loop rest (list.cons param acc))))
           (else (loop rest acc)))))))
  (loop (tree.group-children paren-node) (list))))

;; ══════════════════════════════════════════════════════════════════
;; 9. 装饰器解析: form.decorators → attr list
;; ══════════════════════════════════════════════════════════════════

(define (tree-parse-attr-value node)
  (cond
   ((tree.string? node) (lain-quote `(attr-arg-string ,(tree.string-val node))))
   ((tree.number? node) (lain-quote `(attr-arg-number ,(tree.number-val node))))
   (else (lain-quote `(attr-arg-path ,(if (tree.ident? node) (tree.ident-sym node) '_unknown))))))

(define (tree-parse-attr-arg node)
  (if (and (pair? node) (eq? (car node) '|=|))
      (let* ((name (if (tree.ident? (tree.left node)) (tree.ident-sym (tree.left node)) '_))
             (value (tree-parse-attr-value (tree.right node))))
        (lain-quote `(attr-arg-named ,name ,value)))
      (tree-parse-attr-value node)))

(define (tree-parse-attr-args paren-node)
  (let ((loop #f))
  (set! loop (lambda (children acc)
    (if (null? children) (list.reverse acc)
        (let ((child (car children)))
          (if (and (tree.sep? child) (eq? (tree.sep-sym child) '|,|))
              (loop (cdr children) acc)
              (loop (cdr children) (list.cons (tree-parse-attr-arg child) acc)))))))
  (loop (tree.group-children paren-node) (list))))

(define (tree-parse-single-decorator node)
  (let* ((flat (tree.flatten-juxt node)))
    (let ((loop #f))
  (set! loop (lambda (parts)
      (if (null? parts) #f
          (let ((first (car parts)))
            (if (and (tree.sep? first) (eq? (tree.sep-sym first) '|@|))
                (let ((attr-node (if (null? (cdr parts)) #f (cadr parts))))
                  (if (not attr-node) #f
                      (cond
                       ((tree.ident? attr-node)
                        (lain-quote `(attr ,(tree.ident-sym attr-node) ())))
                       ((tree.call? attr-node)
                        (let* ((name (tree.ident-sym (tree.call-callee attr-node)))
                               (args (tree-parse-attr-args (tree.call-args attr-node))))
                          (lain-quote `(attr ,name ,args))))
                       (else #f))))
                (loop (cdr parts)))))))
  (loop flat))))

(define (tree-parse-attrs form)
  (let ((loop #f))
  (set! loop (lambda (ds acc)
    (if (null? ds) (list.reverse acc)
        (let ((attr (tree-parse-single-decorator (car ds))))
          (loop (cdr ds) (if attr (list.cons attr acc) acc))))))
  (loop (form.decorators form) (list))))

;; ══════════════════════════════════════════════════════════════════
;; 10. Effects 解析
;; ══════════════════════════════════════════════════════════════════

(define (tree-parse-effect-name node)
  (cond
   ((tree.ident? node)
    (lain-quote `(effect-name ,(tree.ident-sym node) ())))
   ((and (pair? node) (eq? (car node) '|<|))
    (let* ((name (tree.ident-sym (tree.left node)))
           (args (tree-collect-type-args (tree.right node))))
      (lain-quote `(effect-name ,name ,args))))
   (else (lain-quote `(effect-name _unknown ())))))

(define (tree-parse-effect-set brace-node)
  (let ((loop #f))
  (set! loop (lambda (kids acc)
    (if (null? kids) (list.reverse acc)
        (let ((kid (car kids)))
          (if (and (tree.sep? kid) (eq? (tree.sep-sym kid) '|,|))
              (loop (cdr kids) acc)
              (loop (cdr kids) (list.cons (tree-parse-effect-name kid) acc)))))))
  (loop (tree.group-children brace-node) (list))))

(define (tree-find-effects parts)
  (let ((loop #f))
  (set! loop (lambda (ps before)
    (if (null? ps)
        (values (optional.none) (list.reverse before))
        (let ((p (car ps)))
          (cond
           ((and (pair? p) (eq? (car p) 'prefix) (eq? (cadr p) '|!|)
                 (tree.brace? (caddr p)))
            (values (optional.some (tree-parse-effect-set (caddr p)))
                    (append (list.reverse before) (cdr ps))))
           ((and (pair? p) (eq? (car p) '|!|))
            (let ((right (tree.right p)))
              (if (tree.brace? right)
                  (values (optional.some (tree-parse-effect-set right))
                          (append (list.reverse before) (cdr ps)))
                  (loop (cdr ps) (cons p before)))))
           (else (loop (cdr ps) (cons p before))))))))
  (loop parts '())))

;; ══════════════════════════════════════════════════════════════════
;; 11. Where 子句解析
;; ══════════════════════════════════════════════════════════════════

(define (tree-parse-where parts)
  (let ((loop #f))
  (set! loop (lambda (ps before)
    (if (null? ps)
        (values (list) (list.reverse before))
        (let ((p (car ps)))
          (if (and (tree.ident? p) (eq? (tree.ident-sym p) '|where|))
              (values (tree-parse-where-predicates (cdr ps)) (list.reverse before))
              (loop (cdr ps) (cons p before)))))))
  (loop parts '())))

(define (tree-parse-where-predicates parts)
  (let ((loop #f))
  (set! loop (lambda (ps acc)
    (if (null? ps) (list.reverse acc)
        (let ((p (car ps)))
          (cond
           ((and (tree.sep? p) (eq? (tree.sep-sym p) '|,|))
            (loop (cdr ps) acc))
           ((and (pair? p) (eq? (car p) '|:|))
            (let* ((param (tree.ident-sym (tree.left p)))
                   (bound (tree-parse-type (tree.right p)))
                   (pred (lain-quote `(where-predicate ,param ,bound))))
              (loop (cdr ps) (list.cons pred acc))))
           (else (loop (cdr ps) acc)))))))
  (loop parts (list))))

(define (tree-split-where-and-brace parts)
  ;; 返回 (where-list . brace-node-or-#f)
  (let-values (((where remaining) (tree-parse-where parts)))
    (cons where (tree-find-brace remaining))))

;; ══════════════════════════════════════════════════════════════════
;; 12. Block 解析 (替代 syntax.parse-block)
;; ══════════════════════════════════════════════════════════════════
;; 暂用简单 placeholder: 将 brace 组的子节点包装为 block AST
;; 完整实现需要在 surface/expr.scm 完成后替换

(define (tree-parse-block brace-node)
  ;; brace-node = (brace child...)
  ;; 简化: 遍历 children，每个当语句/表达式
  (let* ((children (tree.group-children brace-node))
         (items (tree-parse-block-items children (list))))
    (lain-quote `(block ,@items))))

(define (tree-parse-block-items children acc)
  (if (null? children)
      (list.reverse acc)
      (let ((child (car children))
            (rest (cdr children)))
        (cond
         ;; 跳过分号
         ((and (tree.sep? child) (eq? (tree.sep-sym child) '|;|))
          (tree-parse-block-items rest acc))
         ;; return expr
         ((and (tree.ident? child) (eq? (tree.ident-sym child) '|return|))
          (if (null? rest)
              (list.reverse (list.cons (lain-quote '(return void)) acc))
              ;; 下一个是返回值表达式
              (let-values (((expr remaining) (tree-take-expr rest)))
                (tree-parse-block-items remaining
                  (list.cons (lain-quote `(return ,expr)) acc)))))
         ;; return already grouped as juxt: (juxt (ident return) expr)
         ((and (tree.juxt? child)
               (let* ((flat (tree.flatten-juxt child)))
                 (and (pair? flat)
                      (tree.ident? (car flat))
                      (eq? (tree.ident-sym (car flat)) '|return|))))
          (let* ((flat (tree.flatten-juxt child))
                 (value-parts (cdr flat))
                 (return-expr (if (null? value-parts)
                                  (lain-quote '(return void))
                                  (lain-quote
                                    `(return
                                       ,(tree-lower-expr
                                          (if (null? (cdr value-parts))
                                              (car value-parts)
                                              (tree-rebuild-juxt value-parts)))))))
                 (remaining (if (and (pair? rest) (tree.sep? (car rest))
                                     (eq? (tree.sep-sym (car rest)) '|;|))
                                (cdr rest)
                                rest)))
            (tree-parse-block-items remaining (list.cons return-expr acc))))
         ;; let binding
         ((and (tree.ident? child) (eq? (tree.ident-sym child) '|let|))
          (let-values (((let-stmt remaining) (tree-parse-let-stmt rest)))
            (tree-parse-block-items remaining (list.cons let-stmt acc))))
         ;; let binding already grouped as juxt: (juxt (ident let) (= ...))
         ((and (tree.juxt? child)
               (let* ((flat (tree.flatten-juxt child)))
                 (and (pair? flat)
                      (tree.ident? (car flat))
                      (eq? (tree.ident-sym (car flat)) '|let|))))
          (let* ((flat (tree.flatten-juxt child)))
            (let-values (((let-stmt remaining) (tree-parse-let-stmt (cdr flat))))
              (tree-parse-block-items (append remaining rest) (list.cons let-stmt acc)))))
         ;; if statement at the front of a juxt may be followed by the next
         ;; expression without a semicolon. Split only the if prefix here.
         ((tree-leading-if? child)
          (let-values (((if-node remaining) (tree-split-leading-if child)))
            (tree-parse-block-items
              (append remaining rest)
              (list.cons (lain-quote `(expr-stmt ,(tree-lower-expr if-node))) acc))))
         ;; 最后一个表达式（无分号结尾）→ tail
         ((null? rest)
          (list.reverse (list.cons (lain-quote `(tail ,(tree-lower-expr child))) acc)))
         ;; 普通表达式语句
         (else
          ;; 检查下一个是否是 = (赋值)
          (if (and (pair? child) (eq? (car child) '|=|))
              ;; 赋值: (= lhs rhs)
              (let ((stmt (lain-quote `(assign ,(tree-lower-expr (tree.left child))
                                               ,(tree-lower-expr (tree.right child))))))
                (tree-parse-block-items rest (list.cons stmt acc)))
              (let ((stmt (lain-quote `(expr-stmt ,(tree-lower-expr child)))))
                (tree-parse-block-items rest (list.cons stmt acc)))))))))

(define (tree-leading-if? node)
  (let* ((flat (if (tree.juxt? node) (tree.flatten-juxt node) (list node))))
    (and (pair? flat)
         (tree.ident? (car flat))
         (eq? (tree.ident-sym (car flat)) '|if|)
         (pair? (cdr flat))
         (pair? (cddr flat))
         (tree.brace? (caddr flat)))))

(define (tree-split-leading-if node)
  (let* ((flat (if (tree.juxt? node) (tree.flatten-juxt node) (list node)))
         (if-head (car flat))
         (cond-node (cadr flat))
         (then-node (caddr flat))
         (after-then (cdddr flat))
         (has-else (and (pair? after-then)
                        (tree.ident? (car after-then))
                        (eq? (tree.ident-sym (car after-then)) '|else|)
                        (pair? (cdr after-then))
                        (tree.brace? (cadr after-then))))
         (if-parts (if has-else
                       (list if-head cond-node then-node (car after-then) (cadr after-then))
                       (list if-head cond-node then-node)))
         (rest-parts (if has-else (cddr after-then) after-then))
         (remaining (if (null? rest-parts)
                        '()
                        (list (if (null? (cdr rest-parts))
                                  (car rest-parts)
                                  (tree-rebuild-juxt rest-parts))))))
    (values (tree-rebuild-juxt if-parts) remaining)))

(define (tree-take-expr parts)
  ;; 取第一个非分号元素作为表达式，返回 (values expr remaining)
  (if (null? parts)
      (values (lain-quote '(number 0)) '())
      (let ((first (car parts))
            (rest (cdr parts)))
        (if (and (tree.sep? first) (eq? (tree.sep-sym first) '|;|))
            (values (lain-quote '(number 0)) rest)
            ;; 跳过后面的分号
            (let ((remaining (if (and (pair? rest) (tree.sep? (car rest))
                                      (eq? (tree.sep-sym (car rest)) '|;|))
                                 (cdr rest)
                                 rest)))
              (values (tree-lower-expr first) remaining))))))

(define (tree-if-head? node)
  (let* ((flat (if (tree.juxt? node) (tree.flatten-juxt node) (list node))))
    (and (pair? flat)
         (tree.ident? (car flat))
         (eq? (tree.ident-sym (car flat)) '|if|))))

(define (tree-collect-if-rhs rhs0 remaining)
  ;; RawAst keeps `if cond { then } else { else }` as siblings after the
  ;; leading `if` atom in let RHS. Rebuild one juxt expression and consume it.
  (if (not (tree-if-head? rhs0))
      (let* ((rhs-has-brace (and (pair? remaining) (tree.brace? (car remaining))))
             (rhs (if rhs-has-brace
                      (tree-rebuild-juxt (list rhs0 (car remaining)))
                      rhs0))
             (after-rhs (if rhs-has-brace (cdr remaining) remaining)))
        (values rhs after-rhs))
      (let* ((cond-node (if (pair? remaining) (car remaining) #f))
             (after-cond (if (pair? remaining) (cdr remaining) remaining))
             (then-node (if (and (pair? after-cond) (tree.brace? (car after-cond)))
                            (car after-cond)
                            #f))
             (after-then (if then-node (cdr after-cond) after-cond))
             (else-node (if (and (pair? after-then)
                                 (tree.ident? (car after-then))
                                 (eq? (tree.ident-sym (car after-then)) '|else|))
                            (car after-then)
                            #f))
             (after-else (if else-node (cdr after-then) after-then))
             (else-block (if (and (pair? after-else) (tree.brace? (car after-else)))
                             (car after-else)
                             #f))
             (after-rhs (if else-block (cdr after-else) after-else))
             (base (tree.flatten-juxt rhs0))
             (with-cond (if cond-node (append base (list cond-node)) base))
             (with-then (if then-node (append with-cond (list then-node)) with-cond))
             (with-else (if else-node (append with-then (list else-node)) with-then))
             (parts (if else-block (append with-else (list else-block)) with-else)))
        (values (tree-rebuild-juxt parts) after-rhs))))

(define (tree-parse-let-stmt parts)
  ;; parts 是 let 后面的内容
  ;; 可能形式:
  ;;   (= (: (ident x) type) value) — 带类型
  ;;   (= (ident x) value) — 不带类型
  ;;   (juxt (ident mut) (= ...)) — mutable
  (if (null? parts)
      (values (lain-quote '(let #f #f _x () (number 0))) '())
      (let* ((first (car parts))
             (rest (cdr parts)))
        ;; 检查 mut 关键字
        (let-values (((mutable node remaining)
                      (if (and (tree.ident? first) (eq? (tree.ident-sym first) '|mut|))
                          (values #t (if (null? rest) #f (car rest)) (if (null? rest) '() (cdr rest)))
                          (values #f first rest))))
          (if (not node)
              (values (lain-quote '(let #f #f _x () (number 0))) remaining)
              ;; node 是 (= lhs rhs) 或 (: name type) 后跟 (= ...)
              (cond
               ((and (pair? node) (eq? (car node) '|=|))
                (let* ((lhs (tree.left node))
                       (rhs0 (tree.right node)))
                  ;; lhs 可能是 (: name type) 或 (ident name)
                  (let-values (((rhs after-rhs) (tree-collect-if-rhs rhs0 remaining))
                               ((name ty) (tree-extract-let-name-type lhs)))
                    ;; 跳过后面的分号
                    (let ((remaining2 (if (and (pair? after-rhs) (tree.sep? (car after-rhs))
                                               (eq? (tree.sep-sym (car after-rhs)) '|;|))
                                          (cdr after-rhs)
                                          after-rhs)))
                      (values (lain-quote `(let ,mutable #f ,name ,ty ,(tree-lower-expr rhs)))
                              remaining2)))))
               (else
                (values (lain-quote `(let ,mutable #f _x () ,(tree-lower-expr node)))
                        remaining))))))))

(define (tree-extract-let-name-type lhs)
  ;; (: (ident name) type) → (values name (optional.some type))
  ;; (ident name) → (values name (optional.none))
  (cond
   ((and (pair? lhs) (eq? (car lhs) '|:|))
    (values (tree.ident-sym (tree.left lhs))
            (optional.some (tree-parse-type (tree.right lhs)))))
   ((tree.ident? lhs)
    (values (tree.ident-sym lhs) (optional.none)))
   (else (values '_x (optional.none)))))

;; ══════════════════════════════════════════════════════════════════
;; 13. 表达式降级 (树 → lain-quote AST)
;; ══════════════════════════════════════════════════════════════════

(define (tree-lower-expr node)
  (cond
   ((tree.ident? node)
    (lain-quote `(path ,(tree.ident-sym node))))
   ((tree.number? node)
    (lain-quote `(number ,(tree.number-val node))))
   ((tree.string? node)
    (lain-quote `(string ,(tree.string-val node))))
   ;; 函数调用: (call callee (paren args...))
   ((tree.call? node)
    (let* ((callee (tree.call-callee node))
           (args-paren (tree.call-args node))
           (arg-trees (tree-filter-comma (tree.group-children args-paren)))
           (callee-expr (tree-lower-expr callee))
           (arg-exprs (map tree-lower-expr arg-trees)))
      ;; 检查是否是 method call: callee 是 (. recv method)
      (if (and (pair? callee) (eq? (car callee) '|.|))
          (let ((recv (tree-lower-expr (tree.left callee)))
                (method (tree.ident-sym (tree.right callee))))
            (lain-quote `(method-call ,recv ,method ,@arg-exprs)))
          (lain-quote `(call ,callee-expr ,@arg-exprs)))))
   ;; 二元运算
   ((and (pair? node) (memv (car node) '(|+| |-| |*| |/| |%| |==| |!=| |<| |>| |<=| |>=| |&&| |\|\|| |&| |\|| |^|)))
    (lain-quote `(binary ,(car node) ,(tree-lower-expr (tree.left node))
                                      ,(tree-lower-expr (tree.right node)))))
   ;; 赋值
   ((and (pair? node) (eq? (car node) '|=|))
    (lain-quote `(assign ,(tree-lower-expr (tree.left node))
                         ,(tree-lower-expr (tree.right node)))))
   ;; 字段访问: (. recv field)
   ((and (pair? node) (eq? (car node) '|.|))
    (let ((recv (tree-lower-expr (tree.left node)))
          (field (tree.right node)))
      (if (tree.ident? field)
          (lain-quote `(field ,recv ,(tree.ident-sym field)))
          (lain-quote `(field ,recv _unknown)))))
   ;; 路径: (:: a b)
   ((and (pair? node) (eq? (car node) '|::|))
    (let* ((right (tree.right node))
           (syms (tree-path-syms node)))
      (if (tree.call? right)
          (let* ((args-paren (tree.call-args right))
                 (arg-trees (tree-filter-comma (tree.group-children args-paren)))
                 (arg-exprs (map tree-lower-expr arg-trees)))
            (lain-quote `(call (path ,@syms) ,@arg-exprs)))
          (lain-quote `(path ,@syms)))))
   ;; 前缀
   ((and (pair? node) (eq? (car node) 'prefix))
    (let ((op (tree.prefix-op node))
          (operand (tree-lower-expr (tree.prefix-operand node))))
      (cond
       ((eq? op '|&|) (lain-quote `(borrow #f ,operand)))
       ((eq? op '|-|) (lain-quote `(unary |-| ,operand)))
       ((eq? op '|!|) (lain-quote `(unary |!| ,operand)))
       ((eq? op '|*|) (lain-quote `(deref ,operand)))
       (else (lain-quote `(unary ,op ,operand))))))
   ;; 后缀 (? for error propagation)
   ((and (pair? node) (eq? (car node) 'postfix))
    (let ((operand (tree-lower-expr (tree.postfix-operand node)))
          (op (tree.postfix-op node)))
      (if (eq? op '|?|)
          (lain-quote `(try ,operand))
          (lain-quote `(postfix ,op ,operand)))))
   ;; 块: (brace ...)
   ((tree.brace? node)
    (tree-parse-block node))
   ;; 并列: juxt 在表达式上下文
   ((tree.juxt? node)
    (let ((flat (tree.flatten-juxt node)))
      (cond
       ;; if expr
       ((and (pair? flat) (tree.ident? (car flat)) (eq? (tree.ident-sym (car flat)) '|if|))
        (tree-lower-if-expr (cdr flat)))
       ;; true / false
       ((and (pair? flat) (tree.ident? (car flat)) (eq? (tree.ident-sym (car flat)) '|true|))
        (lain-quote '(bool #t)))
       ((and (pair? flat) (tree.ident? (car flat)) (eq? (tree.ident-sym (car flat)) '|false|))
        (lain-quote '(bool #f)))
       ;; @builtin
       ((and (pair? flat) (tree.sep? (car flat)) (eq? (tree.sep-sym (car flat)) '|@|))
        (tree-lower-builtin-expr (cdr flat)))
       ;; match expr { arms }
       ((and (pair? flat) (tree.ident? (car flat)) (eq? (tree.ident-sym (car flat)) '|match|))
        (tree-lower-match-expr (cdr flat)))
       ;; Struct literal: Name { field: value, ... }
       ((and (pair? flat)
             (tree.ident? (car flat))
             (pair? (cdr flat))
             (tree.brace? (cadr flat)))
        (let* ((name (tree.ident-sym (car flat)))
               (fields (struct.literal-fields-tree
                         (tree.group-children (cadr flat))
                         (list))))
          (lain-quote `(aggregate ,name ,@fields))))
       ;; perform call-expr
       ((and (pair? flat) (tree.ident? (car flat)) (eq? (tree.ident-sym (car flat)) '|perform|))
        (if (null? (cdr flat)) (lain-quote '(number 0))
            (lain-quote `(perform ,(tree-lower-expr (cadr flat))))))
       ;; resume(expr)
       ((and (pair? flat) (tree.ident? (car flat)) (eq? (tree.ident-sym (car flat)) '|resume|))
        (if (and (pair? (cdr flat)) (tree.call? (cadr flat)))
            (let* ((args (tree-filter-comma (tree.group-children (tree.call-args (cadr flat))))))
              (lain-quote `(resume ,(if (null? args) (optional.none)
                                        (optional.some (tree-lower-expr (car args)))))))
            (lain-quote `(resume ,(optional.none)))))
       ;; handle EffectName with handler { body }
       ((and (pair? flat) (tree.ident? (car flat)) (eq? (tree.ident-sym (car flat)) '|handle|))
        (tree-lower-handle-expr (cdr flat)))
       ;; otherwise: just take first
       (else (tree-lower-expr (car flat))))))
   ;; paren 透传
   ((tree.paren? node)
    (let ((children (tree.group-children node)))
      (if (null? children)
          (lain-quote '(unit))
          (tree-lower-expr (car children)))))
   ;; -> in expression context (shouldn't happen normally)
   ((and (pair? node) (eq? (car node) '|->|))
    (tree-lower-expr (tree.left node)))
   (else
    (lain-quote '(number 0)))))

(define (tree-filter-comma children)
  (filter (lambda (c) (not (and (tree.sep? c) (eq? (tree.sep-sym c) '|,|)))) children))

(define (tree-path-node-sym node)
  (cond
   ((tree.ident? node) (tree.ident-sym node))
   ((tree.call? node)
    (tree-path-node-sym (tree.call-callee node)))
   (else '_)))

(define (tree-path-syms node)
  (map tree-path-node-sym (tree.flatten-path node)))

(define (tree-lower-if-expr parts)
  ;; parts: [cond-tree (brace then) (ident else) (brace else)]
  (if (null? parts)
      (lain-quote '(number 0))
      (let* ((cond-expr (tree-lower-expr (car parts)))
             (rest (cdr parts))
             (then-block (if (and (pair? rest) (tree.brace? (car rest)))
                             (tree-parse-block (car rest))
                             (lain-quote '(block (tail (number 0))))))
             (after-then (if (and (pair? rest) (tree.brace? (car rest))) (cdr rest) rest))
             ;; 跳过 else 关键字
             (after-else (if (and (pair? after-then) (tree.ident? (car after-then))
                                   (eq? (tree.ident-sym (car after-then)) '|else|))
                              (cdr after-then)
                              after-then))
             (else-block (if (and (pair? after-else) (tree.brace? (car after-else)))
                             (tree-parse-block (car after-else))
                             (lain-quote '(block)))))
        (lain-quote `(if ,cond-expr ,then-block ,else-block)))))

(define (tree-lower-builtin-expr parts)
  ;; parts after @: [(ident name) ...] 或 [(call (ident name) (paren args...))]
  (if (null? parts) (lain-quote '(number 0))
      (let ((first (car parts)))
        (cond
         ((tree.call? first)
          (let* ((name (tree.ident-sym (tree.call-callee first)))
                 (arg-trees (tree-filter-comma (tree.group-children (tree.call-args first))))
                 (arg-exprs (map tree-lower-expr arg-trees)))
            (lain-quote `(builtin ,name ,@arg-exprs))))
         ((tree.ident? first)
          (let ((name (tree.ident-sym first)))
            (if (eq? name '|tail_call|)
                (if (null? (cdr parts))
                    (lain-quote '(number 0))
                    (lain-quote `(tail-call ,(tree-lower-expr (cadr parts)))))
                (lain-quote `(builtin ,name)))))
         (else (lain-quote '(number 0)))))))

;; ── match 表达式 ──

(define (tree-lower-match-expr parts)
  ;; parts: [scrutinee-node (brace arm1 (sep ,) arm2 ...)]
  (if (null? parts) (lain-quote '(number 0))
      (let* ((scrutinee (tree-lower-expr (car parts)))
             (brace (if (and (pair? (cdr parts)) (tree.brace? (cadr parts)))
                        (cadr parts) #f))
             (arms (if brace
                       (tree-parse-match-arms (tree.group-children brace) (list))
                       (list))))
        (raw.node! '|expr.match|
          (record '|expr.match|
            (record.field '|scrutinee| scrutinee)
            (record.field '|arms| arms))))))

;; ── handle 表达式 ──

(define (tree-lower-handle-expr parts)
  ;; handle EffectName with handler { body }
  ;; parts: [EffectName (ident with) handler-path (brace body)]
  ;; 或: [EffectName<T> (ident with) handler-path (brace body)]
  (if (null? parts) (lain-quote '(number 0))
      (let* ((effect-node (car parts))
             (effect-name (if (tree.ident? effect-node) (tree.ident-sym effect-node)
                              (if (and (pair? effect-node) (eq? (car effect-node) '|<|))
                                  (tree.ident-sym (tree.left effect-node))
                                  '_unknown)))
             (effect-args (if (and (pair? effect-node) (eq? (car effect-node) '|<|))
                              (tree-collect-type-args (tree.right effect-node))
                              (list)))
             ;; 找 with 后面的部分
             (rest (cdr parts))
             ;; 跳过 with
             (after-with (if (and (pair? rest) (tree.ident? (car rest))
                                   (eq? (tree.ident-sym (car rest)) '|with|))
                              (cdr rest) rest))
             ;; handler 和 body
             (handler-expr (if (null? after-with) (lain-quote '(number 0))
                               (tree-lower-expr (car after-with))))
             (body-parts (if (null? after-with) '() (cdr after-with)))
             (body (if (and (pair? body-parts) (tree.brace? (car body-parts)))
                       (tree-parse-block (car body-parts))
                       (lain-quote '(block (tail (number 0)))))))
        (lain-quote `(handle ,effect-name ,effect-args ,handler-expr ,body)))))
