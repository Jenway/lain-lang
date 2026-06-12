(meta-source "expr/hooks")

;; ═══════════════════════════════════════════════════
;; 表达式扩展钩子
;; ═══════════════════════════════════════════════════

;; 控制 parse-call-or-path 是否尝试 struct 字面量 {}
;; if/for/while 的条件解析中临时设为 #f，避免 {block} 被误解析
(define *allow-struct-literal* (make-parameter #t))

;; struct 字面量解析钩子: struct/parse.scm 注入
;; 签名: (lambda (cursor path-head type-args body) -> AST)
(define *struct-literal-parser* (make-parameter #f))
