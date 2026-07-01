(meta-source "control/parse")

;; 控制流解析已迁移到 surface/tree.scm:
;;   tree-parse-block — 块解析
;;   tree-lower-expr  — 内含 if/true/false 处理
;;
;; 此文件保留 parse-block 别名供遗留引用

(define (parse-block group)
  (tree-parse-block group))

(define (parse-expr node)
  (tree-lower-expr node))
