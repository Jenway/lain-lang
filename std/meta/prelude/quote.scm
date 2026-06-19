;; =============================================================================
;; std/meta/core/quote.scm — lain-quote: Lain AST 引号构造器
;;
;; 用法: (lain-quote `<template>)
;;   - ` 是 Scheme 原生 quasiquote，构建模板结构
;;   - , 是 Scheme 原生 unquote，在模板中嵌入 Scheme 值或预构 AST 节点
;;
;; 支持: 字面量、表达式、语句、类型、属性、处理器、参数
;; =============================================================================

;; 帮助: 在列表中查找关键字符号，返回索引或 #f
(define (lain-find-keyword kw lst idx)
  (if (null? lst)
      #f
      (if (symbol=? (car lst) kw)
          idx
          (lain-find-keyword kw (cdr lst) (+ idx 1)))))

;; 帮助: 取列表前 n 个元素
(define (lain-take lst n)
  (if (or (null? lst) (= n 0))
      (list)
      (cons (car lst) (lain-take (cdr lst) (- n 1)))))

(define (lain-quote template)
  (cond
    ;; 已是 raw node (vector)，直接透传
    ((vector? template) template)
    ;; 列表形式的 AST 构造指令
    ((and (list? template) (not (null? template)))
     (let* ((kind (car template)))
       (cond
         ;; ══════════════════════════════════════════════════════════════
         ;; 字面量节点
         ;; ══════════════════════════════════════════════════════════════

         ((symbol=? kind 'string)
          (raw.node! '|expr.string|
            (record '|expr.string|
              (record.field '|raw| (cadr template)))))

         ((symbol=? kind 'number)
          (raw.node! '|expr.number|
            (record '|expr.number|
              (record.field '|raw| (cadr template)))))

         ((symbol=? kind 'bool)
          (raw.node! '|expr.bool|
            (record '|expr.bool|
              (record.field '|value| (cadr template)))))

        ((symbol=? kind 'array)
          (raw.node! '|expr.array|
            (record '|expr.array|
              (record.field '|init| (cadr template))
              (record.field '|len| (caddr template)))))

         ;; ══════════════════════════════════════════════════════════════
         ;; 表达式节点
         ;; ══════════════════════════════════════════════════════════════

         ;; 函数调用: (call callee arg ...)
         ;; callee 为裸符号时自动转换为 path 节点，否则送入 lain-quote 递归处理
         ((symbol=? kind 'call)
          (let* ((callee-raw (cadr template))
                 (callee (if (symbol? callee-raw)
                            (lain-quote (list 'path callee-raw))
                            (lain-quote callee-raw)))
                 (args (map lain-quote (cddr template))))
            (raw.node! '|expr.call|
              (record '|expr.call|
                (record.field '|callee| callee)
                (record.field '|args| args)))))

        ;; 路径引用: (path seg ...) 或 (path seg ... :type-args types)
        ;; 支持多段路径: (path foo bar) → path=[foo, bar]
        ((symbol=? kind 'path)
          (let* ((all-args (cdr template))
                 (type-args-kw-idx (lain-find-keyword ':type-args all-args 0))
                 (segments (if type-args-kw-idx
                              (lain-take all-args type-args-kw-idx)
                              all-args))
                 (type-args (if type-args-kw-idx
                               (list-ref all-args (+ type-args-kw-idx 1))
                               (list))))
            (raw.node! '|expr.path|
              (if (list.empty? type-args)
                  (record '|expr.path|
                    (record.field '|path| segments))
                  (record '|expr.path|
                    (record.field '|path| segments)
                    (record.field '|type-args| type-args))))))

         ((symbol=? kind 'binary)
          (raw.node! '|expr.binary|
            (record '|expr.binary|
              (record.field '|op| (cadr template))
              (record.field '|left| (lain-quote (caddr template)))
              (record.field '|right| (lain-quote (cadddr template))))))

         ((symbol=? kind 'unary)
          (raw.node! '|expr.unary|
            (record '|expr.unary|
              (record.field '|op| (cadr template))
              (record.field '|operand| (lain-quote (caddr template))))))

         ((symbol=? kind 'field)
          (raw.node! '|expr.field|
            (record '|expr.field|
              (record.field '|base| (lain-quote (cadr template)))
              (record.field '|field| (caddr template)))))

         ((symbol=? kind 'method-call)
          (raw.node! '|expr.method-call|
            (record '|expr.method-call|
              (record.field '|receiver| (lain-quote (cadr template)))
              (record.field '|method| (caddr template))
              (record.field '|args| (map lain-quote (cdddr template))))))

         ((symbol=? kind 'if)
          (raw.node! '|expr.if|
            (record '|expr.if|
              (record.field '|condition| (lain-quote (cadr template)))
              (record.field '|then| (lain-quote (caddr template)))
              (record.field '|else| (lain-quote (cadddr template))))))

         ((symbol=? kind 'borrow)
          (raw.node! '|expr.borrow|
            (record '|expr.borrow|
              (record.field '|mutable| (cadr template))
              (record.field '|operand| (lain-quote (caddr template))))))

         ((symbol=? kind 'perform)
          (raw.node! '|expr.perform|
            (record '|expr.perform|
              (record.field '|call| (lain-quote (cadr template))))))

         ((symbol=? kind 'resume)
          (let* ((val (cadr template)))
            (raw.node! '|expr.resume|
              (record '|expr.resume|
                (record.field '|value|
                  (if (symbol=? val 'none)
                      (optional.none)
                      (optional.some (lain-quote val))))))))

         ((symbol=? kind 'handle)
          (raw.node! '|expr.handle|
            (record '|expr.handle|
              (record.field '|effect| (cadr template))
              (record.field '|effect-args| (caddr template))
              (record.field '|handler| (lain-quote (cadddr template)))
              (record.field '|body| (lain-quote (car (cddddr template)))))))

         ((symbol=? kind 'tail-call)
          (raw.node! '|expr.tail-call|
            (record '|expr.tail-call|
              (record.field '|call| (lain-quote (cadr template))))))

         ((symbol=? kind 'builtin)
          (raw.node! '|expr.builtin|
            (record '|expr.builtin|
              (record.field '|name| (cadr template))
              (record.field '|args| (map lain-quote (cddr template))))))

         ((symbol=? kind 'macro-call)
          (raw.node! '|expr.macro-call|
            (record '|expr.macro-call|
              (record.field '|name| (cadr template))
              (record.field '|type-args| (caddr template))
              (record.field '|args| (map lain-quote (cdddr template))))))

         ;; 结构体字面量: (aggregate name (struct-field fname fval) ...)
         ;; 可选项: (aggregate name :type-args types (struct-field ...) ...)
         ((symbol=? kind 'aggregate)
          (let* ((struct-name (cadr template))
                 (rest (cddr template))
                 (has-targs (and (not (null? rest))
                                 (symbol=? (car rest) ':type-args)))
                 (type-args (if has-targs (cadr rest) (list)))
                 (field-forms (if has-targs (cddr rest) rest)))
            (raw.node! '|expr.struct|
              (if (list.empty? type-args)
                  (record '|expr.struct|
                    (record.field '|name| struct-name)
                    (record.field '|fields| (map lain-quote field-forms)))
                  (record '|expr.struct|
                    (record.field '|name| struct-name)
                    (record.field '|type-args| type-args)
                    (record.field '|fields| (map lain-quote field-forms)))))))

         ((symbol=? kind 'struct-field)
          (raw.node! '|expr.struct-field|
            (record '|expr.struct-field|
              (record.field '|name| (cadr template))
              (record.field '|value| (lain-quote (caddr template))))))

         ((symbol=? kind 'call-indirect)
          (raw.node! '|expr.call-indirect|
            (record '|expr.call-indirect|
              (record.field '|fn-ptr| (lain-quote (cadr template)))
              (record.field '|ret-ty| (lain-quote (caddr template)))
              (record.field '|args| (map lain-quote (cdddr template))))))

         ;; ══════════════════════════════════════════════════════════════
         ;; 语句节点
         ;; ══════════════════════════════════════════════════════════════

         ((symbol=? kind 'block)
          (raw.node! '|block|
            (record '|block|
              (record.field '|items| (map lain-quote (cdr template))))))

         ((symbol=? kind 'return)
          (let* ((val (cadr template)))
            (raw.node! '|stmt.return|
              (record '|stmt.return|
                (record.field '|value|
                  (if (symbol=? val 'void)
                      (optional.none)
                      (optional.some (lain-quote val))))))))

         ((symbol=? kind 'tail)
          (raw.node! '|stmt.tail|
            (record '|stmt.tail|
              (record.field '|expr| (lain-quote (cadr template))))))

         ;; let 语句: (let name mutable shared type-option value)
         ((symbol=? kind 'let)
          (raw.node! '|stmt.let|
            (record '|stmt.let|
              (record.field '|mutable| (cadr template))
              (record.field '|shared| (caddr template))
              (record.field '|name| (cadddr template))
              (record.field '|type|
                (car (cddddr template)))
              (record.field '|value|
                (lain-quote (cadr (cddddr template)))))))

         ((symbol=? kind 'assign)
          (raw.node! '|stmt.assign|
            (record '|stmt.assign|
              (record.field '|target| (lain-quote (cadr template)))
              (record.field '|value| (lain-quote (caddr template))))))

         ((symbol=? kind 'expr-stmt)
          (raw.node! '|stmt.expr|
            (record '|stmt.expr|
              (record.field '|expr| (lain-quote (cadr template))))))

         ;; ══════════════════════════════════════════════════════════════
         ;; 类型节点
         ;; ══════════════════════════════════════════════════════════════

         ((symbol=? kind 'type-path)
          (raw.node! '|type.path|
            (record '|type.path|
              (record.field '|name| (cadr template)))))

         ((symbol=? kind 'type-unit)
          (raw.node! '|type.unit|
            (record '|type.unit|)))

         ((symbol=? kind 'type-ref)
          (raw.node! '|type.ref|
            (record '|type.ref|
              (record.field '|mutable| (cadr template))
              (record.field '|inner| (lain-quote (caddr template))))))

         ((symbol=? kind 'type-raw-ptr)
          (raw.node! '|type.raw-ptr|
            (record '|type.raw-ptr|
              (record.field '|mutable| (cadr template))
              (record.field '|pointee| (lain-quote (caddr template))))))

         ((symbol=? kind 'type-slice)
          (raw.node! '|type.slice|
            (record '|type.slice|
              (record.field '|element| (lain-quote (cadr template))))))

         ((symbol=? kind 'type-array)
          (raw.node! '|type.array|
            (record '|type.array|
              (record.field '|element| (lain-quote (cadr template)))
              (record.field '|len| (caddr template)))))

         ((symbol=? kind 'type-fn)
          (raw.node! '|type.fn|
            (record '|type.fn|
              (record.field '|params| (cadr template))
              (record.field '|return| (lain-quote (caddr template)))
              (record.field '|effects| (cadddr template)))))

         ((symbol=? kind 'type-app)
          (raw.node! '|type.app|
            (record '|type.app|
              (record.field '|name| (cadr template))
              (record.field '|args| (caddr template)))))

         ;; ══════════════════════════════════════════════════════════════
         ;; 属性节点
         ;; ══════════════════════════════════════════════════════════════

         ((symbol=? kind 'attr)
          (raw.node! '|attr|
            (record '|attr|
              (record.field '|name| (cadr template))
              (record.field '|args| (caddr template)))))

         ((symbol=? kind 'attr-arg-string)
          (raw.node! '|attr.arg.string|
            (record '|attr.arg.string|
              (record.field '|raw| (cadr template)))))

         ((symbol=? kind 'attr-arg-number)
          (raw.node! '|attr.arg.number|
            (record '|attr.arg.number|
              (record.field '|raw| (cadr template)))))

         ((symbol=? kind 'attr-arg-path)
          (raw.node! '|attr.arg.path|
            (record '|attr.arg.path|
              (record.field '|path| (cadr template)))))

         ((symbol=? kind 'attr-arg-named)
          (raw.node! '|attr.arg.named|
            (record '|attr.arg.named|
              (record.field '|name| (cadr template))
              (record.field '|value| (lain-quote (caddr template))))))

         ;; ══════════════════════════════════════════════════════════════
         ;; 处理器 / 效果 / 参数 / 杂项
         ;; ══════════════════════════════════════════════════════════════

         ((symbol=? kind 'handler-operation)
          (raw.node! '|handler.operation|
            (record '|handler.operation|
              (record.field '|name| (cadr template))
              (record.field '|params| (caddr template))
              (record.field '|return| (cadddr template))
              (record.field '|body|
                (lain-quote (car (cddddr template)))))))

         ((symbol=? kind 'handler-value)
          (raw.node! '|handler.value|
            (record '|handler.value|
              (record.field '|value| (lain-quote (cadr template))))))

         ((symbol=? kind 'handler-inline)
          (raw.node! '|handler.inline|
            (record '|handler.inline|
              (record.field '|type| (cadr template))
              (record.field '|operations| (caddr template)))))

         ((symbol=? kind 'param)
          (raw.node! '|param|
            (record '|param|
              (record.field '|name| (cadr template))
              (record.field '|type| (lain-quote (caddr template))))))

         ((symbol=? kind 'param-self-ref)
          (raw.node! '|param.self-ref|
            (record '|param.self-ref|
              (record.field '|name| (cadr template))
              (record.field '|ref| (caddr template)))))

         ((symbol=? kind 'effect-name)
          (raw.node! '|effect.name|
            (record '|effect.name|
              (record.field '|name| (cadr template))
              (record.field '|args| (caddr template)))))

         ((symbol=? kind 'where-predicate)
          (raw.node! '|where.predicate|
            (record '|where.predicate|
              (record.field '|param| (cadr template))
              (record.field '|bound| (lain-quote (caddr template))))))

         ;; ── 默认: 非 AST 构造指令，原样返回 ──
         (else template))))
    ;; 标量值直接透传
    (else template)))
