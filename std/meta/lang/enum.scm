(meta-source "lang/enum")

(define (enum.parse-variants cursor acc)
  (let* ((name (syntax.cursor-match-ident! cursor)))
    (if (optional.none? name)
        (begin
          (syntax.cursor-expect-eof! cursor)
          (list.reverse acc))
        (let* ((comma (syntax.cursor-match-punct! cursor '|,|))
               (variant (raw.node! '|enum.variant|
                          (record '|enum.variant|
                            (record.field '|name| (optional.value name))))))
          (enum.parse-variants cursor (list.cons variant acc))))))

(define-pass (form-parser |enum| form)
  (let* ((cursor (syntax.form-cursor form))
         (attrs (syntax.parse-attrs cursor (list)))
         (_kw (syntax.cursor-expect-ident! cursor))
         (name (syntax.cursor-expect-ident! cursor))
         (generics (syntax.parse-generic-params cursor))
         (body (syntax.cursor-expect-group! cursor '|brace|))
         (_eof (syntax.cursor-expect-eof! cursor))
         (body-cursor (syntax.group-cursor body))
         (variants (enum.parse-variants body-cursor (list)))
         (node (raw.node! '|enum|
                 (record '|enum|
                   (record.field '|attrs| attrs)
                   (record.field '|generics| generics)
                   (record.field '|variants| variants)))))
    (decl.define! '|enum| name node)))

(define-pass (raw-normalizer |enum| decl)
  (middle.normalize-plain-decl decl '|middle.enum|))

;; ===========================================================================
;; Enum L1 降级 — 将 enum 降级为 Tagged Union 结构体
;; ===========================================================================

;; 帮助函数: 创建 struct 字段描述 (用于 core.declare-struct!)
(define (enum.make-field name ty)
  (record '|struct.core-field|
    (record.field '|name| name)
    (record.field '|type| ty)))

;; 构造 variant 构造函数名 (C 兼容): EnumName_VariantName
;; 内部 Scheme 符号使用 "::" 分隔符便于查找，
;; 但 C 函数名必须使用 "_" (:: 不是合法的 C 标识符)
(define (enum.ctor-name enum-name variant-name)
  (let* ((e-str (symbol->string enum-name))
         (v-str (symbol->string variant-name))
         (combined (string-append e-str "_" v-str)))
    (string->symbol combined)))

;; Phase 1: core-declarer — 声明 tagged union 结构体 + variant 构造函数签名
(define (enum.declare-variant-ctors enum-name variants index)
  (if (list.empty? variants)
      unit
      (let* ((variant (list.first variants))
             (variant-payload (raw.payload variant))
             (variant-name (optional.value
                             (record.get variant-payload '|name|)))
             (ctor-name (enum.ctor-name enum-name variant-name))
             (enum-ty (core.struct-type enum-name)))
        (core.begin-function!
          ctor-name
          (list)     ;; 无参数 (后续 payload enum 会添加参数)
          enum-ty)   ;; 返回 enum 类型
        (enum.declare-variant-ctors
          enum-name
          (list.rest variants)
          (u64.add1 index)))))

(define-pass (core-declarer |middle.enum| item)
  (let* ((payload (middle.payload item))
         (enum-payload (optional.value (record.get payload '|payload|)))
         (name (optional.value (record.get payload '|name|)))
         (generics (optional.value (record.get enum-payload '|generics|)))
         (variants (optional.value (record.get enum-payload '|variants|))))
    ;; 1. 声明 tagged union 结构体类型
    ;;    布局: { tag: bits<8>, __data: addr }
    ;;    tag   — 辨別子 (discriminant), 8-bit 足够 256 个变体
    ;;    __data — 载荷数据 blob, 使用 addr 尺寸存放任意指针/值
    ;;    注: 当没有载荷时, __data 字段忽略 (零值填充)
    (core.declare-struct!
      name
      (list
        (enum.make-field '|tag| (type.bits 8))
        (enum.make-field '|__data| (type.addr))))
    ;; 2. 为每个 variant 注册构造函数 (无参数函数, 返回 enum 值)
    (enum.declare-variant-ctors name variants 0)))

;; Phase 2: core-lowerer — 生成 variant 构造函数的函数体
;;   每个构造函数:
;;     1. alloca 分配 enum 结构体空间
;;     2. 用 core.aggregate! 初始化 { tag: discriminant, __data: null }
;;     3. 返回 alloca 指针
(define (enum.lower-variant-ctors enum-name variants index)
  (if (list.empty? variants)
      unit
      (let* ((variant (list.first variants))
             (variant-payload (raw.payload variant))
             (variant-name (optional.value
                             (record.get variant-payload '|name|)))
             (ctor-name (enum.ctor-name enum-name variant-name))
             (fn (core.function-by-name ctor-name))
             (block (core.append-block! fn))
             (enum-ty (core.struct-type enum-name)))
        ;; 构造聚合值: { tag = discriminant, __data = 0 }
        (let* ((tag-val (core.const-bits! block (type.bits 8) index))
               (data-val (core.const-bits! block (type.addr) 0)))
          ;; core.aggregate! 会生成 alloca + 各字段 store + 返回 VAR 表达式
          (core.return-value!
            block
            (core.aggregate! block enum-ty (list tag-val data-val))))
        (enum.lower-variant-ctors
          enum-name
          (list.rest variants)
          (u64.add1 index)))))

(define-pass (core-lowerer |middle.enum| item)
  (let* ((payload (middle.payload item))
         (enum-payload (optional.value (record.get payload '|payload|)))
         (name (optional.value (record.get payload '|name|)))
         (variants (optional.value (record.get enum-payload '|variants|))))
    (enum.lower-variant-ctors name variants 0)))
