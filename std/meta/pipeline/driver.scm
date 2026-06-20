;; ===========================================================================
;; std/meta/pipeline/driver.scm — Pipeline 调度与编译入口
;;
;; define-pass / __lain-passes 由 C 侧 (helpers.c) 注入。
;; ===========================================================================

(define (syntax.form-cursor form)
  (syntax.group-cursor form))

(define (cfg.target-os) '|linux|)

(define (pipeline.rule stage kind)
  (let loop ((passes __lain-passes))
    (if (null? passes)
        (lambda args unit)  ;; 默认无操作 — 未注册的 pass 静默跳过
        (let* ((entry (car passes))
               (e-stage (car entry))
               (e-kind (car (cdr entry)))
               (e-body (car (cdr (cdr entry)))))
          (if (and (equal? e-stage stage) (equal? e-kind kind))
              e-body
              (loop (cdr passes)))))))

;; ---------------------------------------------------------------------------
;; 驱动管线
;; ---------------------------------------------------------------------------

;; Helper: 尝试查找 form-parser，找不到返回 #f
(define (driver.lookup-form-parser kind)
  (let loop ((passes __lain-passes))
    (if (null? passes)
        #f
        (let* ((entry (car passes))
               (e-stage (car entry))
               (e-kind (car (cdr entry))))
          (if (and (equal? e-stage 'form-parser) (equal? e-kind kind))
              (car (cdr (cdr entry)))
              (loop (cdr passes)))))))

;; Phase 1: Peek root group 的第一个标识符，然后分发 form-parser
;; 关键：form-parser 接收原始的 root group（不是 cursor！）
;; form-parser 内部会调用 syntax.form-cursor 创建新的 cursor 从零开始解析
;; 注意：不清空 *lain-declarations*，允许调用者在之前累积声明
(define (driver.parse-and-declare root-group)
  (let* ((peek-cursor (syntax.group-cursor root-group))
         ;; 跳过所有 @attributes 以找到真正的声明关键字
         ;; 例如 @foreign(c, link_name="...") pub fn ... → 派发给 pub form-parser
         (_skip-attrs (syntax.parse-attrs peek-cursor (list)))
         (head (syntax.cursor-match-ident! peek-cursor))
         (kind (if (optional.some? head)
                   (optional.value head)
                   (optional.none))))
    (if (optional.none? kind)
        unit
        (let* ((parser (driver.lookup-form-parser kind)))
          (if parser
              (begin
                (parser root-group)
                (if (null? *lain-declarations*)
                    (error "form-parser produced no declarations")
                    unit))
              (driver.parse-as-implicit-main root-group))))))

;; Helper: 当 root group 不是已知 form 时，当做隐式 main 函数体处理
(define (driver.parse-as-implicit-main root-group)
  (let* ((block (syntax.parse-block root-group))
         (sig (raw.node! '|fn.sig|
                (record '|fn.sig|
                  (record.field '|attrs| (list))
                  (record.field '|generics| (list))
                  (record.field '|params| (list))
                  (record.field '|return|
                    (raw.node! '|ty.path|
                      (record '|ty.path|
                        (record.field '|name| '|i32|))))
                  (record.field '|where| #f)
                  (record.field '|effects| #f)
                  (record.field '|body| (optional.some block))))))
    (decl.define! '|fn| '|main| sig)))

;; Phase 2: 遍历 *lain-declarations*，对每条声明调用 raw-normalizer。
(define (driver.normalize-decls)
  (driver.normalize-decls-from *lain-declarations*))

;; 从给定的 decls 列表进行 normalize（供递归导入使用）
(define (driver.normalize-decls-from decls)
  (let loop ((remaining decls) (acc (list)))
    (if (null? remaining)
        (list.reverse acc)
        (let* ((decl (car remaining))
               (kind (list-ref decl 1))
               (normalizer (pipeline.rule 'raw-normalizer kind))
               (middle-item (normalizer decl)))
          (loop (cdr remaining) (list.cons middle-item acc))))))

;; Phase 3: 对每个 middle item 调用 core-declarer 注册函数签名。
(define (driver.declare-core middle-items)
  (for-each
    (lambda (item)
      (let* ((kind (middle.kind item))
             (declarer (pipeline.rule 'core-declarer kind)))
        (declarer item)))
    middle-items))

;; Phase 4: 对每个 middle item 调用 core-lowerer 生成 L1 指令。
(define (driver.lower-core middle-items)
  (for-each
    (lambda (item)
      (let* ((kind (middle.kind item))
             (lowerer (pipeline.rule 'core-lowerer kind)))
        (lowerer item)))
    middle-items))

;; ---------------------------------------------------------------------------
;; 主入口: compile-group-to-core（支持递归导入）
;; ---------------------------------------------------------------------------

;; 收集在 known-decls 之后新增到 *lain-declarations* 前面的声明
(define (driver.collect-new-declarations known-decls)
  (let loop ((all *lain-declarations*) (acc (list)))
    (if (null? all)
        (list.reverse acc)
        (if (driver.decl-in-list? (car all) known-decls)
            (list.reverse acc)  ;; 遇到已知声明，停止收集
            (loop (cdr all) (list.cons (car all) acc))))))

;; 收集所有 middle items（包括递归导入的模块）
(define (driver.collect-all-middle-items root-group)
  ;; 清空 *lain-declarations*，开始全新的编译
  (set! *lain-declarations* (list))
  ;; Phase 1: parse（会递归触发 import 的 core-declarer，但 core-declarer
  ;; 在 Phase 3 才运行，所以这里只收集主文件的声明）
  (driver.parse-and-declare root-group)
  ;; 保存主文件声明列表
  (let* ((main-decls (list.reverse *lain-declarations*))
         ;; Phase 2: normalize 主文件声明
         (main-middle (driver.normalize-decls-from main-decls))
         ;; Phase 3: declare-core（会触发 import 的 core-declarer 递归编译）
         ;; 注意：driver.declare-core 会调用 core-declarer for |middle.import|，
         ;; 这会在 *lain-declarations* 前面追加被导入模块的声明
         (_ (driver.declare-core main-middle))
         ;; 现在 *lain-declarations* = imported decls + main decls
         (all-decls (list.reverse *lain-declarations*))
         ;; 找出被导入模块新增的声明（不在 main-decls 中的）
         (imported-decls (driver.filter-new-decls all-decls main-decls (list)))
         ;; Phase 2b: normalize 导入模块的声明
         (imported-middle (driver.normalize-decls-from imported-decls)))
    ;; 返回所有 middle items（导入的在前面，主文件的在后面）
    (append imported-middle main-middle)))

(define (driver.filter-new-decls all-decls main-decls acc)
  (if (null? all-decls)
      (list.reverse acc)
      (let* ((decl (car all-decls)))
        (if (driver.decl-in-list? decl main-decls)
            (driver.filter-new-decls (cdr all-decls) main-decls acc)
            (driver.filter-new-decls (cdr all-decls) main-decls
              (list.cons decl acc))))))

(define (driver.decl-in-list? decl lst)
  (if (null? lst)
      #f
      (if (equal? decl (car lst))
          #t
          (driver.decl-in-list? decl (cdr lst)))))

(define (compile-group-to-core root-group)
  (let* ((all-middle-items (driver.collect-all-middle-items root-group)))
    ;; Phase 3 (continued): declare-core 已在 collect 中调用
    ;; Phase 4: lower all middle items
    (driver.lower-core all-middle-items)
    ;; Check for errors: if any diagnostic was raised, signal failure
    (if (> *error-count* 0)
        (error (string-append "compilation failed with "
                              (number->string *error-count*)
                              " error(s)"))
        0)))
