(meta-source "effects/merge")

;; ═══════════════════════════════════════════════════════════
;; Multi-Effect Layout Merge
;; ═══════════════════════════════════════════════════════════
;;
;; When a function has multiple declared effects (e.g.,
;; ! {Throws<E>, Suspend}), the return type is a merged product:
;;
;;   {flag: i8, discriminator: i8, shared_value: T,
;;    effect1_arg_fields..., effect2_arg_fields...}
;;
;; Each effect contributes its non-flag, non-value fields.
;; The discriminator tells the handler which effect triggered.

;; ── Internal: merged layout cache ──
;; Maps effect-name-list → merged info
(define *merged-info* (list))

;; ── effect.merged-info ──
;; Input:  list of effect-name symbols, raw-ret type
;; Output: #f if single effect (not needed), or:
;;         (field-types arg-indices disc-map)
;;   field-types = (u8 u8 T e1_arg1 e1_arg2 ... e2_arg1 ...)
;;   arg-indices = (3 4 7 ...)   ;; 0-based index into field-types
;;   disc-map    = ((name . disc-value) ...)
;;
;; This returns FIELD TYPES (not byte offsets) — caller uses
;; type.product to build the actual product type.

(define (effect.merged-info effect-names raw-ret)
  (if (or (null? effect-names) (null? (cdr effect-names)))
      #f  ;; single effect or none — no merge needed
      (let* ((cached (effect.assoc effect-names *merged-info*)))
        (if cached
            (cdr cached)
            (let* ((field-types (list (core.make-bits 8)    ;; flag
                                      (core.make-bits 8)    ;; discriminator
                                      raw-ret))           ;; shared value
                   (base-count 3)  ;; flag + disc + value = 3 prefix fields
                   ;; Process effects
                   (result
                     (let loop ((names effect-names)
                                (ftypes field-types)
                                (arg-idxs '())
                                (discs '())
                                (disc 0))
                       (if (null? names)
                           (list (reverse ftypes)
                                 (reverse arg-idxs)
                                 (reverse discs))
                           (let* ((eff-name (car names))
                                  (layout (effect.lookup-layout eff-name #f)))
                             (if (not layout)
                                 (loop (cdr names) ftypes arg-idxs
                                       (cons (cons eff-name disc) discs)
                                       (+ disc 1))
                                 (let* ((offsets (cadr layout))
                                        (flag-idx (caddr layout))
                                        (layout-arg-idxs (cadddr layout))
                                        (num-fields (length offsets))
                                        ;; Collect arg types from this effect
                                        (result2
                                          (let inner ((i 0)
                                                      (fts ftypes)
                                                      (ais arg-idxs))
                                            (if (>= i num-fields)
                                                (list fts ais)
                                                (if (or (= i flag-idx)
                                                        (not (effect.index-in-list?
                                                               i layout-arg-idxs)))
                                                    (inner (+ i 1) fts ais)
                                                    (let* ((field-type
                                                             (cdr (list-ref offsets i)))
                                                           (global-idx (length fts)))
                                                      (inner (+ i 1)
                                                             (cons field-type fts)
                                                             (cons global-idx ais))))))))
                                   (loop (cdr names)
                                         (car result2)   ;; updated ftypes
                                         (cadr result2)  ;; updated arg-idxs
                                         (cons (cons eff-name disc) discs)
                                         (+ disc 1)))))))))
              ;; Cache and return
              (set! *merged-info*
                    (cons (cons effect-names result) *merged-info*))
              result)))))

;; ── effect.merged-discriminator ──
;; Get discriminator value for an effect in merged info
(define (effect.merged-discriminator merged-info effect-name)
  (let* ((disc-map (caddr merged-info))
         (entry (effect.assoc effect-name disc-map)))
    (if entry (cdr entry) -1)))
