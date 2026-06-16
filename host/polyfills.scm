;; ============================================================================
;; host/polyfills.scm — 编译器 polyfill / 宿主抽象层 (Layer A)
;;
;; 此文件从 compiler/helpers.c 的 native_inject_all_polyfills() 中提取。
;; 所有定义均为纯 Scheme (R7RS + 少量 syntax-rules)，可被任何 R7RS
;; 运行时加载，不依赖 Chibi 特有的 C FFI。
;;
;; 加载顺序与原始 C 注入顺序完全一致，以保证依赖关系正确。
;; ============================================================================

;; ═══ 1. Import R7RS base ═══
;; Import is handled by the host runtime (helpers.c does it before loading
;; this file, standalone runtimes should uncomment the line below).
;; (import (scheme base) (scheme cxr) (scheme load))

;; ═══ 2. Basic polyfills ═══
(define unit #f)
(define (meta-source x) #f)
(define *error-count* 0)

;; ═══ 3. define-pass macro system ═══
(define __lain-passes '())

(define (define-pass* stage kind body)
  (set! __lain-passes
    (cons (cons stage (cons kind (cons body '()))) __lain-passes)))

(define-syntax define-pass
  (syntax-rules ()
    ((_ (stage kind arg ...) body ...)
     (define-pass* 'stage 'kind (lambda (arg ...) body ...)))))

;; ═══ 4. optional.* polyfills ═══
(define (optional.none) #f)
(define (optional.some v) v)
(define (optional.value v) v)
(define (optional.some? v) (if v #t #f))
(define (optional.none? v) (not v))

;; ═══ 5. list.* polyfills ═══
(define (list.empty? lst) (null? lst))
(define (list.first lst) (car lst))
(define (list.rest lst) (cdr lst))
(define (list.cons item lst) (cons item lst))
(define (list.reverse lst) (reverse lst))

;; ═══ 6. symbol=? polyfill ═══
(define (symbol=? a b) (equal? a b))

;; ═══ 7. record.* polyfills ═══
(define (record kind . fields) (cons kind fields))
(define (record.field k v) (cons k v))
(define (record.get payload name)
  (let loop ((fields (cdr payload)))
    (if (null? fields) #f
        (let ((field (car fields)))
          (if (equal? (car field) name)
              (cdr field)
              (loop (cdr fields)))))))

;; ═══ 8. Pre-declare temp counters (Scheme-side) ═══
(define *syntax-temp-counter* 0)
(define *effect-temp-counter* 0)

(define (syntax.temp)
  (set! *syntax-temp-counter* (+ *syntax-temp-counter* 1))
  (string->symbol (string-append "__syntax_temp_"
                                 (number->string *syntax-temp-counter*))))

(define (effects.temp)
  (set! *effect-temp-counter* (+ *effect-temp-counter* 1))
  (string->symbol (string-append "__effect_temp_"
                                 (number->string *effect-temp-counter*))))

;; ═══ 9. raw.* polyfills ═══
(define (raw.node! kind payload) (vector 'raw-node kind payload))
(define (raw.kind node) (vector-ref node 1))
(define (raw.payload node) (vector-ref node 2))

;; ═══ 10. middle.* polyfills ═══
(define (middle.node! kind payload) (vector 'middle-node kind payload))
(define (middle.kind node) (vector-ref node 1))
(define (middle.payload node) (vector-ref node 2))

;; ═══ 11. decl.* polyfills ═══
(define *lain-declarations* '())

(define (decl.define! kind name node)
  (set! *lain-declarations* (cons (list name kind node) *lain-declarations*)))

(define (decl.name decl) (car decl))
(define (decl.payload decl) (caddr decl))

;; ═══ 12. type.* wrappers (→ ir-api Layer 1) ═══
;; These are thin aliases — meta code uses type.*, which resolves through
;; ir-api.scm to the underlying core.* FFI functions.
(load "host/ir-api.scm")
(load "host/host-api.scm")

(define type.unit    ir.type.unit)
(define type.bits    ir.type.bits)
(define type.addr    ir.type.addr)
(define type.floats  ir.type.floats)
(define type.simd    ir.type.simd)
(define type.never   ir.type.never)
(define type.unit?   ir.type.unit?)
(define type.eq?     ir.type.equal?)

(define (type.raw-ptr . args)        (ir.type.addr))
(define (type.registered name . args) (ir.type.lookup name))
(define (type.unsupported . args)     (ir.type.bits 32))  ;; FIXME: → host.error after migration

;; ── Backward-compat stubs (meta layer still uses these) ──
;; These will be removed when meta layer migration is complete.
(define type.float?              (lambda (ty) #f))
(define type.fn                  (lambda (params ret) (ir.type.addr)))
(define type.array               (lambda (ty size) (ir.type.addr)))
(define type.product             (lambda (types) (core.make-product-type! types)))
(define type.product-field-types (lambda (product) '()))
(define type.product-field-type  (lambda (product field-name)
  (core.struct-field-type-from-type product field-name)))
(define type.product-name        (lambda (ty) #f))
(define string-byte-len          string-length)

;; ═══ 13. core.* Scheme polyfills (non-FFI) ═══
(define (core.invoke-intrinsic! name) #f)
(define (core.empty-effects) 'empty-effects)
(define (registry.operator-registered? op) #t)
(define (u64.add1 n) (+ n 1))
(define (core.effect! name args) (list 'effect name args))
(define (core.effect-row! ids) 'effect-row)
(define (core.bind-generic! name) (if #f #f))
(define (core.clear-generics!) (if #f #f))
(define (core.function-effects fn) (quote empty-effects))
(define (core.begin-function-with-effects! name params effects ret)
  (core.begin-function! name params ret))
(define (core.const-zero! block ty)
  (core.const-bits! block ty 0))
(define (core.unsupported-expr kind) (core.make-bits 32))
(define (diag.raise! . args) #f)
(define (core.declare-enum-name! name variants) unit)

;; ═══ 14. effects.* polyfills ═══
(define (effects.find-throws-arg eff) #f)
(define effects.row-effect-count (lambda (r) 0))
(define effects.row-effect-at (lambda (r i) '()))
(define effects.effect-name (lambda (e) 'unknown))
(define effects.effect-args (lambda (e) '()))

;; ═══ 14.5 Host function polyfills (no-ops in bootstrap) ═══
(define (register-type-constructor! name arity repr) unit)
(define (register-string-literal-type! name) unit)
(define (register-memory-ordering-type! name) unit)
(define (register-array-type! name) unit)
(define (register-condition-type! name) unit)
(define (register-raw-pointer-type! mut name) unit)
(define (register-raw-pointer-index-type! name) unit)
(define (register-constraint! name pred) unit)
(define (register-interface-rule! name rule) unit)
(define (register-operator! symbol name) unit)
(define (register-effect-constructor! name arity repr) unit)
(define (register-expression-macro! name handler) unit)
(define (register-raw-pointer-effect! kind name) unit)

;; ═══ 16. Pre-declare variables (Chibi requires these before set!) ═══
;; syntax
(define syntax.parse-path-tail #f)
(define syntax.parse-path #f)
(define syntax.parse-attr-value #f)
(define syntax.parse-attr-arg #f)
(define syntax.parse-attr-args-tail #f)
(define syntax.parse-attr-args #f)
(define syntax.parse-attrs #f)
(define syntax.parse-generic-tail #f)
(define syntax.parse-generic-params #f)
(define syntax.parse-where-tail #f)
(define syntax.parse-where #f)
(define syntax.parse-type #f)
(define syntax.parse-type-args #f)
(define syntax.parse-type-list-tail #f)
(define syntax.parse-type-list #f)
(define syntax.parse-params-tail #f)
(define syntax.parse-params #f)
(define syntax.parse-empty-params #f)
(define syntax.parse-effect-tail #f)
(define syntax.parse-effect-name #f)
(define syntax.parse-effect-set #f)
(define syntax.parse-optional-effects #f)
(define syntax.parse-expr-args-tail #f)
(define syntax.parse-expr-args #f)
(define syntax.parse-mul-op #f)
(define syntax.parse-add-op #f)
(define syntax.parse-compare-op #f)
(define syntax.parse-mul-tail #f)
(define syntax.parse-add-tail #f)
(define syntax.parse-compare-tail #f)
(define syntax.parse-unary-expr #f)
(define syntax.parse-mul-expr #f)
(define syntax.parse-add-expr #f)
(define syntax.parse-compare-expr #f)
(define syntax.parse-call-or-path #f)
(define syntax.parse-postfix-tail #f)
(define syntax.parse-inline-handler-operation #f)
(define syntax.parse-inline-handler-operations #f)
(define syntax.parse-builtin-expr-after-at #f)
(define syntax.parse-if-expr-after-if #f)
(define syntax.parse-expr-atom #f)
(define syntax.parse-expr #f)
(define syntax.parse-let-stmt #f)
(define syntax.parse-block-items #f)
(define syntax.parse-block #f)
(define syntax.parse-optional-fn-body #f)

;; core
(define core.lower-type #f)
(define core.lower-types #f)
(define core.lower-param-types #f)
(define core.bind-params #f)
(define core.lower-expr #f)
(define core.infer-expr-type #f)
(define core.lower-stmt #f)
(define core.lower-stmts #f)
(define core.lower-if-non-tail-core #f)
(define core.lower-if-branch-body #f)

;; effects
(define effects.handler-entry-type #f)
(define effects.allocate-entry! #f)
(define effects.push-handler! #f)
(define effects.pop-handler! #f)
