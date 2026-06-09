(meta-source "core/call")

(define-pass (middle-normalizer |expr.path| raw-expr)
  (let* ((payload (raw.payload raw-expr)))
    (middle.node! '|middle.expr.path|
      (record '|middle.expr.path|
        (record.field '|path|
          (optional.value
            (record.get payload '|path|)))))))

(define-pass (middle-normalizer |expr.call| raw-expr)
  (let* ((payload (raw.payload raw-expr)))
    (middle.node! '|middle.expr.call|
      (record '|middle.expr.call|
        (record.field '|callee|
          (middle.normalize-expr
            (optional.value
              (record.get payload '|callee|))))
        (record.field '|args|
          (middle.normalize-exprs
            (optional.value
              (record.get payload '|args|))
            (list)))))))

(define-pass (middle-normalizer |expr.method-call| raw-expr)
  (let* ((payload (raw.payload raw-expr)))
    (middle.node! '|middle.expr.method-call|
      (record '|middle.expr.method-call|
        (record.field '|receiver|
          (middle.normalize-expr
            (optional.value
              (record.get payload '|receiver|))))
        (record.field '|method|
          (optional.value
            (record.get payload '|method|)))
        (record.field '|args|
          (middle.normalize-exprs
            (optional.value
              (record.get payload '|args|))
            (list)))))))

(define-pass (middle-normalizer |expr.builtin| raw-expr)
  (let* ((payload (raw.payload raw-expr)))
    (middle.node! '|middle.expr.builtin|
      (record '|middle.expr.builtin|
        (record.field '|name|
          (optional.value
            (record.get payload '|name|)))
        (record.field '|args|
          (middle.normalize-exprs
            (optional.value
              (record.get payload '|args|))
            (list)))))))

(define-pass (middle-normalizer |expr.tail-call| raw-expr)
  (let* ((payload (raw.payload raw-expr)))
    (middle.node! '|middle.expr.tail-call|
      (record '|middle.expr.tail-call|
        (record.field '|call|
          (middle.normalize-expr
            (optional.value
              (record.get payload '|call|))))))))

(define-pass (middle-normalizer |expr.macro-call| raw-expr)
  (let* ((payload (raw.payload raw-expr))
         (name (optional.value
                 (record.get payload '|name|)))
         (args (optional.value
                 (record.get payload '|args|)))
         (expand (pipeline.rule '|expression-macro| name)))
    (middle.normalize-expr (expand args))))

(define-pass (middle-normalizer |expr.if| raw-expr)
  (let* ((payload (raw.payload raw-expr)))
    (middle.node! '|middle.expr.if|
      (record '|middle.expr.if|
        (record.field '|condition|
          (middle.normalize-expr
            (optional.value
              (record.get payload '|condition|))))
        (record.field '|then|
          (middle.normalize-block
            (optional.value
              (record.get payload '|then|))))
        (record.field '|else|
          (middle.normalize-block
            (optional.value
              (record.get payload '|else|))))))))

(define-pass (middle-normalizer |expr.borrow| raw-expr)
  (let* ((payload (raw.payload raw-expr)))
    (middle.node! '|middle.expr.borrow|
      (record '|middle.expr.borrow|
        (record.field '|mutable|
          (optional.value
            (record.get payload '|mutable|)))
        (record.field '|operand|
          (middle.normalize-expr
            (optional.value
              (record.get payload '|operand|))))))))

(define-pass (middle-normalizer |expr.perform| raw-expr)
  (let* ((payload (raw.payload raw-expr)))
    (middle.node! '|middle.expr.perform|
      (record '|middle.expr.perform|
        (record.field '|call|
          (middle.normalize-expr
            (optional.value
              (record.get payload '|call|))))))))

(define-pass (middle-normalizer |expr.resume| raw-expr)
  (let* ((payload (raw.payload raw-expr)))
    (middle.node! '|middle.expr.resume|
      (record '|middle.expr.resume|
        (record.field '|value|
          (let* ((value (optional.value
                         (record.get payload '|value|))))
            (if (optional.none? value)
                (optional.none)
                (optional.some
                  (middle.normalize-expr
                    (optional.value value))))))))))

(define-pass (middle-normalizer |expr.handle| raw-expr)
  (let* ((payload (raw.payload raw-expr)))
    (middle.node! '|middle.expr.handle|
      (record '|middle.expr.handle|
        (record.field '|effect|
          (optional.value
            (record.get payload '|effect|)))
        (record.field '|effect-args|
          (middle.normalize-types
            (optional.value
              (record.get payload '|effect-args|))
            (list)))
        (record.field '|handler|
          (optional.value
            (record.get payload '|handler|)))
        (record.field '|body|
          (middle.normalize-block
            (optional.value
              (record.get payload '|body|))))))))

(define (core.path-leaf path)
  (if (list.empty? (list.rest path))
      (list.first path)
      (core.path-leaf (list.rest path))))

(define (core.local-lookup locals name)
  (if (list.empty? locals)
      (core.function-by-name name)
      (let* ((local (list.first locals))
             (local-name (optional.value
                           (record.get local '|name|))))
        (if (symbol=? local-name name)
            (optional.value
              (record.get local '|value|))
            (core.local-lookup (list.rest locals) name)))))

(define (core.local-type locals name)
  (if (list.empty? locals)
      (core.function-return-type
        (core.function-by-name name))
      (let* ((local (list.first locals))
             (local-name (optional.value
                           (record.get local '|name|))))
        (if (symbol=? local-name name)
            (optional.value
              (record.get local '|type|))
            (core.local-type (list.rest locals) name)))))

(define (core.local-mutable? locals name)
  (if (list.empty? locals)
      #f
      (let* ((local (list.first locals))
             (local-name (optional.value
                           (record.get local '|name|))))
        (if (symbol=? local-name name)
            (let* ((mutable (record.get local '|mutable|)))
              (if (optional.none? mutable)
                  #f
                  (optional.value mutable)))
            (core.local-mutable? (list.rest locals) name)))))

(define (core.lower-args block args param-types locals acc)
  (if (list.empty? args)
      (list.reverse acc)
      (let* ((arg (list.first args))
             (ty (list.first param-types))
             (value (core.lower-expr block arg ty locals)))
        (core.lower-args
          block
          (list.rest args)
          (list.rest param-types)
          locals
          (list.cons value acc)))))

(define-pass (core-expr-lowerer |middle.expr.path| block expr expected-ty locals)
  (let* ((payload (middle.payload expr))
         (path (optional.value
                 (record.get payload '|path|))))
    (core.local-lookup locals (core.path-leaf path))))

(define-pass (core-expr-lowerer |middle.expr.call| block expr expected-ty locals)
  (let* ((payload (middle.payload expr))
         (callee (optional.value
                   (record.get payload '|callee|)))
         (args (optional.value
                 (record.get payload '|args|))))
    (let* ((callee-payload (middle.payload callee))
           (path (optional.value
                   (record.get callee-payload '|path|))))
      (let* ((function (core.function-by-name
                         (core.path-leaf path))))
        (core.call!
          block
          function
          (core.lower-args
            block
            args
            (core.function-param-types function)
            locals
            (list)))))))

(define-pass (core-expr-lowerer |middle.expr.method-call| block expr expected-ty locals)
  (let* ((payload (middle.payload expr))
         (receiver (optional.value
                     (record.get payload '|receiver|)))
         (method (optional.value
                   (record.get payload '|method|)))
         (args (optional.value
                 (record.get payload '|args|))))
    (cond
      ((symbol=? method '|popcount|)
       (core.primitive!
         block
         '|cpu.popcount|
         (list (core.lower-expr block receiver expected-ty locals))
         expected-ty))
      ((symbol=? method '|leading_zeros|)
       (core.primitive!
         block
         '|cpu.leading-zeros|
         (list (core.lower-expr block receiver expected-ty locals))
         expected-ty))
      ((symbol=? method '|bswap|)
       (core.primitive!
         block
         '|cpu.bswap|
         (list (core.lower-expr block receiver expected-ty locals))
         expected-ty))
      ((symbol=? method '|rotate_left|)
       (core.primitive!
         block
         '|cpu.rotate-left|
         (list
           (core.lower-expr block receiver expected-ty locals)
           (core.lower-expr block (list.first args) expected-ty locals))
         expected-ty))
      ((symbol=? method '|extract_bits|)
       (core.primitive!
         block
         '|cpu.extract-bits|
         (list
           (core.lower-expr block receiver expected-ty locals)
           (core.lower-expr block (list.first args) expected-ty locals))
         expected-ty))
      (else
       (let* ((function (core.function-by-name method)))
         (let* ((param-types (core.function-param-types function)))
           (core.call!
             block
             function
             (list.cons
               (core.lower-expr
                 block
                 receiver
                 (list.first param-types)
                 locals)
               (core.lower-args
                 block
                 args
                 (list.rest param-types)
                 locals
                 (list))))))))))

(define-pass (core-expr-lowerer |middle.expr.builtin| block expr expected-ty locals)
  (let* ((payload (middle.payload expr))
         (name (optional.value
                 (record.get payload '|name|)))
         (args (optional.value
                 (record.get payload '|args|))))
    (cond
      ((symbol=? name '|likely|)
       (core.lower-expr block (list.first args) expected-ty locals))
      ((symbol=? name '|unlikely|)
       (core.lower-expr block (list.first args) expected-ty locals))
      ((symbol=? name '|fma|)
       (core.lower-expr block (list.first args) expected-ty locals))
      (else
       (core.unsupported-expr name)))))

(define-pass (core-expr-lowerer |middle.expr.tail-call| block expr expected-ty locals)
  (let* ((payload (middle.payload expr)))
    (core.lower-expr
      block
      (optional.value
        (record.get payload '|call|))
      expected-ty
      locals)))

(define-pass (core-expr-lowerer |middle.expr.if| block expr expected-ty locals)
  (core.unsupported-expr '|if-expression|))

(define (core.lower-if-tail-expr block expr expected-ty locals)
  (let* ((payload (middle.payload expr))
         (condition (optional.value
                      (record.get payload '|condition|)))
         (then-block-expr (optional.value
                            (record.get payload '|then|)))
         (else-block-expr (optional.value
                            (record.get payload '|else|)))
         (function (core.block-function block))
         (then-block (core.append-block! function))
         (else-block (core.append-block! function))
         (condition-value
           (core.lower-expr block condition (type.bits 1) locals)))
    (core.cond-branch! block condition-value then-block else-block)
    (core.lower-stmts
      then-block
      (optional.value
        (record.get (middle.payload then-block-expr) '|items|))
      expected-ty
      locals)
    (core.lower-stmts
      else-block
      (optional.value
        (record.get (middle.payload else-block-expr) '|items|))
      expected-ty
      locals)))

(define-pass (core-expr-lowerer |middle.expr.borrow| block expr expected-ty locals)
  (core.unsupported-expr '|borrow-expression|))

(define-pass (core-expr-lowerer |middle.expr.perform| block expr expected-ty locals)
  (core.unsupported-expr '|perform-expression|))

(define-pass (core-expr-lowerer |middle.expr.resume| block expr expected-ty locals)
  (core.unsupported-expr '|resume-expression|))

(define-pass (core-expr-lowerer |middle.expr.handle| block expr expected-ty locals)
  (core.unsupported-expr '|handle-expression|))
