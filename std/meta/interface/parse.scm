(meta-source "interface/parse")

(register-constraint! '|Interface| '|interface-predicate|)
(register-interface-rule! '|Interface| '|interface-requirements|)

(define (interface.parse-method cursor)
  (let* ((name (syntax.cursor-expect-ident! cursor))
         (params (syntax.parse-params
                   (syntax.cursor-expect-group! cursor '|paren|)))
         (_arrow (syntax.cursor-expect-punct! cursor '|->|))
         (ret (syntax.parse-type cursor))
         (effects (syntax.parse-optional-effects cursor))
         (_semi (syntax.cursor-expect-punct! cursor '|;|)))
    (raw.node! '|interface.method|
      (record '|interface.method|
        (record.field '|name| name)
        (record.field '|params| params)
        (record.field '|return| ret)
        (record.field '|effects| effects)))))

(define (interface.parse-methods cursor acc)
  (let* ((next (syntax.cursor-match-ident! cursor)))
    (if (optional.none? next)
        (begin
          (syntax.cursor-expect-eof! cursor)
          (list.reverse acc))
        (let* ((method (interface.parse-method cursor)))
          (interface.parse-methods cursor (list.cons method acc))))))

(define-pass (form-parser |interface| form)
  (let* ((cursor (syntax.form-cursor form))
         (attrs (syntax.parse-attrs cursor (list)))
         (_kw (syntax.cursor-expect-ident! cursor))
         (name (syntax.cursor-expect-ident! cursor))
         (generics (syntax.parse-generic-params cursor))
         (where (syntax.parse-where cursor))
         (body (syntax.cursor-expect-group! cursor '|brace|))
         (_eof (syntax.cursor-expect-eof! cursor))
         (body-cursor (syntax.group-cursor body))
         (methods (interface.parse-methods body-cursor (list)))
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
    (decl.define! '|let| name unified)))

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
  (let loop ((reg *interface-registry*))
    (if (list.empty? reg)
        (list)  ;; 空列表 = 未找到
        (let* ((entry (list.first reg)))
          (if (symbol=? (car entry) name)
              (cdr entry)
              (loop (list.rest reg)))))))

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
  (let loop ((reg *dyn-registry*))
    (if (list.empty? reg)
        #f
        (let* ((entry (list.first reg)))
          (if (symbol=? (car entry) dyn-type-name)
              (cdr entry)
              (loop (list.rest reg)))))))

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
(define-pass (core-declarer |middle.interface| item)
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
    (interface.register-dyn! dyn-name name)))
