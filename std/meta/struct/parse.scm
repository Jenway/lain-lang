(meta-source "struct/parse")

;; ═══════════════════════════════════════════════════
;; Struct 语法解析 — 树结构版本
;; ═══════════════════════════════════════════════════

;; ── 从 brace 组解析字段: (: (ident name) type-tree) ──

(define (struct.parse-fields-tree children acc)
  (if (null? children)
      (list.reverse acc)
      (let* ((node (car children))
             (rest (cdr children)))
        (cond
         ;; 跳过分隔符
         ((tree.sep? node)
          (struct.parse-fields-tree rest acc))
         ;; (: name type) — 字段声明
         ((and (pair? node) (eq? (car node) '|:|))
          (let* ((name-node (tree.left node))
                 (type-node (tree.right node))
                 (name (tree.ident-sym name-node))
                 (ty (tree-parse-type type-node))
                 (field (raw.node! '|struct.field|
                          (record '|struct.field|
                            (record.field '|name| name)
                            (record.field '|type| ty)))))
            (struct.parse-fields-tree rest (list.cons field acc))))
         (else
          (struct.parse-fields-tree rest acc))))))

(define (struct.literal-fields-tree children acc)
  (if (null? children)
      (list.reverse acc)
      (let ((child (car children))
            (rest (cdr children)))
        (cond
         ((and (tree.sep? child) (eq? (tree.sep-sym child) '|,|))
          (struct.literal-fields-tree rest acc))
         ((and (pair? child) (eq? (car child) '|:|))
          (let* ((name-node (tree.left child))
                 (value-node (tree.right child))
                 (name (if (tree.ident? name-node) (tree.ident-sym name-node) '_unknown))
                 (field (list 'struct-field name (tree-lower-expr value-node))))
            (struct.literal-fields-tree rest (list.cons field acc))))
         (else
          (struct.literal-fields-tree rest acc))))))

(define-pass* 'form-parser '|struct| (lambda (form)
  (let* ((tree (form.tree form))
         (decos (form.decorators form))
         (attrs (tree-parse-attrs form))
         (parts (tree.flatten-juxt tree))
         ;; parts: [(ident struct), (ident Name), ..., (brace ...)]
         ;; 或: [(ident struct), (< (ident Name) (ident T)), ..., (brace ...)]
         (_kw (car parts))
         (rest (cdr parts))
         ;; 提取 name 和 generics
         (name-and-generics (tree-extract-name-and-generics (car rest)))
         (name (car name-and-generics))
         (generics (cdr name-and-generics))
         (rest2 (cdr rest))
         ;; 查找 where 子句和 brace body
         (where-and-body (tree-split-where-and-brace rest2))
         (where (car where-and-body))
         (body-node (cdr where-and-body))
         (fields (if body-node
                     (struct.parse-fields-tree (tree.group-children body-node) (list))
                     (list)))
         (inner-payload (record '|struct|
                          (record.field '|attrs| attrs)
                          (record.field '|generics| generics)
                          (record.field '|where| where)
                          (record.field '|fields| fields)))
         (unified (raw.node! '|let|
                    (record '|let|
                      (record.field '|name| name)
                      (record.field '|type-kind| '|struct|)
                      (record.field '|payload| inner-payload)))))
    (decl.define-dup-checked! '|let| name unified))))
