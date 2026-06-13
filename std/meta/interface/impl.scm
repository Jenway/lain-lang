(meta-source "interface/impl")

;; impl 专用的方法解析器 — 支持带函数体的方法实现
;; 接口声明用 `;` 结束, impl 方法实现用 `{ ... }` 结束
(define (impl.parse-method cursor)
  (let* ((name (syntax.cursor-expect-ident! cursor))
         (params (syntax.parse-params
                   (syntax.cursor-expect-group! cursor '|paren|)))
         (_arrow (syntax.cursor-expect-punct! cursor '|->|))
         (ret (syntax.parse-type cursor))
         (effects (syntax.parse-optional-effects cursor))
         (body (syntax.parse-optional-fn-body cursor)))
    (raw.node! '|impl.method|
      (record '|impl.method|
        (record.field '|name| name)
        (record.field '|params| params)
        (record.field '|return| ret)
        (record.field '|effects| effects)
        (record.field '|body| body)))))

(define (impl.parse-methods cursor acc)
  (let* ((next (syntax.cursor-match-ident! cursor)))
    (if (optional.none? next)
        (begin
          (syntax.cursor-expect-eof! cursor)
          (list.reverse acc))
        (let* ((method (impl.parse-method cursor)))
          (impl.parse-methods cursor (list.cons method acc))))))

(define-pass (form-parser |impl| form)
  (let* ((cursor (syntax.form-cursor form))
         (attrs (syntax.parse-attrs cursor (list)))
         (_kw (syntax.cursor-expect-ident! cursor))
         (generics (syntax.parse-generic-params cursor))
         (interface-name (syntax.cursor-expect-ident! cursor))
         (interface (raw.node! '|type.path|
                      (record '|type.path|
                        (record.field '|name| interface-name))))
         (_for (syntax.cursor-expect-ident! cursor))
         (target (syntax.parse-type cursor))
         (where (syntax.parse-where cursor))
         (body (syntax.cursor-expect-group! cursor '|brace|))
         (_eof (syntax.cursor-expect-eof! cursor))
         (body-cursor (syntax.group-cursor body))
         (methods (impl.parse-methods body-cursor (list)))
         (node (raw.node! '|impl|
                 (record '|impl|
                   (record.field '|attrs| attrs)
                   (record.field '|generics| generics)
                   (record.field '|interface| interface)
                   (record.field '|target| target)
                   (record.field '|where| where)
                   (record.field '|methods| methods)))))
    (decl.define! '|impl| interface-name node)))

(define-pass (raw-normalizer |impl| decl)
  (middle.normalize-plain-decl decl '|middle.impl|))

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
