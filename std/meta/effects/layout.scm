(meta-source "effects/layout")

;; ═══════════════════════════════════════════════════════════
;; Effect Product Layout Registry
;; ═══════════════════════════════════════════════════════════
;;
;; Every effect operation produces a "product" — a multi-field
;; aggregate returned (or yielded) to the handler. This registry
;; maps (effect-name operation-name) → layout description so
;; lowering passes (perform, ?, handle) share the same layout.
;;
;; Layout description:
;;   (layout-info total-size field-descs flag-index)
;;   field-descs = ((offset . type) ...)
;;   flag-index  = which field-descs element is the flag (0-based)
;;   arg-indices = which field-descs elements hold op args (0-based list)
;;
;; For Throws::throw:
;;   product = {flag:i8, value:i32, error:i32}
;;   flag-index = 0, arg-indices = (2)
;;
;; For Suspend::suspend:
;;   product = {flag:i8, waker:addr}
;;   flag-index = 0, arg-indices = (1)

;; ── Internal effect table ──
;; Maps 'effect-name → ((op-name layout-info arg-indices) ...)

(define *effect-layouts* (list))

;; ── Internal: manual assoc (R7RS assoc not in boot env) ──
(define (effect.assoc key alist)
  (if (null? alist)
      #f
      (let* ((pair (car alist)))
        (if (eq? (car pair) key)
            pair
            (effect.assoc key (cdr alist))))))

;; ── Register an effect operation layout ──
(define (effect.register-layout! effect-name op-name
                                 field-types
                                 flag-index
                                 arg-indices)
  ;; field-types: list of types (symbols or lowered types), e.g. (i8 i32 i32)
  ;; flag-index: which field is the flag
  ;; arg-indices: which fields hold operation arguments (ordered)
  (let* ((layout (product-layout field-types))
         (total-size (car layout))
         (offsets (cdr layout))
         (entry (list op-name total-size offsets flag-index arg-indices))
         (existing (effect.assoc effect-name *effect-layouts*)))
    (if existing
        (let* ((ops (cdr existing))
               ;; Manual filter: keep all ops except the one being replaced
               (updated-ops (let filter ((remaining ops) (acc '()))
                              (if (null? remaining)
                                  (reverse acc)
                                  (let ((e (car remaining)))
                                    (if (eq? (car e) op-name)
                                        (filter (cdr remaining) acc)
                                        (filter (cdr remaining) (cons e acc))))))))
          (set-cdr! existing (cons entry updated-ops)))
        (set! *effect-layouts*
              (cons (cons effect-name (list entry)) *effect-layouts*)))))

;; ── Query: get layout for an effect (defaults to first registered op) ──
;; Returns (total-size offsets flag-index arg-indices) or #f
(define (effect.lookup-layout effect-name op-name)
  (let* ((effect-entry (effect.assoc effect-name *effect-layouts*)))
    (if (not effect-entry)
        #f
        (let* ((ops (cdr effect-entry))
               ;; If op-name is #f, use the first registered operation
               (target-op (if op-name op-name (caar ops)))
               (op-entry (effect.assoc target-op ops)))
          (if (not op-entry)
              #f
              (let* ((total-size (cadr op-entry))
                     (offsets    (caddr op-entry))
                     (flag-index (cadddr op-entry))
                     (arg-indices (car (cddddr op-entry))))
                (list total-size offsets flag-index arg-indices)))))))

;; ── Pre-register built-in effects ──

;; Throws::throw(error: E) -> !
;; Product: {flag: u8, value: i32, error: i32}
(effect.register-layout! '|Throws| '|throw|
  (list (ir.type.bits 8) (ir.type.bits 32) (ir.type.bits 32))
  0    ;; flag-index: field 0 is the flag
  '(2)) ;; arg-indices: field 2 holds the error argument

;; Suspend::suspend(waker: Waker) — Waker is addr
;; Product: {flag: u8, waker: addr}
;; Also register Suspend as a valid effect constructor (no-arg ctor)
(register-effect-ctor! '|Suspend| 0)
(effect.register-layout! '|Suspend| '|suspend|
  (list (ir.type.bits 8) (ir.type.addr))
  0
  '(1))

;; Spawn::spawn(task: fn() -> () ! {Suspend})
;; Product: {flag: u8, task_fn: addr}
(register-effect-ctor! '|Spawn| 0)
(effect.register-layout! '|Spawn| '|spawn|
  (list (ir.type.bits 8) (ir.type.addr))
  0
  '(1))

;; ── Helper: build a product from field values ──
(define (effect.build-product block field-types field-values)
  "Given parallel lists of field types and lowered L1Expr values,
   compute the product-layout and emit an ir.type.aggregate-layout."
  (let* ((layout (product-layout field-types))
         (total-size (car layout))
         (offsets (cdr layout))
         ;; offsets = ((offset . ty) ...)
         ;; We need ((offset . value) ...)
         (pairs (map (lambda (off pair val)
                       (cons (car off) val))
                     offsets field-values)))
    (ir.type.aggregate-layout block total-size pairs)))
