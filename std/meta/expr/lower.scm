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

(define (core.flatten-path-fn-name path)
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

;; 将多段路径拼接为 C 兼容的函数名
;; 单段: Color → Color
;; 多段: [Color, Red] → Color_Red (用 "_" 替代 "::")
(define (core.path-fn-name path)
  (let* ((resolved (import.resolve-qualified-symbol path)))
    (if resolved
        resolved
        (core.flatten-path-fn-name path))))

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
                 (record.get payload '|path|)))
         (imported (import.resolve-qualified-symbol path)))
    (if imported
        (ir.sub.by-name imported)
        (core.local-lookup locals (core.path-leaf path)))))


;; Helper: dispatch to ir.expr.eval for comptime fns, ir.expr.call otherwise
(define (core.call-or-eval block fn-name function args locals)
  (if (comptime? fn-name)
      (ir.expr.eval block function
        (core.lower-args block args (ir.sub.params function) locals (list)))
      (ir.expr.call block function
        (core.lower-args block args (ir.sub.params function) locals (list)))))

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
      ;; Phase 2: propagate callee effects to caller
      (let* ((callee-effects (propagate.lookup-fn-effects fn-name)))
        (propagate.record-effects! callee-effects))
      (let* ((intrinsic (core.invoke-intrinsic! fn-name)))
        (if (optional.some? intrinsic)
            ((optional.value intrinsic) block args expected-ty locals)
            (let* ((function (ir.sub.by-name fn-name)))
              (core.call-or-eval block fn-name function args locals)))))))

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
;; Uses the Throws effect layout from effects/layout.scm.
;; Layout: {flag: u8, value: T, error: E} — flag at index 0, value at index 1.
(define-pass (core-expr-lowerer |middle.expr.question| block expr expected-ty locals)
  (let* ((payload (middle.payload expr))
         (inner-expr (optional.value (record.get payload '|expr|)))
         (call-expr (core.lower-expr block inner-expr expected-ty locals))
         ;; Query Throws layout for flag field info
         (throws-layout (effect.lookup-layout '|Throws| '|throw|))
         (offsets    (if throws-layout (cadr throws-layout) #f))
         (flag-index (if throws-layout (caddr throws-layout) 0))
         ;; flag field at flag-index
         (flag-offset-ty (if offsets (list-ref offsets flag-index) (cons 0 (ir.type.bits 8))))
         (flag-offset (car flag-offset-ty))
         (flag-ty     (cdr flag-offset-ty))
         ;; value field at index 1
         (value-offset-ty (if offsets (list-ref offsets 1) (cons 4 (ir.type.bits 32))))
         (value-offset (car value-offset-ty))
         (flag-val (ir.expr.field-offset block call-expr flag-offset flag-ty))
         (function (ir.block.parent block))
         (cont-block (ir.sub.block function))
         (err-block (ir.sub.block function))
         (flag-zero (ir.expr.const block flag-ty 0))
         (is-ok (ir.expr.primitive block '|integer.eq| (list flag-val flag-zero) (ir.type.bits 1))))
    (ir.term.cond-branch block is-ok cont-block err-block)
    ;; Error path: return the product (propagate error)
    (ir.block.set! err-block)
    (ir.term.return err-block call-expr)
    ;; Ok path: extract value field
    (ir.block.set! cont-block)
    (let* ((value-ty (core.product-value-type expected-ty))
           (value-val (ir.expr.field-offset cont-block call-expr value-offset value-ty)))
      value-val)))

;; ── handle lowering: generic, layout-driven ──
;; For any effect E with handler fn H, given `handle E with H { body }`:
;;   1. Look up E's product layout from the registry
;;   2. Lower body tail expr → product
;;   3. Extract flag field; branch
;;   4. flag == 0 → extract value fields, return
;;   5. flag != 0 → extract arg fields, call handler with all of them
;; The handler function's parameter types must match the arg field types
;; in the effect's layout.
(define (core.lower-handle-tail-expr block expr expected-ty locals)
  (let* ((payload (middle.payload expr))
         (body-block (optional.value (record.get payload '|body|)))
         (body-items (optional.value (record.get (middle.payload body-block) '|items|)))
         (handler-expr (optional.value (record.get payload '|handler|)))
         ;; Get handler function name from path
         (handler-payload (middle.payload handler-expr))
         (handler-path (optional.value (record.get handler-payload '|path|)))
         (handler-name (if handler-path (list.first handler-path) '|unknown|))
         (handler-fn (ir.sub.by-name handler-name))
         ;; Extract effect info — lookup in registry
         (effect-name (optional.value (record.get payload '|effect|)))
         (layout (effect.lookup-layout effect-name #f)))
    (if (not layout)
        ;; No layout registered for this effect — can't lower handle
        (let* ((msg (string-append "cannot lower handle for effect '"
                                   (symbol->string effect-name)
                                   "': no product layout in registry")))
          (error msg))
        (let* ((total-size   (car layout))
               (offsets      (cadr layout))
               (flag-index   (caddr layout))
               (arg-indices  (cadddr layout))
               (num-fields   (length offsets))
               ;; Compute value indices: all non-flag, non-arg indices
               (value-indices
                 (let loop ((i 0) (acc '()))
                   (if (>= i num-fields)
                       (reverse acc)
                       (if (or (= i flag-index)
                               (effect.index-in-list? i arg-indices))
                           (loop (+ i 1) acc)
                           (loop (+ i 1) (cons i acc))))))
               ;; flag field info
               (flag-offset-ty (list-ref offsets flag-index))
               (flag-offset (car flag-offset-ty))
               (flag-ty     (cdr flag-offset-ty))
               ;; Build product type from field types
               (field-types (map cdr offsets))
               (product-ty  (type.product field-types))
               ;; Get the tail expression from body (last statement)
               (last-stmt (list-ref body-items (- (length body-items) 1)))
               (tail-expr (optional.value (record.get (middle.payload last-stmt) '|expr|))))
          ;; Lower body call → returns product
          (let* ((call-expr (core.lower-expr block tail-expr product-ty locals))
                 ;; Phase 2: the handle absorbs this effect — consume it
                 (_consume (propagate.consume! effect-name))
                 ;; Extract flag field
                 (flag-val (ir.expr.field-offset block call-expr flag-offset flag-ty))
                 ;; Create ok/err blocks
                 (function (ir.block.parent block))
                 (ok-block (ir.sub.block function))
                 (err-block (ir.sub.block function))
                 ;; Branch on flag == 0
                 (flag-zero (ir.expr.const block flag-ty 0))
                 (is-ok (ir.expr.primitive block '|integer.eq| (list flag-val flag-zero) (ir.type.bits 1))))
            (ir.term.cond-branch block is-ok ok-block err-block)
            ;; ── Error path: extract all arg fields, call handler ──
            (ir.block.set! err-block)
            (let* ((arg-vals
                     (map (lambda (idx)
                            (let* ((off-ty (list-ref offsets idx)))
                              (ir.expr.field-offset err-block call-expr (car off-ty) (cdr off-ty))))
                          arg-indices))
                   (handler-ret (ir.expr.call err-block handler-fn arg-vals)))
              (ir.term.return err-block handler-ret))
            ;; ── Ok path: extract value fields, return ──
            (ir.block.set! ok-block)
            (if (null? value-indices)
                ;; No value fields — return void (unit)
                (ir.term.return-none ok-block)
                ;; Return the first value field
                ;; (multi-value effects would build an aggregate here)
                (let* ((val-idx (car value-indices))
                       (off-ty (list-ref offsets val-idx))
                       (val-expr (ir.expr.field-offset ok-block call-expr (car off-ty) (cdr off-ty))))
                  (ir.term.return ok-block val-expr))))))))
