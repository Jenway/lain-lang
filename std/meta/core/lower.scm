(meta-source "core/lower")

(set! core.lower-type
  (lambda (ty)
  (let* ((kind (middle.kind ty))
         (payload (middle.payload ty)))
    (cond
      ((symbol=? kind '|middle.ty.unit|)
       (type.unit))
      ((symbol=? kind '|middle.ty.path|)
       (let* ((name (optional.value
                     (record.get payload '|name|))))
         (type.registered name (list))))
      ((symbol=? kind '|middle.ty.app|)
       (type.registered
         (optional.value
           (record.get payload '|name|))
         (core.lower-types
           (optional.value
             (record.get payload '|args|))
           (list))))
      ((symbol=? kind '|middle.ty.raw-ptr|)
       (type.raw-ptr
         (optional.value
           (record.get payload '|mutable|))
         (core.lower-type
           (optional.value
             (record.get payload '|pointee|)))))
      ((symbol=? kind '|middle.ty.ref|)
       (type.raw-ptr
         (optional.value
           (record.get payload '|mutable|))
         (core.lower-type
           (optional.value
             (record.get payload '|inner|)))))
      ((symbol=? kind '|middle.ty.slice|)
       (type.unsupported kind))
      ((symbol=? kind '|middle.ty.array|)
       (type.array
         (core.lower-type
           (optional.value
             (record.get payload '|element|)))
         (optional.value
           (record.get payload '|len|))))
      ((symbol=? kind '|middle.ty.fn|)
       (type.unsupported kind))
      (else
       (type.unsupported kind))))))

(set! core.lower-types
  (lambda (types acc)
  (if (list.empty? types)
      (list.reverse acc)
      (let* ((ty (core.lower-type (list.first types))))
        (core.lower-types
          (list.rest types)
          (list.cons ty acc))))))

(set! core.lower-param-types
  (lambda (params acc)
  (if (list.empty? params)
      (list.reverse acc)
      (let* ((param (list.first params))
             (payload (middle.payload param))
             (ty (core.lower-type
                   (optional.value
                     (record.get payload '|type|)))))
        (core.lower-param-types
          (list.rest params)
          (list.cons ty acc))))))

(set! core.bind-params
  (lambda (function params index locals)
  (if (list.empty? params)
      (list.reverse locals)
      (let* ((param (list.first params))
             (payload (middle.payload param))
             (name (optional.value
                     (record.get payload '|name|)))
             (ty (core.lower-type
                   (optional.value
                     (record.get payload '|type|))))
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
          (list.cons local locals))))))

(set! core.lower-expr
  (lambda (block expr expected-ty locals)
  (let* ((kind (middle.kind expr))
         (lowerer (pipeline.rule '|core-expr-lowerer| kind)))
    (lowerer block expr expected-ty locals))))

(set! core.infer-expr-type
  (lambda (expr locals)
  (let* ((kind (middle.kind expr))
         (payload (middle.payload expr)))
    (cond
      ((symbol=? kind '|middle.expr.path|)
       (let* ((path (optional.value
                     (record.get payload '|path|))))
         (core.local-type locals (core.path-leaf path))))
      ((symbol=? kind '|middle.expr.call|)
       (let* ((callee (optional.value
                       (record.get payload '|callee|))))
         (let* ((callee-payload (middle.payload callee)))
           (core.local-type
             locals
             (core.path-leaf
               (optional.value
                 (record.get callee-payload '|path|)))))))
      ((symbol=? kind '|middle.expr.method-call|)
       (core.function-return-type
         (core.function-by-name
           (optional.value
             (record.get payload '|method|)))))
      ((symbol=? kind '|middle.expr.builtin|)
       (core.infer-expr-type
         (list.first
           (optional.value
             (record.get payload '|args|)))
         locals))
      ((symbol=? kind '|middle.expr.tail-call|)
       (core.infer-expr-type
         (optional.value
           (record.get payload '|call|))
         locals))
      ((symbol=? kind '|middle.expr.if|)
       (type.unsupported '|if-expression-type|))
      ((symbol=? kind '|middle.expr.borrow|)
       (type.unsupported '|borrow-expression-type|))
      ((symbol=? kind '|middle.expr.perform|)
       (type.unsupported '|perform-expression-type|))
      ((symbol=? kind '|middle.expr.resume|)
       (type.unsupported '|resume-expression-type|))
      ((symbol=? kind '|middle.expr.handle|)
       (type.unsupported '|handle-expression-type|))
      ((symbol=? kind '|middle.expr.unary|)
       (core.infer-expr-type
         (optional.value
           (record.get payload '|operand|))
         locals))
      ((symbol=? kind '|middle.expr.binary|)
       (let* ((op (optional.value
                   (record.get payload '|op|))))
         (if (operators.compare-op? op)
             (type.registered '|bool| (list))
             (core.infer-expr-type
               (optional.value
                 (record.get payload '|left|))
               locals))))
      ((symbol=? kind '|middle.expr.bool|)
       (type.registered '|bool| (list)))
      ((symbol=? kind '|middle.expr.string|)
       (type.registered '|addr| (list)))
      ((symbol=? kind '|middle.expr.struct|)
       (core.struct-type
         (optional.value
           (record.get payload '|name|))))
      ((symbol=? kind '|middle.expr.field|)
       (let* ((base (optional.value
                     (record.get payload '|base|)))
              (field-name (optional.value
                            (record.get payload '|field|))))
         (core.struct-field-type-from-type
           (core.infer-expr-type base locals)
           field-name)))
      (else
       (type.unsupported '|inferred-expression-type|))))))

(set! core.lower-stmt
  (lambda (block stmt ret-ty locals)
  (let* ((kind (middle.kind stmt))
         (payload (middle.payload stmt)))
    (cond
      ((symbol=? kind '|middle.stmt.return|)
       (let* ((value (optional.value
                     (record.get payload '|value|))))
         (begin
           (if (optional.none? value)
               (core.return-none! block)
               (core.return-value!
                 block
                 (core.lower-expr
                   block
                   (optional.value value)
                   ret-ty
                   locals)))
           locals)))
      ((symbol=? kind '|middle.stmt.tail|)
       (let* ((expr (optional.value
                      (record.get payload '|expr|))))
         (begin
           (if (symbol=? (middle.kind expr) '|middle.expr.if|)
               (core.lower-if-tail-expr block expr ret-ty locals)
               (if (type.unit? ret-ty)
                   (begin
                     (core.lower-expr
                       block
                       expr
                       ret-ty
                       locals)
                     (core.return-none! block))
                   (core.return-value!
                     block
                     (core.lower-expr
                       block
                       expr
                       ret-ty
                       locals))))
           locals)))
      ((symbol=? kind '|middle.stmt.expr|)
       (let* ((expr (optional.value
                      (record.get payload '|expr|))))
         (begin
           (core.lower-expr
             block
             expr
             (core.infer-expr-type expr locals)
             locals)
           locals)))
      ((symbol=? kind '|middle.stmt.let|)
       (let* ((ty-option (optional.value
                          (record.get payload '|type|))))
         (let* ((name (optional.value
                       (record.get payload '|name|)))
                (mutable (optional.value
                           (record.get payload '|mutable|)))
                (value-expr (optional.value
                              (record.get payload '|value|))))
           (let* ((ty (if (optional.none? ty-option)
                         (core.infer-expr-type value-expr locals)
                         (core.lower-type
                           (optional.value ty-option)))))
             (let* ((value (core.lower-expr block value-expr ty locals)))
               (list.cons
                 (record '|local|
                   (record.field '|name| name)
                   (record.field '|type| ty)
                   (record.field '|mutable| mutable)
                   (record.field '|value| value))
                 locals))))))
      ((symbol=? kind '|middle.stmt.assign|)
       (let* ((target (optional.value
                       (record.get payload '|target|)))
              (value-expr (optional.value
                            (record.get payload '|value|))))
         (if (symbol=? (middle.kind target) '|middle.expr.path|)
             (let* ((target-payload (middle.payload target)))
               (let* ((path (optional.value
                             (record.get target-payload '|path|))))
                 (let* ((name (list.first path)))
                   (if (core.local-mutable? locals name)
                       (let* ((ty (core.local-type locals name)))
                         (let* ((value (core.lower-expr block value-expr ty locals)))
                           (list.cons
                             (record '|local|
                               (record.field '|name| name)
                               (record.field '|type| ty)
                               (record.field '|mutable| #t)
                               (record.field '|value| value))
                             locals)))
                       (type.unsupported '|immutable-assignment|)))))
             (type.unsupported '|assignment-target|))))
      (else locals)))))

(set! core.lower-stmts
  (lambda (block stmts ret-ty locals)
  (if (list.empty? stmts)
      (core.return-none! block)
      (let* ((next-locals
              (core.lower-stmt block (list.first stmts) ret-ty locals)))
        (if (list.empty? (list.rest stmts))
            unit
            (core.lower-stmts
              block
              (list.rest stmts)
              ret-ty
              next-locals))))))
