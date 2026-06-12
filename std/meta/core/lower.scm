(meta-source "core/lower")

;; ===========================================================================
;; 核心降级调度总线 — Data-Directed Dispatch 架构
;;
;; 本文件不再包含任何语言特性的具体降级逻辑。
;; 所有降级规则通过 define-pass 注册到 pipeline 系统，
;; core.lower-type / core.lower-stmt / core.lower-expr / core.infer-expr-type
;; 仅作为调度器从注册表中查找并调用对应的处理函数。
;; ===========================================================================

;; 查找 pipeline 中注册的处理函数，返回 #f 如果未注册
(define (pipeline.lookup stage kind)
  (let loop ((passes __lain-passes))
    (if (null? passes)
        #f
        (let* ((entry (car passes))
               (e-stage (car entry))
               (e-kind (car (cdr entry)))
               (e-body (car (cdr (cdr entry)))))
          (if (and (equal? e-stage stage) (equal? e-kind kind))
              e-body
              (loop (cdr passes)))))))

;; ---------------------------------------------------------------------------
;; 类型降级: pipeline stage = core-type-lowerer
;; ---------------------------------------------------------------------------

(define-pass (core-type-lowerer |middle.ty.unit| ty)
  (type.unit))

(define-pass (core-type-lowerer |middle.ty.path| ty)
  (let* ((payload (middle.payload ty))
         (name (optional.value (record.get payload '|name|))))
    (type.registered name (list))))

(define-pass (core-type-lowerer |middle.ty.app| ty)
  (let* ((payload (middle.payload ty))
         (name (optional.value (record.get payload '|name|)))
         (args (core.lower-types
                 (optional.value (record.get payload '|args|))
                 (list))))
    (type.registered name args)))

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
;; 语句降级: pipeline stage = core-stmt-lowerer
;; ---------------------------------------------------------------------------

(define-pass (core-stmt-lowerer |middle.stmt.return| block stmt ret-ty locals)
  (let* ((payload (middle.payload stmt))
         (value (optional.value (record.get payload '|value|))))
    (if (optional.none? value)
        (core.return-none! block)
        (core.return-value!
          block
          (core.lower-expr block (optional.value value) ret-ty locals)))
    locals))

(define-pass (core-stmt-lowerer |middle.stmt.tail| block stmt ret-ty locals)
  (let* ((payload (middle.payload stmt))
         (expr (optional.value (record.get payload '|expr|))))
    (if (symbol=? (middle.kind expr) '|middle.expr.if|)
        (core.lower-if-tail-expr block expr ret-ty locals)
        (if (type.unit? ret-ty)
            (begin
              (core.lower-expr block expr ret-ty locals)
              (core.return-none! block))
            (core.return-value!
              block
              (core.lower-expr block expr ret-ty locals))))
    locals))

(define-pass (core-stmt-lowerer |middle.stmt.expr| block stmt ret-ty locals)
  (let* ((payload (middle.payload stmt))
         (expr (optional.value (record.get payload '|expr|))))
    (core.lower-expr
      block
      expr
      (core.infer-expr-type expr locals)
      locals)
    locals))

(define-pass (core-stmt-lowerer |middle.stmt.let| block stmt ret-ty locals)
  (let* ((payload (middle.payload stmt))
         (ty-option (optional.value (record.get payload '|type|)))
         (name (optional.value (record.get payload '|name|)))
         (mutable (optional.value (record.get payload '|mutable|)))
         (value-expr (optional.value (record.get payload '|value|)))
         (ty (if (optional.none? ty-option)
                 (core.infer-expr-type value-expr locals)
                 (core.lower-type (optional.value ty-option))))
         (value (core.lower-expr block value-expr ty locals)))
    (list.cons
      (record '|local|
        (record.field '|name| name)
        (record.field '|type| ty)
        (record.field '|mutable| mutable)
        (record.field '|value| value))
      locals)))

(define-pass (core-stmt-lowerer |middle.stmt.assign| block stmt ret-ty locals)
  (let* ((payload (middle.payload stmt))
         (target (optional.value (record.get payload '|target|)))
         (value-expr (optional.value (record.get payload '|value|))))
    (if (symbol=? (middle.kind target) '|middle.expr.path|)
        (let* ((target-payload (middle.payload target))
               (path (optional.value (record.get target-payload '|path|)))
               (name (list.first path)))
          (if (core.local-mutable? locals name)
              (let* ((ty (core.local-type locals name))
                     (value (core.lower-expr block value-expr ty locals)))
                (list.cons
                  (record '|local|
                    (record.field '|name| name)
                    (record.field '|type| ty)
                    (record.field '|mutable| #t)
                    (record.field '|value| value))
                  locals))
              (type.unsupported '|immutable-assignment|)))
        (type.unsupported '|assignment-target|))))

(define (core.lower-stmt block stmt ret-ty locals)
  (let* ((kind (middle.kind stmt))
         (lowerer (pipeline.lookup '|core-stmt-lowerer| kind)))
    (if lowerer
        (lowerer block stmt ret-ty locals)
        locals)))

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

(define (core.lower-stmts block stmts ret-ty locals)
  (if (list.empty? stmts)
      (core.return-none! block)
      (let* ((next-locals (core.lower-stmt
                            block
                            (list.first stmts)
                            ret-ty
                            locals)))
        (if (list.empty? (list.rest stmts))
            unit
            (core.lower-stmts
              block
              (list.rest stmts)
              ret-ty
              next-locals)))))
