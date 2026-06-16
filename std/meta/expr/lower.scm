(meta-source "expr/lower")

;; ═══════════════════════════════════════════════════════════
;; 表达式降级 — core-expr-lowerer 通道 + 辅助函数
;; 将 middle IR 的表达式节点降级为 core IR
;; ═══════════════════════════════════════════════════════════

;; ── 辅助函数 ──

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
      (ir.sub.by-name name)
      (let* ((local (list.first locals))
             (local-name (optional.value
                           (record.get local '|name|))))
        (if (symbol=? local-name name)
            (optional.value
              (record.get local '|value|))
            (core.local-lookup (list.rest locals) name)))))

(define (core.local-type locals name)
  (if (list.empty? locals)
      (ir.sub.ret-type
        (ir.sub.by-name name))
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

(define (core.lower-args-by-inference block args locals acc)
  (if (list.empty? args)
      (list.reverse acc)
      (let* ((arg (list.first args))
             (ty (core.infer-expr-type arg locals))
             (value (core.lower-expr block arg ty locals)))
        (core.lower-args-by-inference
          block (list.rest args) locals (list.cons value acc)))))

(define (core.lower-if-tail-expr block expr expected-ty locals)
  ;; Structured if lowering: uses INST_IF instead of manual cond_br + block switching.
  ;; Flow:
  ;;   1. Evaluate condition in parent block
  ;;   2. ir.inst.begin-if → creates then/else scratch blocks
  ;;   3. Lower then-body into then-scratch
  ;;   4. Lower else-body into else-scratch
  ;;   5. ir.inst.end-if → builds INST_IF, appends to parent block, frees scratch blocks
  (let* ((payload (middle.payload expr))
         (condition (optional.value
                      (record.get payload '|condition|)))
         (then-block-expr (optional.value
                            (record.get payload '|then|)))
         (else-block-expr (optional.value
                            (record.get payload '|else|)))
         ;; Evaluate condition in parent block
         (condition-value
           (core.lower-expr block condition (type.bits 1) locals))
         ;; Create scratch blocks
         (scratch-pair (ir.inst.begin-if block condition-value))
         (then-scratch (car scratch-pair))
         (else-scratch (car (cdr scratch-pair))))
    ;; Lower then-body into then-scratch
    (ir.block.set! then-scratch)
    (core.lower-stmts
      then-scratch
      (optional.value
        (record.get (middle.payload then-block-expr) '|items|))
      expected-ty
      locals)
    ;; Lower else-body into else-scratch
    (ir.block.set! else-scratch)
    (core.lower-stmts
      else-scratch
      (optional.value
        (record.get (middle.payload else-block-expr) '|items|))
      expected-ty
      locals)
    ;; Build INST_IF in parent block
    (ir.inst.end-if block condition-value then-scratch else-scratch)))

;; ── core-expr-lowerer 通道 ──

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
            (let* ((function (ir.sub.by-name fn-name)))
              (ir.expr.call
                block
                function
                (core.lower-args
                  block
                  args
                  (ir.sub.params function)
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
       (ir.expr.primitive
         block
         '|cpu.popcount|
         (list (core.lower-expr block receiver expected-ty locals))
         expected-ty))
      ((symbol=? method '|leading_zeros|)
       (ir.expr.primitive
         block
         '|cpu.leading-zeros|
         (list (core.lower-expr block receiver expected-ty locals))
         expected-ty))
      ((symbol=? method '|bswap|)
       (ir.expr.primitive
         block
         '|cpu.bswap|
         (list (core.lower-expr block receiver expected-ty locals))
         expected-ty))
      ((symbol=? method '|rotate_left|)
       (ir.expr.primitive
         block
         '|cpu.rotate-left|
         (list
           (core.lower-expr block receiver expected-ty locals)
           (core.lower-expr block (list.first args) expected-ty locals))
         expected-ty))
      ((symbol=? method '|extract_bits|)
       (ir.expr.primitive
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
            (let* ((function (ir.sub.by-name method)))
              (let* ((param-types (ir.sub.params function)))
                (ir.expr.call
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
                 (let* ((function (ir.sub.by-name method)))
                   (let* ((param-types (ir.sub.params function)))
                     (ir.expr.call
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

(define-pass (core-expr-lowerer |middle.expr.call-indirect| block expr expected-ty locals)
  (let* ((payload (middle.payload expr))
         (fn-ptr (optional.value (record.get payload '|fn-ptr|)))
         (ret-ty (core.lower-type (optional.value (record.get payload '|ret-ty|))))
         (args (optional.value (record.get payload '|args|))))
    (let* ((lowered-fn-ptr (core.lower-expr block fn-ptr (type.addr) locals))
           (lowered-args (core.lower-args-by-inference block args locals (list))))
      (ir.expr.call-indirect block lowered-fn-ptr ret-ty lowered-args))))

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

(define-pass (core-expr-lowerer |middle.expr.borrow| block expr expected-ty locals)
  (core.unsupported-expr '|borrow-expression|))

(define-pass (core-expr-lowerer |middle.expr.perform| block expr expected-ty locals)
  (core.unsupported-expr '|perform-expression|))

(define-pass (core-expr-lowerer |middle.expr.resume| block expr expected-ty locals)
  (core.unsupported-expr '|resume-expression|))

(define-pass (core-expr-lowerer |middle.expr.handle| block expr expected-ty locals)
  (core.unsupported-expr '|handle-expression|))

;; ── ? 操作符 lowering: check flag → propagate or unwrap ──
(define-pass (core-expr-lowerer |middle.expr.question| block expr expected-ty locals)
  ;; throws product layout: {i8@0, i32@4, i32@8} — hardcoded offsets
  (let* ((payload (middle.payload expr))
         (inner-expr (optional.value (record.get payload '|expr|)))
         (call-expr (core.lower-expr block inner-expr expected-ty locals))
         (flag-ty (ir.type.bits 8))
         (flag-val (ir.expr.field-offset block call-expr 0 flag-ty))  ;; offset 0
         (function (ir.block.parent block))
         (cont-block (ir.sub.block function))
         (err-block (ir.sub.block function))
         (flag-zero (ir.expr.const block flag-ty 0))
         (is-ok (ir.expr.primitive block '|integer.eq| (list flag-val flag-zero) (ir.type.bits 1))))
    (ir.term.cond-branch block is-ok cont-block err-block)
    ;; Error path: return the product (propagate error)
    (ir.block.set! err-block)
    (ir.term.return err-block call-expr)
    ;; Ok path: extract value field (field index 1, offset 4)
    (ir.block.set! cont-block)
    (let* ((value-ty (core.product-value-type expected-ty))
           (value-val (ir.expr.field-offset cont-block call-expr 4 value-ty)))  ;; offset 4
      value-val)))

;; ── handle lowering: body → check flag → call handler or unwrap ──
(define (core.lower-handle-tail-expr block expr expected-ty locals)
  ;; For `handle Throws<E> with handler_fn { body }`:
  ;;   1. Lower body tail expr (call to throws fn → returns product)
  ;;   2. Check flag: if 0 → unwrap value; if 1 → call handler_fn(error)
  (let* ((payload (middle.payload expr))
         (body-block (optional.value (record.get payload '|body|)))
         (body-items (optional.value (record.get (middle.payload body-block) '|items|)))
         (handler-expr (optional.value (record.get payload '|handler|)))
         ;; Get handler function name from path
         (handler-payload (middle.payload handler-expr))
         (handler-path (optional.value (record.get handler-payload '|path|)))
         (handler-name (list.first handler-path))
         (handler-fn (ir.sub.by-name handler-name))
         ;; Build throws product type: {i8 flag, i32 value, i32 error}
         ;; Offsets: flag@0, value@4, error@8
         (throws-ty (type.product (list (ir.type.bits 8) (ir.type.bits 32) (ir.type.bits 32))))
         ;; Get the tail expression from body (last statement)
         (last-stmt (list-ref body-items (- (length body-items) 1)))
         (tail-expr (optional.value (record.get (middle.payload last-stmt) '|expr|))))
         ;; Lower body call → emits INST_CALL, returns EXPR_CALL (product)
         (let* ((call-expr (core.lower-expr block tail-expr throws-ty locals))
            ;; Extract flag field (field 0, offset 0, i8)
            (flag-ty (ir.type.bits 8))
            (flag-val (ir.expr.field-offset block call-expr 0 flag-ty))
            ;; Create ok/err blocks
            (function (ir.block.parent block))
            (ok-block (ir.sub.block function))
            (err-block (ir.sub.block function))
            ;; Branch on flag == 0
            (flag-zero (ir.expr.const block flag-ty 0))
            (is-ok (ir.expr.primitive block '|integer.eq| (list flag-val flag-zero) (ir.type.bits 1))))
         (ir.term.cond-branch block is-ok ok-block err-block)
         ;; Error path: extract error (field 2, offset 8), call handler, return
         (ir.block.set! err-block)
         (let* ((err-ty (ir.type.bits 32))
              (err-val (ir.expr.field-offset err-block call-expr 8 err-ty))
              (handler-ret (ir.expr.call err-block handler-fn (list err-val))))
         (ir.term.return err-block handler-ret))
         ;; Ok path: extract value (field 1, offset 4), return
         (ir.block.set! ok-block)
         (let* ((val-ty (ir.type.bits 32))
              (val-expr (ir.expr.field-offset ok-block call-expr 4 val-ty)))
         (ir.term.return ok-block val-expr)))))
