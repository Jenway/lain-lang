(meta-source "expr/lower")

;; ═══════════════════════════════════════════════════════════
;; 函数调用降级 — call.* 系列 + 辅助函数
;; 将 call.fn / call.method / call.indirect / call.tail / call.builtin
;; 从 middle IR 降级为 core IR
;; ═══════════════════════════════════════════════════════════

;; ── 辅助函数 ──

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
  ;;   2. core.begin-if! → creates then/else scratch blocks
  ;;   3. Lower then-body into then-scratch
  ;;   4. Lower else-body into else-scratch
  ;;   5. core.end-if! → builds INST_IF, appends to parent block, frees scratch blocks
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
         (scratch-pair (core.begin-if! block condition-value))
         (then-scratch (car scratch-pair))
         (else-scratch (car (cdr scratch-pair))))
    ;; Lower then-body into then-scratch
    (core.set-current-block! then-scratch)
    (core.lower-stmts
      then-scratch
      (optional.value
        (record.get (middle.payload then-block-expr) '|items|))
      expected-ty
      locals)
    ;; Lower else-body into else-scratch
    (core.set-current-block! else-scratch)
    (core.lower-stmts
      else-scratch
      (optional.value
        (record.get (middle.payload else-block-expr) '|items|))
      expected-ty
      locals)
    ;; Build INST_IF in parent block
    (core.end-if! block condition-value then-scratch else-scratch)))

;; ── core-expr-lowerer 通道 ──

;; pass: calls core.call-or-eval (not a define-pass, a helper)
;; reads: compiler-state.comptime-fns (via comptime-fns.member?)
;; calls: core.eval!, core.call!, core.lower-args, core.function-param-types
;; Helper: dispatch to core.eval! for comptime fns, core.call! otherwise
(define (core.call-or-eval block fn-name function args locals)
  (if (comptime-fns.member? fn-name)
      (core.eval! block function
        (core.lower-args block args (core.function-param-types function) locals (list)))
      (core.call! block function
        (core.lower-args block args (core.function-param-types function) locals (list)))))

(define-pass (core-expr-lowerer |call.fn| block expr expected-ty locals)
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
            (let* ((function (core.function-by-name fn-name)))
              (core.call-or-eval block fn-name function args locals)))))))

(define-pass (core-expr-lowerer |call.method| block expr expected-ty locals)
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
           ((not (symbol=? receiver-kind '|path.access|))
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

(define-pass (core-expr-lowerer |call.indirect| block expr expected-ty locals)
  (let* ((payload (middle.payload expr))
         (fn-ptr (optional.value (record.get payload '|fn-ptr|)))
         (ret-ty (core.lower-type (optional.value (record.get payload '|ret-ty|))))
         (args (optional.value (record.get payload '|args|))))
    (let* ((lowered-fn-ptr (core.lower-expr block fn-ptr (type.addr) locals))
           (lowered-args (core.lower-args-by-inference block args locals (list))))
      (core.call-indirect! block lowered-fn-ptr ret-ty lowered-args))))

(define-pass (core-expr-lowerer |call.builtin| block expr expected-ty locals)
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

(define-pass (core-expr-lowerer |call.tail| block expr expected-ty locals)
  (let* ((payload (middle.payload expr)))
    (core.lower-expr
      block
      (optional.value
        (record.get payload '|call|))
      expected-ty
      locals)))

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
         (handler-fn (core.function-by-name handler-name))
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
                 (flag-val (core.field-offset! block call-expr flag-offset flag-ty))
                 ;; Create ok/err blocks
                 (function (core.block-function block))
                 (ok-block (core.append-block! function))
                 (err-block (core.append-block! function))
                 ;; Branch on flag == 0
                 (flag-zero (core.const-bits! block flag-ty 0))
                 (is-ok (core.primitive! block '|integer.eq| (list flag-val flag-zero) (core.make-bits 1))))
            (core.cond-branch! block is-ok ok-block err-block)
            ;; ── Error path: extract all arg fields, call handler ──
            (core.set-current-block! err-block)
            (let* ((arg-vals
                     (map (lambda (idx)
                            (let* ((off-ty (list-ref offsets idx)))
                              (core.field-offset! err-block call-expr (car off-ty) (cdr off-ty))))
                          arg-indices))
                   (handler-ret (core.call! err-block handler-fn arg-vals)))
              (core.return-value! err-block handler-ret))
            ;; ── Ok path: extract value fields, return ──
            (core.set-current-block! ok-block)
            (if (null? value-indices)
                ;; No value fields — return void (unit)
                (core.return-none! ok-block)
                ;; Return the first value field
                ;; (multi-value effects would build an aggregate here)
                (let* ((val-idx (car value-indices))
                       (off-ty (list-ref offsets val-idx))
                       (val-expr (core.field-offset! ok-block call-expr (car off-ty) (cdr off-ty))))
                  (core.return-value! ok-block val-expr))))))))
