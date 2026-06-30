;; ============================================================================
;; std/meta/canonicalize.scm — C AST → 标准前缀 S-表达式 转换
;;
;; 把 C Pratt Parser 生成的纯拓扑树 (AstNodeId) 转换为标准前缀 S-表达式。
;; 并列 (juxtaposition) INFIX(" ", left, right) 自动扁平化拼接。
;; 输出格式与 meta.lex-source! 对齐: (root child1 child2 ...)
;;
;; AST 节点类型 (lain_ast.h):
;;   0 = ATOM    — 叶子: 标识符、字面量、关键字
;;   1 = INFIX   — 二元: 显式(left op right) 或 并列(op=" ",left,right)
;;   2 = PREFIX  — 一元前缀: op operand
;;   3 = POSTFIX — 一元后缀: operand op (含函数调用)
;;   4 = GROUP   — 括号/花括号: ( children... )
;; ============================================================================

(meta-source "canonicalize")

;; ── 常量 ──
(define ast.atom    0)
(define ast.infix   1)
(define ast.prefix  2)
(define ast.postfix 3)
(define ast.group   4)

;; ── token-classify: ATOM 文本 ↦ 词法标记 (tag . value) ──
(define (token-classify text)
  (if (string? text)
      (cond
       ;; 标点/操作符
       ((or (string=? text "+") (string=? text "-") (string=? text "*")
            (string=? text "/") (string=? text "%") (string=? text "=")
            (string=? text "!") (string=? text "<") (string=? text ">")
            (string=? text "&") (string=? text "|") (string=? text "^")
            (string=? text "~") (string=? text ".") (string=? text ":")
            (string=? text ";") (string=? text ",") (string=? text "@")
            (string=? text "#") (string=? text "$") (string=? text "->")
            (string=? text "=>") (string=? text "==") (string=? text "!=")
            (string=? text "<=") (string=? text ">=") (string=? text "&&")
            (string=? text "||") (string=? text "++") (string=? text "--")
            (string=? text "+=") (string=? text "-=") (string=? text "*=")
            (string=? text "/=") (string=? text "(") (string=? text ")")
            (string=? text "[") (string=? text "]") (string=? text "{")
            (string=? text "}"))
        (list 'punct text))
       ;; 数字
       ((char-numeric? (string-ref text 0))
        (list 'int text))
       ;; 默认为标识符
       (else
        (list 'ident text)))
      '()))

;; ── ast-node-tag: 获取节点的词法标签 ──
(define (ast-node-tag id)
  (if (and (not (= id 0)) (= (ast.node-kind id) ast.atom))
      (car (token-classify (ast.node-text id)))
      'unknown))

;; ── ast->sexp-flat: 递归展开为扁平 token 列表 ──
;; 返回 token 列表: ((tag val) (tag val) ...)
(define (ast->sexp-flat id)
  (if (= id 0)
      '()
      (let* ((kind (ast.node-kind id))
             (left (ast.node-left id))
             (right (ast.node-right id))
             (op   (ast.node-op id))
             (text (ast.node-text id)))
        (case kind
         ;; ATOM → token
         ((0) ; ast.atom
          (if text
              (list (token-classify text))
              '()))

         ;; INFIX → 根据 op 区分显式还是并列
         ((1) ; ast.infix
          (let ((op-text (if (= op 0) "" (ast.node-text op))))
            (if (and (string? op-text) (string=? op-text " "))
                ;; 并列: 拼接 left 和 right 的 tokens
                (append (ast->sexp-flat left)
                        (ast->sexp-flat right))
                ;; 显式中缀: (left op right)
                (append (ast->sexp-flat left)
                        (ast->sexp-flat op)
                        (ast->sexp-flat right)))))

         ;; PREFIX → (op left) 含 right
         ((2) ; ast.prefix
          (let ((base (append (ast->sexp-flat op)
                              (ast->sexp-flat left))))
            (if (= right 0)
                base
                (append base (ast->sexp-flat right)))))

         ;; POSTFIX → (left op) / func(args)
         ((3) ; ast.postfix
          (let ((left-tokens (ast->sexp-flat left))
                (op-text (if (= op 0) "" (ast.node-text op))))
            (if (and (string? op-text) (string=? op-text "("))
                ;; 函数调用: name 后跟 args GROUP (GROUP 自带定界符)
                (append left-tokens (ast->sexp-flat right))
                ;; 一般后缀
                (append left-tokens (ast->sexp-flat op)))))

         ;; GROUP → (delim child1-flat child2-flat ...)
         ((4) ; ast.group
          (let* ((delim-tag (ast-node-delim id))
                 (children (ast-group->list-flat left))
                 (right-flat (ast->sexp-flat right)))
            (list
             (cons delim-tag
                   (append (apply append children) right-flat)))))

         (else '())))))

;; ── ast-node-delim: 从 GROUP 的 op 字段获取定界符文本 ──
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

;; ── ast-group->list-flat: 沿 next 链收集兄弟 ──
(define (ast-group->list-flat first-id)
  (let loop ((id first-id) (acc '()))
    (if (= id 0)
        (reverse acc)
        (let* ((next-id (ast.node-next id)))
          (loop next-id
                (cons (ast->sexp-flat id) acc))))))

;; ── ast->root-form: 转换并包装为 root group ──
;; 输出格式与 meta.lex-source! 对齐
;; 如果是单子 GROUP，直接返回子节点 tokens
(define (ast->root-form id)
  (let* ((kind (ast.node-kind id))
         (left (ast.node-left id)))
    (if (and (= kind ast.group) (not (= left 0)))
        ;; 如果是只有一个子节点的 GROUP，直接返回子节点展开
        (if (= (ast.node-next left) 0)
            (cons 'root (ast->sexp-flat left))
            ;; 多子节点
            (cons 'root
                  (apply append
                         (map (lambda (tokens) tokens)
                              (ast-group->list-flat left)))))
        (cons 'root (ast->sexp-flat id)))))

;; ── parse-and-canonicalize: 一步到位 ──
;; 拆分顶层 GROUP 为多个 (root ...) form (每个顶层表达式一个)
(define (parse-and-canonicalize src len)
  (let* ((root-id (ast.parse! src len))
         (kind (ast.node-kind root-id))
         (left (ast.node-left root-id))
         (forms
          (if (and (= kind ast.group) (not (= left 0)))
              ;; 每个顶层子节点一个 form
              (ast-group->root-forms left)
              ;; 单根
              (list (ast->root-form root-id)))))
    (ast.destroy!)
    forms))

;; ── ast-group->root-forms: 每个子节点包装为独立的 (root ...) ──
(define (ast-group->root-forms first-id)
  (let loop ((id first-id) (acc '()))
    (if (= id 0)
        (reverse acc)
        (let* ((next-id (ast.node-next id))
               (form (cons 'root (ast->sexp-flat id))))
          (loop next-id (cons form acc))))))
