;; ============================================================================
;; std/meta/surface/expr.scm — 表达式树 → 核心 IR 降级
;;
;; 替代旧的 expr/prec.scm + expr/call.scm，
;; 直接消费 canonicalize.scm 的结构化树。
;;
;; 入口：(expr.lower tree env)
;;   tree — 来自 canonicalize.scm 的树节点
;;   env  — 当前作用域（变量绑定环境）
;;   返回 L1 IR 表达式节点
;; ============================================================================

(meta-source "surface/expr")

;; ── 主分发 ──

(define (expr.lower tree env)
  (cond
   ;; 叶节点
   ((tree.ident? tree)   (expr.lower-ident (tree.ident-sym tree) env))
   ((tree.number? tree)  (expr.lower-number (tree.number-val tree)))
   ((tree.string? tree)  (expr.lower-string (tree.string-val tree)))

   ;; 函数调用: (call callee args-paren)
   ((tree.call? tree)
    (expr.lower-call (tree.call-callee tree) (tree.call-args tree) env))

   ;; 并列: 在表达式上下文中当作错误（应由 form parser 在语句级消费）
   ((tree.juxt? tree)
    (expr.lower-juxt tree env))

   ;; 中缀运算
   ((expr.infix-op? tree)
    (expr.lower-infix tree env))

   ;; 前缀运算: (prefix OP x)
   ((and (pair? tree) (eq? (car tree) 'prefix))
    (expr.lower-prefix tree env))

   ;; 后缀运算: (postfix x OP)
   ((and (pair? tree) (eq? (car tree) 'postfix))
    (expr.lower-postfix tree env))

   ;; 块表达式: (brace ...)
   ((tree.brace? tree)
    (expr.lower-block tree env))

   ;; 圆括号: (paren child) — 透明包装
   ((tree.paren? tree)
    (let ((children (tree.group-children tree)))
      (if (= (length children) 1)
          (expr.lower (car children) env)
          (error "expr.lower: multi-child paren not supported"))))

   (else
    (error (string-append "expr.lower: unhandled node kind: "
                          (if (pair? tree)
                              (symbol->string (car tree))
                              "non-pair"))))))

;; ── 叶节点处理 ──

(define (expr.lower-ident sym env)
  ;; 变量查找
  (raw.node! '|expr.var| (record '|expr.var| (record.field '|name| sym))))

(define (expr.lower-number n)
  ;; 整数字面量
  (raw.node! '|expr.int| (record '|expr.int| (record.field '|value| n))))

(define (expr.lower-string s)
  ;; 字符串字面量
  (raw.node! '|expr.str| (record '|expr.str| (record.field '|value| s))))

;; ── 函数调用 ──

(define (expr.lower-call callee-tree args-paren env)
  ;; callee: 任意表达式（变量、字段访问、方法链...）
  ;; args-paren: (paren arg1 (sep ,) arg2 ...) — 含逗号分隔符
  (let* ((callee-ir (expr.lower callee-tree env))
         (arg-trees (expr.parse-arg-list (tree.group-children args-paren)))
         (arg-irs   (map (lambda (a) (expr.lower a env)) arg-trees)))
    (raw.node! '|expr.call|
      (record '|expr.call|
        (record.field '|callee| callee-ir)
        (record.field '|args| arg-irs)))))

(define (expr.parse-arg-list children)
  ;; 过滤掉 (sep ,) 分隔符，返回实参树列表
  (filter (lambda (c)
            (not (and (tree.sep? c)
                      (eq? (tree.sep-sym c) '|,|))))
          children))

;; ── 并列（表达式上下文） ──

(define (expr.lower-juxt tree env)
  ;; juxt 在表达式上下文通常表示方法调用链或路径
  ;; (juxt receiver (call method args)) → method call
  ;; 其他情况按二元并列处理（错误或特殊语义由上层决定）
  (let ((left  (tree.left tree))
        (right (tree.right tree)))
    (cond
     ;; receiver.method(args) — 已由 canonicalize 输出为 (call (. receiver method) args)
     ;; 所以 juxt 在此出现通常是 type-level 或关键字序列
     (else
      (error "expr.lower-juxt: unexpected juxt in expr context")))))

;; ── 中缀运算 ──

(define *infix-ops*
  '(|+| |-| |*| |/| |%|
    |==| |!=| |<| |>| |<=| |>=|
    |&&| |\|\|| |&| |\|| |^|
    |=| |+=| |-=| |*=| |/=|
    |.| |->|))

(define (expr.infix-op? tree)
  (and (pair? tree)
       (memv (car tree) *infix-ops*)))

(define (expr.lower-infix tree env)
  (let ((op    (car tree))
        (left  (tree.left tree))
        (right (tree.right tree)))
    (cond
     ;; 字段访问: (. receiver field-ident)
     ((eq? op '|.|)
      (let ((recv-ir (expr.lower left env))
            (field   (if (tree.ident? right)
                         (tree.ident-sym right)
                         (error "expr.lower: field access: rhs must be ident"))))
        (raw.node! '|expr.field|
          (record '|expr.field|
            (record.field '|recv| recv-ir)
            (record.field '|field| field)))))
     ;; 赋值
     ((eq? op '|=|)
      (let ((lhs-ir (expr.lower left env))
            (rhs-ir (expr.lower right env)))
        (raw.node! '|expr.assign|
          (record '|expr.assign|
            (record.field '|lhs| lhs-ir)
            (record.field '|rhs| rhs-ir)))))
     ;; 算术/比较/逻辑 → 二元节点
     (else
      (let ((l-ir (expr.lower left env))
            (r-ir (expr.lower right env)))
        (raw.node! '|expr.binop|
          (record '|expr.binop|
            (record.field '|op| op)
            (record.field '|lhs| l-ir)
            (record.field '|rhs| r-ir))))))))

;; ── 前缀运算 ──

(define (expr.lower-prefix tree env)
  (let ((op      (tree.prefix-op tree))
        (operand (tree.prefix-operand tree)))
    (let ((x-ir (expr.lower operand env)))
      (raw.node! '|expr.unop|
        (record '|expr.unop|
          (record.field '|op| op)
          (record.field '|operand| x-ir))))))

;; ── 后缀运算 ──

(define (expr.lower-postfix tree env)
  (let ((operand (tree.postfix-operand tree))
        (op      (tree.postfix-op tree)))
    (let ((x-ir (expr.lower operand env)))
      (raw.node! '|expr.unop|
        (record '|expr.unop|
          (record.field '|op| op)
          (record.field '|operand| x-ir))))))

;; ── 块表达式 ──

(define (expr.lower-block brace-tree env)
  ;; (brace stmt...) → 块节点
  ;; 简化版：每个 child 当语句处理
  (let* ((stmts (tree.group-children brace-tree))
         (stmt-irs (map (lambda (s) (expr.lower-stmt s env)) stmts)))
    (raw.node! '|expr.block|
      (record '|expr.block|
        (record.field '|stmts| stmt-irs)))))

(define (expr.lower-stmt stmt env)
  ;; 语句：sep(;) 跳过，其他当表达式处理
  (if (tree.sep? stmt)
      unit
      (expr.lower stmt env)))
