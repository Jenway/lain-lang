;; =============================================================================
;; std/meta/core/quote.scm — lain-quote: Lain AST 引号构造器
;;
;; 用法: (lain-quote `<template>)
;;   - ` 是 Scheme 原生 quasiquote，构建模板结构
;;   - , 是 Scheme 原生 unquote，在模板中嵌入 Scheme 值或预构 AST 节点
;;
;; 示例:
;;   (lain-quote `(call __lain_println_raw (string ,msg-sym) (number ,len-sym)))
;;   → raw.node! 调用链，构建完整的 expr.call AST 节点
;; =============================================================================

;; ---------------------------------------------------------------------------
;; lain-quote 核心函数
;; ---------------------------------------------------------------------------

(define (lain-quote template)
  (cond
    ;; 已是 raw node (vector)，直接透传
    ((vector? template) template)
    ;; 列表形式的 AST 构造指令
    ((and (list? template) (not (null? template)))
     (let* ((kind (car template)))
       (cond
         ;; ── 表达式节点 ──

         ;; 函数调用: (call callee arg ...)
         ((symbol=? kind 'call)
          (let* ((callee-raw (cadr template))
                 (callee (if (symbol? callee-raw)
                            ;; 裸符号自动转换为 path 节点
                            (lain-quote (list 'path callee-raw))
                            callee-raw))
                 (args (map lain-quote (cddr template))))
            (raw.node! '|expr.call|
              (record '|expr.call|
                (record.field '|callee| callee)
                (record.field '|args| args)))))

         ;; 路径引用: (path name)
         ((symbol=? kind 'path)
          (raw.node! '|expr.path|
            (record '|expr.path|
              (record.field '|path| (list (cadr template))))))

         ;; 字符串字面量: (string raw-value)
         ((symbol=? kind 'string)
          (raw.node! '|expr.string|
            (record '|expr.string|
              (record.field '|raw| (cadr template)))))

         ;; 数字字面量: (number raw-value)
         ((symbol=? kind 'number)
          (raw.node! '|expr.number|
            (record '|expr.number|
              (record.field '|raw| (cadr template)))))

         ;; 布尔字面量: (bool #t|#f)
         ((symbol=? kind 'bool)
          (raw.node! '|expr.bool|
            (record '|expr.bool|
              (record.field '|value| (cadr template)))))

         ;; 二元运算: (binary op left right)
         ((symbol=? kind 'binary)
          (raw.node! '|expr.binary|
            (record '|expr.binary|
              (record.field '|op| (cadr template))
              (record.field '|left| (lain-quote (caddr template)))
              (record.field '|right| (lain-quote (cadddr template))))))

         ;; 一元运算: (unary op operand)
         ((symbol=? kind 'unary)
          (raw.node! '|expr.unary|
            (record '|expr.unary|
              (record.field '|op| (cadr template))
              (record.field '|operand| (lain-quote (caddr template))))))

         ;; 字段访问: (field base field-name)
         ((symbol=? kind 'field)
          (raw.node! '|expr.field|
            (record '|expr.field|
              (record.field '|base| (lain-quote (cadr template)))
              (record.field '|field| (caddr template)))))

         ;; 方法调用: (method-call receiver method arg ...)
         ((symbol=? kind 'method-call)
          (raw.node! '|expr.method-call|
            (record '|expr.method-call|
              (record.field '|receiver| (lain-quote (cadr template)))
              (record.field '|method| (caddr template))
              (record.field '|args| (map lain-quote (cdddr template))))))

         ;; if 表达式: (if cond then else)
         ((symbol=? kind 'if)
          (raw.node! '|expr.if|
            (record '|expr.if|
              (record.field '|condition| (lain-quote (cadr template)))
              (record.field '|then| (lain-quote (caddr template)))
              (record.field '|else| (lain-quote (cadddr template))))))

         ;; 结构体字面量: (aggregate struct-name (struct-field fname fval) ...)
         ((symbol=? kind 'aggregate)
          (let* ((struct-name (cadr template))
                 (field-forms (cddr template)))
            (raw.node! '|expr.struct|
              (record '|expr.struct|
                (record.field '|name| struct-name)
                (record.field '|fields|
                  (map lain-quote field-forms))))))

         ;; 结构体字段值 (用于 aggregate 内): (struct-field fname fval)
         ((symbol=? kind 'struct-field)
          (raw.node! '|expr.struct-field|
            (record '|expr.struct-field|
              (record.field '|name| (cadr template))
              (record.field '|value| (lain-quote (caddr template))))))

         ;; ── 语句节点 ──

         ;; 代码块: (block item ...)
         ((symbol=? kind 'block)
          (raw.node! '|block|
            (record '|block|
              (record.field '|items| (map lain-quote (cdr template))))))

         ;; return 语句: (return expr) 或 (return void)
         ((symbol=? kind 'return)
          (let* ((val (cadr template)))
            (raw.node! '|stmt.return|
              (record '|stmt.return|
                (record.field '|value|
                  (if (symbol=? val 'void)
                      (optional.none)
                      (optional.some (lain-quote val))))))))

         ;; 尾表达式: (tail expr)
         ((symbol=? kind 'tail)
          (raw.node! '|stmt.tail|
            (record '|stmt.tail|
              (record.field '|expr| (lain-quote (cadr template))))))

         ;; let 语句: (let name value)
         ((symbol=? kind 'let)
          (raw.node! '|stmt.let|
            (record '|stmt.let|
              (record.field '|mutable| #f)
              (record.field '|shared| #f)
              (record.field '|name| (cadr template))
              (record.field '|type| (optional.none))
              (record.field '|value| (lain-quote (caddr template))))))

         ;; 赋值语句: (assign target value)
         ((symbol=? kind 'assign)
          (raw.node! '|stmt.assign|
            (record '|stmt.assign|
              (record.field '|target| (lain-quote (cadr template)))
              (record.field '|value| (lain-quote (caddr template))))))

         ;; 间接调用: (call-indirect fn-ptr ret-ty arg ...)
         ((symbol=? kind 'call-indirect)
          (raw.node! '|expr.call-indirect|
            (record '|expr.call-indirect|
              (record.field '|fn-ptr| (lain-quote (cadr template)))
              (record.field '|ret-ty| (lain-quote (caddr template)))
              (record.field '|args| (map lain-quote (cdddr template))))))

         ;; 类型路径: (type-path name)
         ((symbol=? kind 'type-path)
          (raw.node! '|type.path|
            (record '|type.path|
              (record.field '|name| (cadr template)))))

         ;; ── 默认: 非 AST 构造指令，原样返回 ──
         (else template))))
    ;; 标量值 (symbol, number, string 等) 直接透传
    (else template)))
