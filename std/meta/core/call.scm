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
        (record.field '|type-args|
          (let* ((raw-type-args (record.get payload '|type-args|)))
            (if (optional.none? raw-type-args)
                (list)
                (middle.normalize-types
                  (optional.value raw-type-args)
                  (list)))))
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
    ;; 注意: macro 展开目前忽略 type-args (宏不支持泛型参数)
    ;; 后续可扩展为 (expand args type-args)
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

;; 将多段路径拼接为 C 兼容的函数名
;; 单段: Color → Color
;; 多段: [Color, Red] → Color_Red (用 "_" 替代 "::")
(define (core.path-fn-name path)
  (if (list.empty? (list.rest path))
      (list.first path)
      (let loop ((p path) (acc ""))
        (if (list.empty? p)
            (string->symbol acc)
            (let* ((seg (symbol->string (list.first p)))
                   (new-acc (if (string=? acc "")
                                seg
                                (string-append acc "_" seg))))
              (loop (list.rest p) new-acc))))))

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
                   (record.get callee-payload '|path|)))
           (fn-name (core.path-fn-name path)))
      (let* ((intrinsic (core.invoke-intrinsic! fn-name)))
        (if (optional.some? intrinsic)
            ((optional.value intrinsic) block args expected-ty locals)
            (let* ((function (core.function-by-name fn-name)))
              (core.call!
                block
                function
                (core.lower-args
                  block
                  args
                  (core.function-param-types function)
                  locals
                  (list)))))))))

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
       ;; 检测是否为 Dyn 胖指针的动态分发调用
       (let* ((receiver-kind (middle.kind receiver)))
         (cond
           ;; 接收者不是变量路径 — 静态分发
           ((not (symbol=? receiver-kind '|middle.expr.path|))
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
                      (list)))))))
           ;; 接收者是变量路径 — 检查是否为 Dyn 类型
           (else
            (let* ((receiver-payload (middle.payload receiver))
                   (receiver-path (optional.value
                                   (record.get receiver-payload '|path|)))
                   (receiver-name (list.first receiver-path))
                   (receiver-ty (core.local-type locals receiver-name)))
              (cond
                ((interface.dyn-type? receiver-ty)
                 ;; 动态分发: vtable 查找 + 间接调用
                 (impl.lower-dyn-dispatch!
                   block receiver method args locals))
                (else
                 ;; 静态分发回退
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
                           (list))))))))))))))))

(define-pass (middle-normalizer |expr.call-indirect| raw-expr)
  (let* ((payload (raw.payload raw-expr)))
    (middle.node! '|middle.expr.call-indirect|
      (record '|middle.expr.call-indirect|
        (record.field '|fn-ptr|
          (middle.normalize-expr
            (optional.value
              (record.get payload '|fn-ptr|))))
        (record.field '|ret-ty|
          (middle.normalize-type
            (optional.value
              (record.get payload '|ret-ty|))))
        (record.field '|args|
          (middle.normalize-exprs
            (optional.value
              (record.get payload '|args|))
            (list)))))))

(define (core.lower-args-by-inference block args locals acc)
  (if (list.empty? args)
      (list.reverse acc)
      (let* ((arg (list.first args))
             (ty (core.infer-expr-type arg locals))
             (value (core.lower-expr block arg ty locals)))
        (core.lower-args-by-inference
          block (list.rest args) locals (list.cons value acc)))))

(define-pass (core-expr-lowerer |middle.expr.call-indirect| block expr expected-ty locals)
  (let* ((payload (middle.payload expr))
         (fn-ptr (optional.value (record.get payload '|fn-ptr|)))
         (ret-ty (core.lower-type (optional.value (record.get payload '|ret-ty|))))
         (args (optional.value (record.get payload '|args|))))
    (let* ((lowered-fn-ptr (core.lower-expr block fn-ptr (type.addr) locals))
           (lowered-args (core.lower-args-by-inference block args locals (list))))
      (core.call-indirect! block lowered-fn-ptr ret-ty lowered-args))))

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

;; ---------------------------------------------------------------------------
;; 表达式类型推导: 调用相关
;; ---------------------------------------------------------------------------

(define-pass (core-expr-inferer |middle.expr.path| expr locals)
  (let* ((payload (middle.payload expr))
         (path (optional.value (record.get payload '|path|))))
    (core.local-type locals (core.path-leaf path))))

(define-pass (core-expr-inferer |middle.expr.call| expr locals)
  (let* ((payload (middle.payload expr))
         (callee (optional.value (record.get payload '|callee|)))
         (callee-payload (middle.payload callee))
         (path (optional.value (record.get callee-payload '|path|))))
    (core.local-type locals (core.path-leaf path))))

(define-pass (core-expr-inferer |middle.expr.method-call| expr locals)
  (let* ((payload (middle.payload expr))
         (method (optional.value (record.get payload '|method|))))
    (core.function-return-type (core.function-by-name method))))

(define-pass (core-expr-inferer |middle.expr.builtin| expr locals)
  (let* ((payload (middle.payload expr))
         (args (optional.value (record.get payload '|args|))))
    (core.infer-expr-type (list.first args) locals)))

(define-pass (core-expr-inferer |middle.expr.tail-call| expr locals)
  (let* ((payload (middle.payload expr))
         (call (optional.value (record.get payload '|call|))))
    (core.infer-expr-type call locals)))

(define-pass (core-expr-inferer |middle.expr.call-indirect| expr locals)
  (let* ((payload (middle.payload expr))
         (ret-ty (optional.value (record.get payload '|ret-ty|))))
    (core.lower-type ret-ty)))

(define-pass (core-expr-inferer |middle.expr.if| expr locals)
  (type.unsupported '|if-expression-type|))

(define-pass (core-expr-inferer |middle.expr.borrow| expr locals)
  (type.unsupported '|borrow-expression-type|))

(define-pass (core-expr-inferer |middle.expr.perform| expr locals)
  (type.unsupported '|perform-expression-type|))

(define-pass (core-expr-inferer |middle.expr.resume| expr locals)
  (type.unsupported '|resume-expression-type|))

(define-pass (core-expr-inferer |middle.expr.handle| expr locals)
  (type.unsupported '|handle-expression-type|))
