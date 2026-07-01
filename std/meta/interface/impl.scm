(meta-source "interface/impl")

;; ── tree-based impl method parsing ──
;; An impl method has a body (brace block) instead of trailing semicolon.
;; Tree shape: (juxt (ident fn) (juxt (-> (call name (paren params)) ret) (brace body...)))
;; or without ->: (juxt (ident fn) (juxt (call name (paren params)) (brace body...)))

(define (impl.parse-method-tree node)
  (let* ((parts (tree.flatten-juxt node))
         ;; drop fn keyword
         (no-kw (cdr parts))
         ;; separate trailing brace (body) if present
         (last (car (reverse no-kw)))
         (has-body (tree.brace? last))
         (sig-parts (if has-body (reverse (cdr (reverse no-kw))) no-kw))
         (body (if has-body
                   (optional.some (tree-parse-block last))
                   (optional.none)))
         ;; sig-parts should be one node: (-> (call name params) ret) or (call name params)
         (sig (car sig-parts)))
    (cond
     ;; with return type
     ((and (pair? sig) (eq? (car sig) '|->|))
      (let* ((lhs (tree.left sig))
             (ret (interface.parse-type-tree (tree.right sig))))
        (impl.parse-method-call-part lhs ret body)))
     ;; call without return type
     ((tree.call? sig)
      (impl.parse-method-call-part sig (lain-quote '(type-unit)) body))
     (else
      (error "impl.parse-method-tree: unexpected method shape")))))

(define (impl.parse-method-call-part call-node ret body)
  (let* ((callee (tree.call-callee call-node))
         (name (tree.ident-sym callee))
         (args-paren (tree.call-args call-node))
         (params (interface.parse-params-tree args-paren)))
    (raw.node! '|impl.method|
      (record '|impl.method|
        (record.field '|name| name)
        (record.field '|params| params)
        (record.field '|return| ret)
        (record.field '|effects| (optional.none))
        (record.field '|body| body)))))

;; Parse all method trees from brace body children
(define (impl.parse-methods-tree children acc)
  (if (null? children)
      (list.reverse acc)
      (let ((child (car children)))
        (if (and (tree.sep? child))
            ;; skip separators (semicolons)
            (impl.parse-methods-tree (cdr children) acc)
            (impl.parse-methods-tree
              (cdr children)
              (list.cons (impl.parse-method-tree child) acc))))))

(define-pass (form-parser |impl| form)
  (let* ((tree (form.tree form))
         (attrs (form.decorators form))
         (parts (tree.flatten-juxt tree))
         ;; parts: ((ident impl) maybe-generic interface-name (ident for) target ... (brace ...))
         ;; skip keyword
         (rest (cdr parts))
         ;; last element is the brace body
         (brace (car (reverse rest)))
         (mid-parts (reverse (cdr (reverse rest))))
         ;; Find "for" keyword to split interface-name and target
         ;; Everything before "for" (possibly with generics) is interface side
         ;; Everything after "for" until brace is target side
         (split (impl.split-at-for mid-parts))
         (before-for (car split))
         (after-for (cdr split))
         ;; generics: check if first element is a < node
         (first-part (car before-for))
         (generics (if (and (pair? first-part) (eq? (car first-part) '|<|))
                       ;; generic params on the impl itself (rare)
                       (list)  ;; TODO: handle impl<T> generics
                       (list)))
         ;; interface name (possibly with generics via < node)
         (interface-name-node (car before-for))
         (interface-name (if (and (pair? interface-name-node) (eq? (car interface-name-node) '|<|))
                             (tree.ident-sym (tree.left interface-name-node))
                             (tree.ident-sym interface-name-node)))
         (interface (raw.node! '|type.path|
                      (record '|type.path|
                        (record.field '|name| interface-name))))
         ;; target type
         (target-node (car after-for))
         (target (interface.parse-type-tree target-node))
         ;; where clause (remaining after-for parts after target)
         (where-rest (cdr after-for))
         (where (interface.parse-where-parts where-rest))
         ;; parse methods from brace body
         (body-children (tree.group-children brace))
         (methods (impl.parse-methods-tree body-children (list)))
         (inner-payload (record '|impl|
                          (record.field '|attrs| attrs)
                          (record.field '|generics| generics)
                          (record.field '|interface| interface)
                          (record.field '|target| target)
                          (record.field '|where| where)
                          (record.field '|methods| methods)))
         (unified (raw.node! '|let|
                    (record '|let|
                      (record.field '|name| interface-name)
                      (record.field '|type-kind| '|impl|)
                      (record.field '|payload| inner-payload)))))
    (decl.define! '|let| interface-name unified)))

;; Helper: split a list at the (ident for) element
(define (impl.split-at-for parts)
  (impl.split-at-for-loop parts (list)))

(define (impl.split-at-for-loop parts acc)
  (if (null? parts)
      (cons (list.reverse acc) (list))  ;; no "for" found
      (let ((head (car parts)))
        (if (and (tree.ident? head) (eq? (tree.ident-sym head) '|for|))
            (cons (list.reverse acc) (cdr parts))
            (impl.split-at-for-loop (cdr parts) (cons head acc))))))

;; impl raw-normalizer 已迁至 let/normalize.scm 的统一分发器

;; ===========================================================================
;; Impl L1 降级 — 生成 VTable 实例 (工厂函数)
;; ===========================================================================

;; 帮助: 从 raw method 节点提取方法名
(define (impl.method-name method)
  (let* ((payload (raw.payload method)))
    (optional.value (record.get payload '|name|))))

;; 帮助: 构造 VTable 实例工厂函数名
;; impl Animal for Dog → Animal_VTable_for_Dog
(define (impl.vtable-instance-name interface-name target-name)
  (let* ((i-str (symbol->string interface-name))
         (t-str (symbol->string target-name))
         (combined (string-append i-str "_VTable_for_" t-str)))
    (string->symbol combined)))

;; 帮助: 构造目标 struct 的方法函数名 (遵循现有命名约定)
;; Dog::speak → Dog_speak
(define (impl.method-fn-name target-name method-name)
  (let* ((t-str (symbol->string target-name))
         (m-str (symbol->string method-name))
         (combined (string-append t-str "_" m-str)))
    (string->symbol combined)))

;; core-declarer: 注册 VTable 实例工厂函数签名
(define-pass (core-declarer |middle.impl| item)
  (let* ((payload (middle.payload item))
         (impl-payload (optional.value (record.get payload '|payload|)))
         (interface-raw (optional.value (record.get impl-payload '|interface|)))
         (interface-payload (raw.payload interface-raw))
         (interface-name (optional.value (record.get interface-payload '|name|)))
         (target-raw (optional.value (record.get impl-payload '|target|)))
         (target-payload (raw.payload target-raw))
         (target-name (optional.value (record.get target-payload '|name|)))
         (vtable-name (interface.vtable-name interface-name))
         (instance-name (impl.vtable-instance-name interface-name target-name))
         (interface-methods (interface.lookup interface-name)))
    ;; 注册工厂函数: fn ImplTarget_VTable_for_InterfaceName() -> VTableType
    ;; vtable is a struct → pass addr to C, store struct-type in Scheme
    (let ((vtable-ty (struct-type vtable-name)))
      (fn-return-type! instance-name vtable-ty)
      (core.begin-function! instance-name (list) (type.addr)))))

;; 帮助: 在 interface methods 列表中查找方法名对应的索引
(define (impl.find-method-index name methods index)
  (if (list.empty? methods)
      index  ;; fallback: return current index
      (let* ((method (list.first methods))
             (payload (raw.payload method))
             (method-name (optional.value (record.get payload '|name|))))
        (if (symbol=? method-name name)
            index
            (impl.find-method-index name (list.rest methods) (u64.add1 index))))))

;; core-lowerer: 主入口 — 生成完整的 VTable 实例工厂函数
(define-pass (core-lowerer |middle.impl| item)
  (let* ((payload (middle.payload item))
         (impl-payload (optional.value (record.get payload '|payload|)))
         (interface-raw (optional.value (record.get impl-payload '|interface|)))
         (interface-payload (raw.payload interface-raw))
         (interface-name (optional.value (record.get interface-payload '|name|)))
         (target-raw (optional.value (record.get impl-payload '|target|)))
         (target-payload (raw.payload target-raw))
         (target-name (optional.value (record.get target-payload '|name|)))
         (methods (optional.value (record.get impl-payload '|methods|)))
         (vtable-name (interface.vtable-name interface-name))
         (instance-name (impl.vtable-instance-name interface-name target-name))
         (interface-methods (interface.lookup interface-name)))
    (if (list.empty? interface-methods)
        unit  ;; interface 未注册 — 跳过 (实际应由 core-declarer 保证)
        (let* ((fn (core.function-by-name instance-name))
               (block (core.append-block! fn)))
          ;; 为每个 impl method 计算函数指针并填入 vtable
          (impl.lower-vtable-body
            block target-name methods interface-methods)))))

;; 生成 vtable 工厂函数体:
;;   alloca vtable → store fn pointers → return vtable ptr
;; VTable layout: flat array of addr (8-byte) slots — one per interface method
(define (impl.lower-vtable-body block target-name methods interface-methods)
  (let* ((field-values (impl.build-vtable-field-values target-name methods interface-methods (list)))
         (method-count (length field-values))
         ;; Each slot is 8 bytes (addr). Total size = count * 8
         (total-size (* method-count 8))
         (pairs (let loop ((vals field-values) (i 0) (acc '()))
                  (if (null? vals)
                      (reverse acc)
                      (loop (cdr vals) (+ i 1)
                            (cons (cons (* i 8) (car vals)) acc))))))
     (core.return-value!
      block
      (core.aggregate-layout! block total-size pairs))))

;; ===========================================================================
;; Impl 动态分发降级 — vtable 查找 + 间接调用
;; ===========================================================================

;; 胖指针结构: { data: addr (field 0, offset 0), vtable: addr (field 1, offset 8) }
;; 降级路径:
;;   1. Lower 接收者 → fat-ptr 表达式
;;   2. core.field-offset!(block, fat-ptr, 0, addr) → data-ptr
;;   3. core.field-offset!(block, fat-ptr, 8, addr) → vtable-ptr
;;   4. 查找 method 在 interface 中的索引
;;   5. core.field-offset!(block, vtable-ptr, method-index*8, addr) → fn-ptr
;;   6. core.call-indirect!(block, fn-ptr, ret-ty, (data-ptr . args))
(define (impl.lower-dyn-dispatch! block receiver method args locals)
  (let* ((receiver-payload (middle.payload receiver))
         (receiver-path (optional.value (record.get receiver-payload '|path|)))
         ;; receiver-name 即变量名
         (_receiver-name (list.first receiver-path))
         ;; 从 locals 获取接收者的 Dyn 类型
         (dyn-ty (core.local-type locals (list.first receiver-path)))
         ;; Dyn 类型名 → Interface 名
         (dyn-type-name (type.product-name dyn-ty))
         (interface-name (interface.name-from-dyn dyn-type-name))
         ;; VTable 类型
         (vtable-name (interface.vtable-name interface-name))
         ;; Interface 方法列表
         (interface-methods (interface.lookup interface-name)))
    ;; 查找方法在 interface 中的索引
    (let* ((method-index (interface.find-method-index method interface-methods 0)))
      (if (eq? method-index #f)
          (type.unsupported '|dyn-method-not-found-in-interface|)
          ;; Lower 接收者为 fat-ptr 表达式
          (let* ((fat-ptr (core.lower-expr block receiver dyn-ty locals)))
            ;; Fat pointer layout: {data:addr@0, vtable:addr@8}
            ;; 提取 data 指针 (field 0, offset 0)
            (let* ((data-ptr (core.field-offset! block fat-ptr 0 (type.addr))))
              ;; 提取 vtable 指针 (field 1, offset 8)
              (let* ((vtable-ptr (core.field-offset! block fat-ptr 8 (type.addr))))
                ;; 从 vtable 中提取函数指针 (offset = method-index * 8)
                (let* ((fn-ptr (core.field-offset! block vtable-ptr (* method-index 8) (type.addr))))
                  ;; 间接调用: fn_ptr(data_ptr, ...args)
                  ;; 保守参数类型: 所有参数均为 addr (vtable 中所有槽位都是 addr)
                  (let* ((lowered-args (core.lower-args block args (list) locals (list))))
                    (core.call-indirect!
                      block
                      fn-ptr
                      (type.addr)  ;; 返回类型 (保守默认)
                      (list.cons data-ptr lowered-args)))))))))))

;; 构建 vtable 字段值列表 (与 interface 定义顺序对齐)
(define (impl.build-vtable-field-values target-name methods interface-methods acc)
  (if (list.empty? interface-methods)
      (list.reverse acc)
      (let* ((if-method (list.first interface-methods))
             (if-payload (raw.payload if-method))
             (if-name (optional.value (record.get if-payload '|name|)))
             (target-fn-name (impl.method-fn-name target-name if-name))
             (fn-ptr (core.function-ref! target-fn-name)))
        (impl.build-vtable-field-values
          target-name
          methods
          (list.rest interface-methods)
          (list.cons fn-ptr acc)))))
