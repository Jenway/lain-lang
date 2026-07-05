(meta-source "operators/integer")

(register-operator! '|+| '|integer-or-float-add|)
(register-operator! '|-| '|integer-or-float-sub|)
(register-operator! '|*| '|integer-or-float-mul|)
(register-operator! '|/| '|integer-or-float-div|)
(register-operator! '|==| '|equality|)
(register-operator! '|!=| '|inequality|)
(register-operator! '|<| '|ordering-less-than|)
(register-operator! '|<=| '|ordering-less-or-equal|)
(register-operator! '|>| '|ordering-greater-than|)
(register-operator! '|>=| '|ordering-greater-or-equal|)
(register-operator! '|!| '|logical-not|)

(define-pass* 'middle-normalizer '|expr.binary| (lambda (raw-expr)
  (let* ((payload (raw.payload raw-expr)))
    (middle.node! '|operators.binary|
      (record '|operators.binary|
        (record.field '|op|
          (optional.value
            (record.get payload '|op|)))
        (record.field '|left|
          (middle.normalize-expr
            (optional.value
              (record.get payload '|left|))))
        (record.field '|right|
          (middle.normalize-expr
            (optional.value
              (record.get payload '|right|)))))))))

(define-pass* 'middle-normalizer '|expr.unary| (lambda (raw-expr)
  (let* ((payload (raw.payload raw-expr)))
    (middle.node! '|operators.unary|
      (record '|operators.unary|
        (record.field '|op|
          (optional.value
            (record.get payload '|op|)))
        (record.field '|operand|
          (middle.normalize-expr
            (optional.value
              (record.get payload '|operand|)))))))))

(define-pass* 'core-expr-lowerer '|operators.binary| (lambda (block expr expected-ty locals)
  (let* ((payload (middle.payload expr))
        (op (optional.value
              (record.get payload '|op|)))
        (left-expr (optional.value
                     (record.get payload '|left|)))
        (right-expr (optional.value
                      (record.get payload '|right|))))
    (let* ((operand-ty (if (operators.compare-op? op)
                          (operators.infer-operand-type left-expr locals expected-ty)
                          expected-ty)))
      (let* ((left (core.lower-expr
                    block
                    left-expr
                    operand-ty
                    locals))
            (right (core.lower-expr
                     block
                     right-expr
                     operand-ty
                     locals)))
        (cond
          ((type.float? operand-ty)
           (cond
             ((symbol=? op '|+|) (ir.expr.fadd block left right))
             ((symbol=? op '|-|) (ir.expr.fsub block left right))
             ((symbol=? op '|*|) (ir.expr.fmul block left right))
             ((symbol=? op '|/|) (ir.expr.fdiv block left right))
             ((symbol=? op '|==|) (ir.expr.feq block left right))
             ((symbol=? op '|<|) (ir.expr.flt block left right))
             (else (core.unsupported-operator op))))
          (else
           (cond
             ((symbol=? op '|+|) (ir.expr.add block left right))
             ((symbol=? op '|-|) (ir.expr.sub block left right))
             ((symbol=? op '|*|) (ir.expr.mul block left right))
             ((symbol=? op '|/|) (ir.expr.div block left right))
             ((symbol=? op '|==|) (ir.expr.eq block left right))
             ((symbol=? op '|!=|) (ir.expr.ne block left right))
             ((symbol=? op '|<|) (ir.expr.lt block left right))
             ((symbol=? op '|<=|) (ir.expr.le block left right))
             ((symbol=? op '|>|) (ir.expr.gt block left right))
             ((symbol=? op '|>=|) (ir.expr.ge block left right))
             (else (core.unsupported-operator op))))))))))

(define-pass* 'core-expr-lowerer '|operators.unary| (lambda (block expr expected-ty locals)
  (let* ((payload (middle.payload expr))
        (op (optional.value
              (record.get payload '|op|)))
        (operand-expr (optional.value
                        (record.get payload '|operand|))))
    (cond
      ((symbol=? op '|-|)
       (let* ((zero (core.const-bits! block expected-ty 0))
             (operand (core.lower-expr block operand-expr expected-ty locals)))
         (ir.expr.sub block zero operand)))
      ((symbol=? op '|!|)
       (let* ((operand (core.lower-expr block operand-expr expected-ty locals))
             (zero (core.const-bits! block expected-ty 0)))
         (ir.expr.eq block operand zero)))
      (else (core.unsupported-operator op))))))

(define (operators.compare-op? op)
  (cond
    ((symbol=? op '|==|) #t)
    ((symbol=? op '|!=|) #t)
    ((symbol=? op '|<|) #t)
    ((symbol=? op '|<=|) #t)
    ((symbol=? op '|>|) #t)
    ((symbol=? op '|>=|) #t)
    (else #f)))

(define (operators.infer-operand-type expr locals fallback)
  (let* ((kind (middle.kind expr))
         (payload (middle.payload expr)))
    (cond
      ((symbol=? kind '|path.access|)
       (let* ((path (optional.value
                     (record.get payload '|path|))))
         (core.local-type locals (list.first path))))
      ((symbol=? kind '|operators.binary|)
       (operators.infer-operand-type
         (optional.value
           (record.get payload '|left|))
         locals
         fallback))
      (else fallback))))

(define (core.binary-primitive op operand-ty)
  (if (registry.operator-registered? op)
      (if (type.float? operand-ty)
          (cond
            ((symbol=? op '|+|) '|float.add|)
            ((symbol=? op '|-|) '|float.sub|)
            ((symbol=? op '|*|) '|float.mul|)
            ((symbol=? op '|/|) '|float.div|)
            ((symbol=? op '|==|) '|float.eq|)
            ((symbol=? op '|<|) '|float.lt|)
            (else (core.unsupported-operator op)))
          (cond
            ((symbol=? op '|+|) '|integer.add|)
            ((symbol=? op '|-|) '|integer.sub|)
            ((symbol=? op '|*|) '|integer.mul|)
            ((symbol=? op '|/|) '|integer.div|)
            ((symbol=? op '|==|) '|integer.eq|)
            ((symbol=? op '|!=|) '|integer.ne|)
            ((symbol=? op '|<|) '|integer.lt|)
            ((symbol=? op '|<=|) '|integer.le|)
            ((symbol=? op '|>|) '|integer.gt|)
            ((symbol=? op '|>=|) '|integer.ge|)
            (else (core.unsupported-operator op))))
      (core.unsupported-operator op)))

;; ---------------------------------------------------------------------------
;; 表达式类型推导: 运算符
;; ---------------------------------------------------------------------------

(define-pass* 'core-expr-inferer '|operators.binary| (lambda (expr locals)
  (let* ((payload (middle.payload expr))
         (op (optional.value (record.get payload '|op|))))
    (if (operators.compare-op? op)
        (type.registered '|bool| (list))
        (core.infer-expr-type
          (optional.value (record.get payload '|left|))
          locals)))))

(define-pass* 'core-expr-inferer '|operators.unary| (lambda (expr locals)
  (let* ((payload (middle.payload expr))
         (operand (optional.value (record.get payload '|operand|))))
    (core.infer-expr-type operand locals))))
