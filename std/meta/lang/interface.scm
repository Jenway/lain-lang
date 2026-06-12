(meta-source "lang/interface")

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
         (node (raw.node! '|interface|
                 (record '|interface|
                   (record.field '|attrs| attrs)
                   (record.field '|generics| generics)
                   (record.field '|where| where)
                   (record.field '|methods| methods)))))
    (decl.define! '|interface| name node)))

(define-pass (raw-normalizer |interface| decl)
  (let* ((name (decl.name decl))
         (raw (decl.payload decl))
         (payload (raw.payload raw)))
    (middle.node! '|middle.interface|
      (record '|middle.interface|
        (record.field '|name| name)
        (record.field '|payload| payload)))))

;; ===========================================================================
;; Interface L1 降级 — 生成 VTable 结构体类型
;; ===========================================================================

;; 全局注册表: 存储 interface → methods 映射，供 impl lowerer 查询
(set! *interface-registry* (list))

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
          (record '|struct.core-field|
            (record.field '|name| (list.first method-names))
            (record.field '|type| (type.addr)))
          acc))))

;; core-declarer: 为 interface 生成 VTable 结构体类型
(define-pass (core-declarer |middle.interface| item)
  (let* ((payload (middle.payload item))
         (interface-payload (optional.value (record.get payload '|payload|)))
         (name (optional.value (record.get payload '|name|)))
         (methods (optional.value (record.get interface-payload '|methods|)))
         (method-names (interface.method-names methods (list)))
         (vtable-name (interface.vtable-name name)))
    ;; 1. 声明 VTable 结构体: { method1: addr, method2: addr, ... }
    (core.declare-struct!
      vtable-name
      (interface.vtable-fields method-names (list)))
    ;; 2. 注册到全局表，供 impl lowerer 查询
    (interface.register! name methods)))
