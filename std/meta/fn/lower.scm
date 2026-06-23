(meta-source "fn/lower")

;; ═══════════════════════════════════════════════════════════════════════════
;; Comptime function registry and const table are in compiler-state.scm.
;; Use comptime-fns.register!, comptime-fns.member?, const-table.register!,
;; and const-table.lookup.
;; ═══════════════════════════════════════════════════════════════════════════


(define (fn.attr-named-string args name fallback)
  (if (list.empty? args) fallback
      (let* ((arg (list.first args)) (kind (raw.kind arg)) (payload (raw.payload arg)))
        (if (symbol=? kind '|attr.arg.named|)
            (let* ((arg-name-path (optional.value (record.get payload '|name|))) (arg-name (list.first arg-name-path)))
              (if (symbol=? arg-name name)
                  (let* ((value (optional.value (record.get payload '|value|))))
                    (if (symbol=? (raw.kind value) '|attr.arg.string|)
                        (optional.value (record.get (raw.payload value) '|raw|)) fallback))
                  (fn.attr-named-string (list.rest args) name fallback)))
            (fn.attr-named-string (list.rest args) name fallback)))))

(define (fn.foreign-link-name attrs fallback)
  (if (list.empty? attrs) fallback
      (let* ((attr (list.first attrs)) (payload (raw.payload attr)) (attr-name (optional.value (record.get payload '|name|))))
        (if (symbol=? attr-name '|foreign|)
            (fn.attr-named-string (optional.value (record.get payload '|args|)) '|link_name| fallback)
            (fn.foreign-link-name (list.rest attrs) fallback)))))

(define (fn.cfg-target-os attrs)
  (if (list.empty? attrs) (optional.none)
      (let* ((attr (list.first attrs)) (payload (raw.payload attr)) (attr-name (optional.value (record.get payload '|name|))))
        (if (symbol=? attr-name '|cfg|)
            (optional.some (fn.attr-named-string (optional.value (record.get payload '|args|)) '|target_os| '|unknown|))
            (fn.cfg-target-os (list.rest attrs))))))

(define (fn.cfg-enabled? attrs)
  (let* ((target-os (fn.cfg-target-os attrs)))
    (if (optional.none? target-os) #t (symbol=? (optional.value target-os) (cfg.target-os)))))

;; ── Effect helpers (generalized from Throws-only) ──

(define (fn.effect-name effect)
  ;; effect is a raw node: (effect-name <name> <args>)
  ;; Returns the effect name symbol
  (let* ((payload (raw.payload effect)))
    (optional.value (record.get payload '|name|))))

(define (fn.effect-args effect)
  ;; Returns the type arguments list (raw type nodes)
  (let* ((payload (raw.payload effect)))
    (optional.value (record.get payload '|args|))))

(define (fn.has-declared-effects? effects)
  ;; effects may be optional.none or an empty list
  (and effects (optional.some? effects)
       (not (list.empty? (optional.value effects)))))

(define (fn.first-effect-name effects)
  ;; Returns the first declared effect name symbol, or #f
  (if (fn.has-declared-effects? effects)
      (fn.effect-name (list.first (optional.value effects)))
      #f))

(define (fn.effects-contains-throws? effects)
  ;; Kept for backward compat — checks if Throws is in declared effects
  (if (or (optional.none? effects) (not effects))
      #f
      (if (list.empty? effects)
          #f
          (let* ((first (list.first effects)))
            (if (symbol=? (fn.effect-name first) '|Throws|)
                #t
                (fn.effects-contains-throws? (list.rest effects)))))))

(define (fn.throws-error-type effects)
  ;; Returns the lowered error type (L1Type* cpointer), or #f if no Throws
  (if (or (optional.none? effects) (not effects))
      #f
      (if (list.empty? effects)
          #f
          (let* ((first (list.first effects)))
            (if (symbol=? (fn.effect-name first) '|Throws|)
                (let* ((args (fn.effect-args first)))
                  (if (list.empty? args)
                      (core.make-bits 32)  ;; default error type i32
                      (core.lower-type (list.first args))))
                (fn.throws-error-type (list.rest effects)))))))

(define (fn.make-throws-product ret-ty error-ty)
  ;; Create TY_PRODUCT: {flag: i8, value: ret_ty, error: error_ty}
  (type.product (list (core.make-bits 8) ret-ty error-ty)))

;; ── Generalized: build product type from any effect declaration ──
;; Looks up the effect's layout in the registry and builds a product
;; from the layout's field types. Falls back to raw-ret if no layout.
;;
;; Layout defines: {flag: i8, value: T, arg1: A1, arg2: A2, ...}
;; We substitute the raw return type as the value field type.
;; For effects with no value field in the layout (only flag + args),
;; we insert the value field after the flag.
(define (fn.make-effect-product-name effects raw-ret effect-name)
  (let* ((layout (effect.lookup-layout effect-name #f)))
    (if (not layout)
        raw-ret
        (let* ((offsets     (cadr layout))
               (flag-index  (caddr layout))
               (arg-indices (cadddr layout))
               (num-fields  (length offsets))
               ;; Compute value indices: all non-flag, non-arg
               (value-indices
                 (let loop ((i 0) (acc '()))
                   (if (>= i num-fields)
                       (reverse acc)
                       (if (or (= i flag-index)
                               (effect.index-in-list? i arg-indices))
                           (loop (+ i 1) acc)
                           (loop (+ i 1) (cons i acc))))))
               ;; Build field types, substituting the value fields with raw-ret
               (field-types
                 (let loop ((i 0) (acc '()))
                   (if (>= i num-fields)
                       (reverse acc)
                       (let* ((original-ty (cdr (list-ref offsets i))))
                         (if (effect.index-in-list? i value-indices)
                             (loop (+ i 1) (cons raw-ret acc))
                             (loop (+ i 1) (cons original-ty acc))))))))
          ;; Build product from substituted field types
          (type.product field-types)))))

;; ── Declarer / Lowerer ──

;; pass: core-declarer |middle.fn|
;; writes: compiler-state.comptime-fns (if comptime flag set)
;; calls: validate-effects!, core.lower-type
(define-pass (core-declarer |middle.fn| item)
  (let* ((payload (middle.payload item)) (body (optional.value (record.get (middle.payload item) '|body|))))
    (if (optional.none? body) unit
        (let* ((name (optional.value (record.get payload '|name|)))
               (params (optional.value (record.get payload '|params|)))
               (effects-opt (record.get payload '|effects|))
               ;; Validate effects before lowering
               (_ (validate-effects! effects-opt))
               ;; Register comptime function
               (_ (let ((comptime-flag (record.get payload '|comptime|)))
                    (if (and (optional.some? comptime-flag) (optional.value comptime-flag))
                        (comptime-fns.register! name)
                        unit)))
               (effects (optional.value effects-opt))
               (raw-ret (core.lower-type (optional.value (record.get payload '|return|))))
               ;; Phase 4: any declared effect wraps return in a product
               ;; Multi-effect: use merged layout; single effect: use direct lookup
               (ret (if (fn.has-declared-effects? effects-opt)
                        (let* ((eff-list (optional.value effects-opt)))
                          (if (> (length eff-list) 1)
                              ;; Multi-effect: build merged product
                              (let* ((eff-names (map fn.effect-name eff-list))
                                     (merged (effect.merged-info eff-names raw-ret)))
                                (if merged
                                    (type.product (car merged))
                                    (error "merged-info returned #f for multi-effect")))
                              ;; Single effect: use existing lookup
                              (let* ((eff-name (fn.first-effect-name effects-opt)))
                                (fn.make-effect-product-name effects-opt raw-ret eff-name))))
                        raw-ret))
               ;; C only knows bits/addr/void — structs become addr
               (c-ret (if (struct-type? ret) (type.addr) ret))
               (param-types (core.lower-param-types params (list)))
               ;; Convert struct params to addr for C
               (c-params (map (lambda (p) (if (struct-type? p) (type.addr) p))
                              param-types)))
          ;; Store real return type in Scheme table (avoids C-side type storage)
          (fn-return-type! name ret)
          (core.begin-function! name c-params c-ret)
          ;; Name mangling: pub fn gets C-level link_name (e.g., add → io_add)
          ;; Internal name stays unmangled for intra-module lookups.
          (let* ((public (record.get payload '|public|))
                 (is-public (and (optional.some? public) (optional.value public))))
            (if is-public
                (core.set-function-link-name! name
                  (string-append (core.module-prefix) "_"
                                 (symbol->string name)))
                unit))))))

(define-pass (core-declarer |middle.foreign-fn| item)
  (let* ((payload (middle.payload item)) (name (optional.value (record.get payload '|name|)))
         (attrs (optional.value (record.get payload '|attrs|)))
         (params (optional.value (record.get payload '|params|)))
         (ret (core.lower-type (optional.value (record.get payload '|return|))))
         (param-types (core.lower-param-types params (list))) (link-name (fn.foreign-link-name attrs name)))
    (if (fn.cfg-enabled? attrs) (host.new-extern name link-name param-types ret) unit)))

(define-pass (core-lowerer |middle.fn| item)
  (let* ((payload (middle.payload item)) (name (optional.value (record.get payload '|name|)))
         (params (optional.value (record.get payload '|params|)))
         (raw-ret-ty (core.lower-type (optional.value (record.get payload '|return|))))
         (effects (optional.value (record.get payload '|effects|)))
         ;; Phase 4: any declared effect wraps return in a product
         ;; Multi-effect: use merged layout; single effect: use direct lookup
         (ret-ty (if (fn.has-declared-effects? (record.get payload '|effects|))
                     (let* ((eff-list (optional.value (record.get payload '|effects|))))
                       (if (> (length eff-list) 1)
                           (let* ((eff-names (map fn.effect-name eff-list))
                                  (merged (effect.merged-info eff-names raw-ret-ty)))
                             (if merged
                                 (type.product (car merged))
                                 (error "merged-info returned #f in lowerer")))
                           (let* ((eff-name (fn.first-effect-name (record.get payload '|effects|))))
                             (fn.make-effect-product-name (record.get payload '|effects|) raw-ret-ty eff-name))))
                     raw-ret-ty))
         (body (optional.value (record.get payload '|body|))))
    (if (optional.none? body) unit
        (let* ((function (core.function-by-name name)) (block (core.append-block! function))
               (locals (core.bind-params function params 0 (list)))
               (body-payload (middle.payload (optional.value body))))
          ;; Phase 2: clear effect tracker before lowering this fn
          (propagate.clear!)
          (core.lower-stmts block (optional.value (record.get body-payload '|items|)) ret-ty locals)
          ;; Phase 2: register and validate effects
          (let* ((collected (propagate.collected-effects))
                 (declared-names (if effects (propagate.extract-names (optional.value effects)) '()))
                 ;; Register this fn's effects in the global table for callers
                 (_ (propagate.register-fn-effects! name declared-names)))
            ;; Validate that all collected effects are declared
            (propagate.validate-collected! collected declared-names name))))))

(define-pass (core-type-lowerer |middle.ty.fn| ty) (type.unsupported (middle.kind ty)))

;; ═══════════════════════════════════════════════════════════════════════════
;; Compile-time constant table is in compiler-state.scm.
;; Use const-table.register! and const-table.lookup.
;;
;; Expression evaluation for top-level let bindings:
;;   lowered via core.lower-expr → eval'd via core.eval-value!
;; This replaces the old const.eval-expr hand-written pattern matcher.
;; ═══════════════════════════════════════════════════════════════════════════

(define-pass (core-declarer |middle.let-binding| item)
  ;; pass: core-declarer |middle.let-binding|
  ;; reads: none (no-op — value computed during lowering)
  ;; Register the name as a known binding but don't create IR sub.
  ;; The actual value is computed during lowering.
  unit)

;; pass: core-lowerer |middle.let-binding|
;; reads: compiler-state.const-table (via register)
;; calls: core.begin-function!, core.function-by-name, core.append-block!,
;;        core.lower-expr (pipeline/lower.scm), core.eval-value! (builder_ffi.c)
;; Lower the expression to L1 IR, then eval it at compile time.
;; Uses the same lowering path as function bodies — no separate evaluator.
(define-pass (core-lowerer |middle.let-binding| item)
  (let* ((payload (middle.payload item))
         (name (optional.value (record.get payload '|name|)))
         (value-expr (optional.value (record.get payload '|value|))))
    ;; Create a temporary sub to hold the lowered expression
    (let* ((tmp-name (string->symbol
                       (string-append "$comptime_let_"
                                      (symbol->string name))))
           (_ (core.begin-function! tmp-name (list) (core.make-bits 32)))
           (tmp-sub (core.function-by-name tmp-name))
           (block (core.append-block! tmp-sub)))
      ;; Lower the expression into the temp block
      (let* ((ir-val (core.lower-expr block value-expr (core.make-bits 32) (list)))
             (result (core.eval-value! ir-val)))
        (if (and result (not (eq? result #f)))
            ;; Store in compile-time constant table
            (const-table.register! name (core.make-bits 32) result)
            ;; NOTE: unsupported expressions reach here.
            ;; Will be replaced by full L1 interpreter eval when needed.
            (error (string-append "unsupported let binding expression for: "
                                   (symbol->string name))))))))
