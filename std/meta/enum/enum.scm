(meta-source "enum/enum")

;; 解析可选的 variant 载荷类型: VariantName(Type) 或 VariantName
(define (enum.parse-optional-payload cursor)
  (let* ((group (syntax.cursor-match-group! cursor '|paren|))
         (has-payload (optional.some? group)))
    (if has-payload
        (let* ((body-cursor (syntax.group-cursor (optional.value group)))
               (ty (syntax.parse-type body-cursor))
               (_eof (syntax.cursor-expect-eof! body-cursor)))
          (optional.some ty))
        (optional.none))))

(define (enum.parse-variants cursor acc)
  (let* ((name (syntax.cursor-match-ident! cursor)))
    (if (optional.none? name)
        (begin
          (syntax.cursor-expect-eof! cursor)
          (list.reverse acc))
        (let* ((payload (enum.parse-optional-payload cursor))
               (_comma (syntax.cursor-match-punct! cursor '|,|))
               (variant (raw.node! '|enum.variant|
                          (record '|enum.variant|
                            (record.field '|name| (optional.value name))
                            (record.field '|payload| payload)))))
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
         (inner-payload (record '|enum|
                          (record.field '|attrs| attrs)
                          (record.field '|generics| generics)
                          (record.field '|variants| variants)))
         (unified (raw.node! '|let|
                    (record '|let|
                      (record.field '|name| name)
                      (record.field '|type-kind| '|enum|)
                      (record.field '|payload| inner-payload)))))
    (decl.define! '|let| name unified)))

;; enum raw-normalizer 已迁至 let/normalize.scm 的统一分发器

;; ===========================================================================
;; Enum L1 降级 — 将 enum 降级为 Tagged Union 结构体
;;
;; 架构: 使用 lain-quote 声明式构建构造函数体 AST，
;;       然后通过标准 pipeline (normalize → lower) 发射 L1 指令。
;;       这样 enum.scm 完全自治，不依赖 core-lowerer 的 cond 分支。
;; ===========================================================================

;; 帮助函数: 创建 struct 字段描述 (用于 core.declare-struct!)
(define (enum.make-field name ty)
  (record '|struct.core-field|
    (record.field '|name| name)
    (record.field '|type| ty)))

;; 构造 variant 构造函数名 (C 兼容): EnumName_VariantName
(define (enum.ctor-name enum-name variant-name)
  (let* ((e-str (symbol->string enum-name))
         (v-str (symbol->string variant-name))
         (combined (string-append e-str "_" v-str)))
    (string->symbol combined)))

;; 帮助函数: 将 raw type 节点解析为 core type (用于构造函数参数)
(define (enum.resolve-type raw-ty)
  (let* ((payload (raw.payload raw-ty))
         (name (optional.value (record.get payload '|name|))))
    (type.registered name (list))))

;; 为带载荷的 variant 构造函数绑定参数到 locals
;; 这允许 lain-quote 生成的 (path _payload) 在 lowering 时正确解析到函数参数
(define (enum.bind-ctor-param fn variant)
  (let* ((variant-payload (raw.payload variant))
         (payload-raw-ty (optional.value
                           (record.get variant-payload '|payload|)))
         (payload-mid-ty (middle.normalize-type payload-raw-ty))
         (mid-param (middle.node! '|middle.param|
                      (record '|middle.param|
                        (record.field '|name| '|_payload|)
                        (record.field '|type| payload-mid-ty)))))
    (core.bind-params fn (list mid-param) 0 (list))))

;; ── lain-quote 神器: 声明式构建构造函数体 AST ──
;; variant 无载荷: return EnumName { tag: N, __data: 0 }
;; variant 有载荷: return EnumName { tag: N, __data: _payload }
(define (enum.build-ctor-body-ast enum-name index has-payload)
  (if (optional.some? has-payload)
      (lain-quote
       `(return
          (aggregate ,enum-name
            (struct-field tag (number ,index))
            (struct-field __data (path _payload)))))
      (lain-quote
       `(return
          (aggregate ,enum-name
            (struct-field tag (number ,index))
            (struct-field __data (number 0)))))))

;; Phase 1: core-declarer — 声明 tagged union 结构体 + variant 构造函数签名
(define (enum.declare-variant-ctors enum-name variants index)
  (if (list.empty? variants)
      unit
      (let* ((variant (list.first variants))
             (variant-payload (raw.payload variant))
             (variant-name (optional.value
                             (record.get variant-payload '|name|)))
             (ctor-name (enum.ctor-name enum-name variant-name))
             (enum-ty (struct-type enum-name))
             (payload (record.get variant-payload '|payload|))
             (param-types (if (optional.some? payload)
                              (list (enum.resolve-type
                                      (optional.value payload)))
                              (list))))
        (core.begin-function!
          ctor-name
          param-types
          enum-ty)
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
    (struct-register! name
      (list
        (cons '|tag| (type.bits 8))
        (cons '|__data| (type.addr))))
    ;; 2. 为每个 variant 注册构造函数
    (enum.declare-variant-ctors name variants 0)))

;; Phase 2: core-lowerer — 使用 lain-quote + pipeline 生成构造函数体
;;   不再直接调用 core.const-bits!/core.param/core.aggregate!，
;;   而是用 lain-quote 构建 raw AST，经 normalize→lower 管道发射 L1 指令。
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
             (ret-ty (struct-type enum-name))
             (has-payload (record.get variant-payload '|payload|))
             (locals (if (optional.some? has-payload)
                        (enum.bind-ctor-param fn variant)
                        (list)))
             (body-raw (enum.build-ctor-body-ast
                         enum-name index has-payload))
             (body-mid (middle.normalize-stmt body-raw)))
        (core.lower-stmt block body-mid ret-ty locals)
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

;; ===========================================================================
;; 注册到调度总线: enum 类型/表达式的降级规则
;;   虽然 enum 类型在 middle IR 中表示为 |middle.ty.path| (通过名称查找)，
;;   但这里保留类型降级钩子，供未来扩展 enum 特定类型语义时使用。
;; ===========================================================================
