(meta-source "pipeline/lower")

;; ===========================================================================
;; 核心降级调度总线 + 类型降级 + 辅助函数
;;
;; core.lower-type / core.lower-expr / core.infer-expr-type
;; 仅作为调度器从注册表中查找并调用对应的处理函数。
;; 语句降级 (core.lower-stmt/core.lower-stmts) 已迁至 control/lower.scm。
;; ===========================================================================

;; 查找 pipeline 中注册的处理函数，返回 #f 如果未注册
(define (pipeline.lookup stage kind)
  (let ((loop #f))
  (set! loop (lambda (passes)
    (if (null? passes)
        #f
        (let* ((entry (car passes))
               (e-stage (car entry))
               (e-kind (car (cdr entry)))
               (e-body (car (cdr (cdr entry)))))
          (if (and (equal? e-stage stage) (equal? e-kind kind))
              e-body
              (loop (cdr passes)))))))
  (loop __lain-passes)))

;; ---------------------------------------------------------------------------
;; 类型降级: pipeline stage = core-type-lowerer
;; ---------------------------------------------------------------------------

(define-pass* 'core-type-lowerer '|types.unit| (lambda (ty)
  (type.unit)))

(define-pass* 'core-type-lowerer '|types.path| (lambda (ty)
  (let* ((payload (middle.payload ty))
         (name (optional.value (record.get payload '|name|))))
    ;; Struct types return pure Scheme record — no C cpointer
    (if (struct-registered? name)
        (struct-type name)
        (type.registered name (list))))))

(define-pass* 'core-type-lowerer '|types.app| (lambda (ty)
  (let* ((payload (middle.payload ty))
         (name (optional.value (record.get payload '|name|)))
         (args (core.lower-types
                 (optional.value (record.get payload '|args|))
                 (list))))
    ;; If the name is a registered struct (e.g., DynArray<T>),
    ;; instantiate it concretely in Scheme. Otherwise delegate to C.
    (if (struct-registered? name)
        (struct-instantiate name args)
        (type.registered name args)))))

(define (core.lower-type ty)
  (let* ((kind (middle.kind ty))
         (lowerer (pipeline.lookup '|core-type-lowerer| kind)))
    (if lowerer
        (lowerer ty)
        (type.unsupported kind))))

;; ---------------------------------------------------------------------------
;; 表达式类型推导: pipeline stage = core-expr-inferer
;; ---------------------------------------------------------------------------

(define (core.infer-expr-type expr locals)
  (let* ((kind (middle.kind expr))
         (inferer (pipeline.lookup '|core-expr-inferer| kind)))
    (if inferer
        (inferer expr locals)
        (type.unsupported '|inferred-expression-type|))))

;; ---------------------------------------------------------------------------
;; 表达式降级: pipeline stage = core-expr-lowerer (已有)
;; ---------------------------------------------------------------------------

(define (core.lower-expr block expr expected-ty locals)
  (let* ((kind (middle.kind expr))
         (lowerer (pipeline.rule '|core-expr-lowerer| kind)))
    (lowerer block expr expected-ty locals)))

;; ---------------------------------------------------------------------------
;; 辅助函数 (不变)
;; ---------------------------------------------------------------------------

(define (core.lower-types types acc)
  (if (list.empty? types)
      (list.reverse acc)
      (let* ((ty (core.lower-type (list.first types))))
        (core.lower-types (list.rest types) (list.cons ty acc)))))

(define (core.lower-param-types params acc)
  (if (list.empty? params)
      (list.reverse acc)
      (let* ((param (list.first params))
             (payload (middle.payload param))
             (ty (core.lower-type (optional.value (record.get payload '|type|)))))
        (core.lower-param-types (list.rest params) (list.cons ty acc)))))

(define (core.bind-params function params index locals)
  (if (list.empty? params)
      (list.reverse locals)
      (let* ((param (list.first params))
             (payload (middle.payload param))
             (name (optional.value (record.get payload '|name|)))
             (ty (core.lower-type (optional.value (record.get payload '|type|))))
             (value (core.param function index))
             (local (record '|local|
                      (record.field '|name| name)
                      (record.field '|type| ty)
                      (record.field '|mutable| #f)
                      (record.field '|value| value))))
        (core.bind-params
          function
          (list.rest params)
          (u64.add1 index)
          (list.cons local locals)))))
