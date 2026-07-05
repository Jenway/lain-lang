(meta-source "interface/parse")

(register-constraint! '|Interface| '|interface-predicate|)
(register-interface-rule! '|Interface| '|interface-requirements|)

;; ── tree-based type parsing ──
;; Convert a tree node representing a type into lain-quote type AST.
(define (interface.parse-type-tree node)
  (cond
   ;; simple named type: (ident i32)
   ((tree.ident? node)
    (lain-quote `(type-path ,(tree.ident-sym node))))
   ;; unit type: (paren) — empty parens
   ((tree.paren? node)
    (if (null? (tree.group-children node))
        (lain-quote '(type-unit))
        ;; single-child paren is transparent
        (interface.parse-type-tree (car (tree.group-children node)))))
   ;; generic application: (< Name T) or (< Name (juxt T1 T2))
   ((and (pair? node) (eq? (car node) '|<|))
    (let* ((base (tree.ident-sym (tree.left node)))
           (args-node (tree.right node))
           (args (if (tree.juxt? args-node)
                     (map interface.parse-type-tree
                          (filter (lambda (n) (not (and (tree.sep? n) (eq? (tree.sep-sym n) '|,|))))
                                  (tree.flatten-juxt args-node)))
                     (list (interface.parse-type-tree args-node)))))
      (lain-quote `(type-app ,base ,args))))
   ;; reference: (prefix & inner)
   ((and (pair? node) (eq? (car node) 'prefix) (eq? (cadr node) '|&|))
    (let ((inner (interface.parse-type-tree (caddr node))))
      (lain-quote `(type-ref #f ,inner))))
   ;; pointer: (prefix * inner)
   ((and (pair? node) (eq? (car node) 'prefix) (eq? (cadr node) '|*|))
    (let ((inner (interface.parse-type-tree (caddr node))))
      (lain-quote `(type-raw-ptr #f ,inner))))
   (else
    (error "interface.parse-type-tree: unhandled node"))))

;; ── tree-based param parsing ──
;; Parse params from a paren group node: (paren (: name type) (sep ,) ...)
(define (interface.parse-params-tree paren-node)
  (let* ((children (tree.group-children paren-node))
         (filtered (filter (lambda (n) (not (and (tree.sep? n) (eq? (tree.sep-sym n) '|,|)))) children)))
    (map (lambda (child)
           (if (and (pair? child) (eq? (car child) '|:|))
               (let* ((name (tree.ident-sym (tree.left child)))
                      (ty (interface.parse-type-tree (tree.right child))))
                 (lain-quote `(param ,name ,ty)))
               (error "interface.parse-params-tree: expected colon annotation")))
         filtered)))

;; ── tree-based method signature parsing ──
;; A method in the brace body is a child tree like:
;;   (juxt (ident fn) (juxt (-> (call name (paren params...)) ret-type) (sep ;)))
;; or without return type:
;;   (juxt (ident fn) (juxt (call name (paren params...)) (sep ;)))
(define (interface.parse-method-tree node)
  (let* ((parts (tree.flatten-juxt node))
         ;; skip (ident fn) keyword and trailing (sep ;)
         (no-kw (cdr parts))  ;; drop fn keyword
         ;; find the core signature part (everything before trailing ;)
         (sig-parts (filter (lambda (n) (not (and (tree.sep? n) (eq? (tree.sep-sym n) '|;|)))) no-kw)))
    ;; sig-parts should be a single node: either (-> (call name params) ret) or (call name params)
    (let ((sig (if (= (length sig-parts) 1)
                   (car sig-parts)
                   ;; multiple parts remaining — reassemble as the first element
                   (car sig-parts))))
      (cond
       ;; with return type: (-> (call name (paren ...)) ret-type)
       ((and (pair? sig) (eq? (car sig) '|->|))
        (let* ((lhs (tree.left sig))
               (ret-node (tree.right sig))
               (ret (interface.parse-type-tree ret-node)))
          (interface.parse-method-call-part lhs ret)))
       ;; function call without return type
       ((tree.call? sig)
        (interface.parse-method-call-part sig (lain-quote '(type-unit))))
       (else
        (error "interface.parse-method-tree: unexpected method shape"))))))

(define (interface.parse-method-call-part call-node ret)
  (let* ((callee (tree.call-callee call-node))
         (name (tree.ident-sym callee))
         (args-paren (tree.call-args call-node))
         (params (interface.parse-params-tree args-paren)))
    (raw.node! '|interface.method|
      (record '|interface.method|
        (record.field '|name| name)
        (record.field '|params| params)
        (record.field '|return| ret)
        (record.field '|effects| (optional.none))))))

;; Parse all method trees from brace body children
(define (interface.parse-methods-tree children acc)
  (if (null? children)
      (list.reverse acc)
      (let ((child (car children)))
        (if (and (tree.sep? child) (eq? (tree.sep-sym child) '|;|))
            ;; skip standalone semicolons
            (interface.parse-methods-tree (cdr children) acc)
            (interface.parse-methods-tree
              (cdr children)
              (list.cons (interface.parse-method-tree child) acc))))))

;; ── tree-based generic params parsing ──
;; generics appear as (< Name (juxt T1 (juxt T2 ...))) where the < node
;; wraps the name+brace. We detect this in the flattened top-level parts.
(define (interface.parse-generics-from-name-node node)
  ;; If node is (< name T), extract generic params; else return empty list + name
  (if (and (pair? node) (eq? (car node) '|<|))
      (let* ((name-node (tree.left node))
             (name (tree.ident-sym name-node))
             (params-node (tree.right node))
             (param-nodes (if (tree.juxt? params-node)
                              (filter (lambda (n)
                                        (not (and (tree.sep? n) (eq? (tree.sep-sym n) '|,|))))
                                      (tree.flatten-juxt params-node))
                              (list params-node)))
             (params (map tree.ident-sym param-nodes)))
        (cons name params))
      (cons (tree.ident-sym node) (list))))

;; ── tree-based where clause parsing ──
;; where T: Bound appears as additional juxt parts: (ident where) (: T Bound)
(define (interface.parse-where-parts parts)
  (if (null? parts) (list)
      (let ((head (car parts)))
        (if (and (tree.ident? head) (eq? (tree.ident-sym head) '|where|))
            (interface.parse-where-predicates (cdr parts) (list))
            (list)))))

(define (interface.parse-where-predicates parts acc)
  (if (null? parts) (list.reverse acc)
      (let ((node (car parts)))
        (if (and (pair? node) (eq? (car node) '|:|))
            (let* ((param (tree.ident-sym (tree.left node)))
                   (bound (interface.parse-type-tree (tree.right node)))
                   (predicate (lain-quote `(where-predicate ,param ,bound))))
              (interface.parse-where-predicates
                (filter (lambda (n) (not (and (tree.sep? n) (eq? (tree.sep-sym n) '|,|))))
                        (cdr parts))
                (list.cons predicate acc)))
            (list.reverse acc)))))

(define-pass* 'form-parser '|interface| (lambda (form)
  (let* ((tree (form.tree form))
         (attrs (form.decorators form))
         (parts (tree.flatten-juxt tree))
         ;; parts: ((ident interface) name-or-generic-node ... maybe-where ... (brace ...))
         ;; skip keyword
         (rest (cdr parts))
         ;; last element is the brace body
         (brace (car (reverse rest)))
         (mid-parts (reverse (cdr (reverse rest))))
         ;; first mid-part is name (possibly with generics via < node)
         (name-node (car mid-parts))
         (name+generics (interface.parse-generics-from-name-node name-node))
         (name (car name+generics))
         (generics (cdr name+generics))
         ;; remaining mid-parts may contain where clause
         (where-rest (cdr mid-parts))
         (where (interface.parse-where-parts where-rest))
         ;; parse methods from brace body
         (body-children (tree.group-children brace))
         (methods (interface.parse-methods-tree body-children (list)))
         (inner-payload (record '|interface|
                          (record.field '|attrs| attrs)
                          (record.field '|generics| generics)
                          (record.field '|where| where)
                          (record.field '|methods| methods)))
         (unified (raw.node! '|let|
                    (record '|let|
                      (record.field '|name| name)
                      (record.field '|type-kind| '|interface|)
                      (record.field '|payload| inner-payload)))))
    (decl.define! '|let| name unified))))

;; interface raw-normalizer 已迁至 let/normalize.scm 的统一分发器

;; ===========================================================================
;; Interface L1 降级 — 生成 VTable 结构体类型 + Dyn 胖指针类型
;; ===========================================================================

;; 全局注册表: 存储 interface → methods 映射，供 impl lowerer 查询
(define *interface-registry* (list))

;; Dyn 类型注册表: 存储 dyn-type-name → (interface-name . method-index-offsets)
(define *dyn-registry* (list))

;; 帮助函数: 将 interface 注册到全局表
(define (interface.register! name methods)
  (set! *interface-registry*
    (list.cons
      (list.cons name methods)
      *interface-registry*)))

;; 帮助函数: 查询 interface 的 methods
(define (interface.lookup name)
  (let ((loop #f))
  (set! loop (lambda (reg)
    (if (list.empty? reg)
        (list)  ;; 空列表 = 未找到
        (let* ((entry (list.first reg)))
          (if (symbol=? (car entry) name)
              (cdr entry)
              (loop (list.rest reg)))))))
  (loop *interface-registry*)))

;; Dyn 胖指针类型名: Animal → Animal_Dyn
(define (interface.dyn-name interface-name)
  (let* ((s (symbol->string interface-name))
         (combined (string-append s "_Dyn")))
    (string->symbol combined)))

;; 注册 Dyn 类型到全局表
(define (interface.register-dyn! dyn-name interface-name)
  (set! *dyn-registry*
    (list.cons
      (list.cons dyn-name interface-name)
      *dyn-registry*)))

;; 查询 Dyn 类型对应的 interface name
(define (interface.lookup-dyn dyn-type-name)
  (let ((loop #f))
  (set! loop (lambda (reg)
    (if (list.empty? reg)
        #f
        (let* ((entry (list.first reg)))
          (if (symbol=? (car entry) dyn-type-name)
              (cdr entry)
              (loop (list.rest reg)))))))
  (loop *dyn-registry*)))

;; 判断一个类型是否为 Dyn 胖指针类型
(define (interface.dyn-type? ty)
  (let* ((name (type.product-name ty)))
    (if name
        (interface.lookup-dyn name)
        #f)))

;; 从 Dyn 类型名提取 interface 名: Animal_Dyn → Animal
(define (interface.name-from-dyn dyn-name)
  (let* ((str (symbol->string dyn-name))
         (len (string-length str)))
    (string->symbol (substring str 0 (- len 4)))))

;; 在 interface methods 列表中查找方法名对应的索引
(define (interface.find-method-index name methods index)
  (if (list.empty? methods)
      #f  ;; 未找到
      (let* ((method (list.first methods))
             (payload (raw.payload method))
             (method-name (optional.value (record.get payload '|name|))))
        (if (symbol=? method-name name)
            index
            (interface.find-method-index name (list.rest methods) (u64.add1 index))))))

;; 帮助函数: 从 raw method 节点列表中提取 method name (符号)
(define (interface.method-names methods acc)
  (if (list.empty? methods)
      (list.reverse acc)
      (let* ((method (list.first methods))
             (payload (raw.payload method))
             (name (optional.value (record.get payload '|name|))))
        (interface.method-names
          (list.rest methods)
          (list.cons name acc)))))

;; 构造 VTable 结构体名: InterfaceName_VTable
(define (interface.vtable-name interface-name)
  (let* ((s (symbol->string interface-name))
         (combined (string-append s "_VTable")))
    (string->symbol combined)))

;; 帮助函数: 创建 vtable 字段描述 (每个方法 → addr 类型的函数指针槽位)
(define (interface.vtable-fields method-names acc)
  (if (list.empty? method-names)
      (list.reverse acc)
      (interface.vtable-fields
        (list.rest method-names)
        (list.cons
          (cons (list.first method-names) (type.addr))
          acc))))

;; core-declarer: 为 interface 生成 VTable 结构体类型 + Dyn 胖指针类型
(define-pass* 'core-declarer '|middle.interface| (lambda (item)
  (let* ((payload (middle.payload item))
         (interface-payload (optional.value (record.get payload '|payload|)))
         (name (optional.value (record.get payload '|name|)))
         (methods (optional.value (record.get interface-payload '|methods|)))
         (method-names (interface.method-names methods (list)))
         (vtable-name (interface.vtable-name name))
         (dyn-name (interface.dyn-name name)))
    ;; 1. 声明 VTable 结构体: { method1: addr, method2: addr, ... }
    (struct-register!
      vtable-name
      (interface.vtable-fields method-names (list)))
    ;; 2. 声明 Dyn 胖指针结构体: { data: addr, vtable: addr }
    (struct-register!
      dyn-name
      (list
        (cons '|data| (type.addr))
        (cons '|vtable| (type.addr))))
    ;; 3. 注册到全局表，供 impl lowerer 查询
    (interface.register! name methods)
    ;; 4. 注册 Dyn 类型映射
    (interface.register-dyn! dyn-name name))))
