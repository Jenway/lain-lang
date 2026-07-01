;; ============================================================================
;; std/meta/canonicalize.scm — C AST → 结构化 S-表达式树
;;
;; 把 C Pratt Parser 生成的纯拓扑树 (AstNodeId) 转换为结构化 S-表达式。
;; 保留 Pratt 树的全部拓扑信息，供后续 form parsers 直接消费。
;;
;; 输出节点格式：
;;
;;   原子:
;;     (ident sym)       — 标识符
;;     (number n)        — 数字字面量 (Scheme number)
;;     (string s)        — 字符串（不含引号，symbol）
;;     (sep sym)         — 分隔符: , ; @ # $
;;
;;   复合（对应 C Pratt 树拓扑）:
;;     (juxt l r)        — INFIX(" ")  并列
;;     (OP l r)          — INFIX(op)   显式中缀: -> :: . + - * / < > == != 等
;;     (prefix OP x)     — PREFIX      前缀一元运算
;;     (postfix x OP)    — POSTFIX     后缀运算: ++ --
;;     (call callee args)— POSTFIX "(" 函数调用，args 是 (paren ...) 节点
;;
;;   分组:
;;     (paren child...)  — ( ... )
;;     (brace child...)  — { ... }
;;     (bracket child...)— [ ... ]
;;
;; 每个顶层表达式包装为 (root tree-node)。
;; @decorator 与紧随的声明合并在同一 (root ...) 中作为兄弟节点。
;; ============================================================================

(meta-source "canonicalize")

;; ── 常量 ──
(define ast.atom    0)
(define ast.infix   1)
(define ast.prefix  2)
(define ast.postfix 3)
(define ast.group   4)

;; ── atom->tree-node: ATOM 文本 → 结构化叶节点 ──
(define (atom->tree-node text)
  (cond
   ;; 分隔符（C parser 在 group 内作为兄弟 ATOM 添加）
   ((string=? text ",") (list 'sep '|,|))
   ((string=? text ";") (list 'sep '|;|))
   ((string=? text "@") (list 'sep '|@|))
   ((string=? text "#") (list 'sep '|#|))
   ((string=? text "$") (list 'sep '|$|))
   ;; 字符串字面量：C parser 保留引号，如 "hello"
   ((char=? (string-ref text 0) #\")
    (list 'string (string->symbol (substring text 1 (- (string-length text) 1)))))
   ;; 数字
   ((char-numeric? (string-ref text 0))
    (list 'number (string->number text)))
   ;; 默认为标识符
   (else
    (list 'ident (string->symbol text)))))

;; ── ast->sexp-tree: 递归转换为结构化树节点 ──
(define (ast->sexp-tree id)
  (if (= id 0)
      '()
      (let* ((kind  (ast.node-kind id))
             (left  (ast.node-left id))
             (right (ast.node-right id))
             (op    (ast.node-op id))
             (text  (ast.node-text id)))
        (case kind

          ;; ATOM → 带标签的叶节点
          ((0)
           (if text (atom->tree-node text) '()))

          ;; INFIX → (juxt l r) 或 (OP l r)
          ((1)
           (let ((op-text (if (= op 0) " " (ast.node-text op))))
             (if (string=? op-text " ")
                 (list 'juxt
                       (ast->sexp-tree left)
                       (ast->sexp-tree right))
                 (list (string->symbol op-text)
                       (ast->sexp-tree left)
                       (ast->sexp-tree right)))))

          ;; PREFIX → (prefix OP operand)
          ((2)
           (let ((op-text (if (= op 0) "?" (ast.node-text op))))
             (list 'prefix
                   (string->symbol op-text)
                   (ast->sexp-tree left))))

          ;; POSTFIX → (call callee args) 或 (postfix callee OP)
          ((3)
           (let ((op-text (if (= op 0) "?" (ast.node-text op))))
             (if (string=? op-text "(")
                 ;; 函数调用: args 是 right 字段中的 paren GROUP
                 (list 'call
                       (ast->sexp-tree left)
                       (ast->sexp-tree right))
                 ;; 普通后缀 (++, --)
                 (list 'postfix
                       (ast->sexp-tree left)
                       (string->symbol op-text)))))

          ;; GROUP → (paren|brace|bracket children...)
          ((4)
           (let* ((delim-tag (ast-node-delim id))
                  (children  (ast-group->list-tree left)))
             (cons (or delim-tag 'group) children)))

          (else '())))))

;; ── ast-node-delim: 从 GROUP 的 op 字段获取定界符符号 ──
(define (ast-node-delim id)
  (let ((op (ast.node-op id)))
    (if (= op 0)
        #f
        (let ((text (ast.node-text op)))
          (and (string? text)
               (cond
                ((string=? text "(") 'paren)
                ((string=? text "{") 'brace)
                ((string=? text "[") 'bracket)
                (else #f)))))))

;; ── ast-group->list-tree: 沿 next 链收集兄弟，每个转为树节点 ──
(define (ast-group->list-tree first-id)
  (let loop ((id first-id) (acc '()))
    (if (= id 0)
        (reverse acc)
        (loop (ast.node-next id)
              (cons (ast->sexp-tree id) acc)))))

;; ── tree-starts-with-at?: 检测装饰器 (@attr 或 @attr(...)) ──
;; 递归检查树节点最左叶是否为 (sep @)
(define (tree-starts-with-at? node)
  (cond
   ((and (pair? node) (eq? (car node) 'sep)
         (eq? (cadr node) '|@|)) #t)
   ((and (pair? node) (or (eq? (car node) 'juxt)
                           (eq? (car node) 'call)))
    (tree-starts-with-at? (cadr node)))
   (else #f)))

;; ── parse-and-canonicalize: 一步到位 ──
;; 输出: 每个顶层表达式为 (root tree-node) 或 (root deco-tree decl-tree)
(define (parse-and-canonicalize src len)
  (let* ((root-id (ast.parse! src len))
         (kind    (ast.node-kind root-id))
         (left    (ast.node-left root-id))
         (forms
          (if (and (= kind ast.group) (not (= left 0)))
              (ast-group->root-forms left)
              ;; 单个表达式
              (list (list 'root (ast->sexp-tree root-id))))))
    (ast.destroy!)
    forms))

;; ── ast-group->root-forms: 每个顶层子节点 → (root ...) ──
;; @decorator 与紧随的声明合并为一个 (root deco-node decl-node)
(define (ast-group->root-forms first-id)
  (let loop ((id first-id) (acc '()) (pending-decos '()))
    (if (= id 0)
        ;; 孤立 deco（通常不应出现）→ 也输出
        (reverse
         (if (null? pending-decos)
             acc
             (cons (cons 'root pending-decos) acc)))
        (let* ((next-id (ast.node-next id))
               (node    (ast->sexp-tree id)))
          (if (tree-starts-with-at? node)
              ;; 是装饰器 → 暂存，和下一个声明合并
              (loop next-id acc (append pending-decos (list node)))
              ;; 普通声明 → 与积累的 deco 一起输出
              (loop next-id
                    (cons (cons 'root (append pending-decos (list node))) acc)
                    '()))))))

;; ── 接入管道 ──
(define meta.lex-source! parse-and-canonicalize)
