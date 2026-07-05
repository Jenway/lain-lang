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

(define (effect.merge-layout-args offsets flag-idx layout-arg-idxs ftypes arg-idxs)
  (let ((inner #f)
        (num-fields (length offsets)))
    (set! inner
      (lambda (i fts ais)
        (if (>= i num-fields)
            (list fts ais)
            (if (or (= i flag-idx)
                    (not (effect.index-in-list? i layout-arg-idxs)))
                (inner (+ i 1) fts ais)
                (let* ((field-type (cdr (list-ref offsets i)))
                       (global-idx (length fts)))
                  (inner (+ i 1)
                         (cons field-type fts)
                         (cons global-idx ais)))))))
    (inner 0 ftypes arg-idxs)))

(define (effect.merged-info effect-names raw-ret)
  (if (or (null? effect-names) (null? (cdr effect-names)))
      #f
      (let* ((cached (effect.assoc effect-names *merged-info*)))
        (if cached
            (cdr cached)
            (let ((field-types (list (core.make-bits 8)
                                     (core.make-bits 8)
                                     raw-ret))
                  (loop #f))
              (set! loop
                (lambda (names ftypes arg-idxs discs disc)
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
                                   (merged-args
                                    (effect.merge-layout-args
                                     offsets flag-idx layout-arg-idxs
                                     ftypes arg-idxs)))
                              (loop (cdr names)
                                    (car merged-args)
                                    (cadr merged-args)
                                    (cons (cons eff-name disc) discs)
                                    (+ disc 1))))))))
              (let ((result (loop effect-names field-types '() '() 0)))
                (set! *merged-info*
                      (cons (cons effect-names result) *merged-info*))
                result))))))

;; ── effect.merged-discriminator ──
;; Get discriminator value for an effect in merged info
(define (effect.merged-discriminator merged-info effect-name)
  (let* ((disc-map (caddr merged-info))
         (entry (effect.assoc effect-name disc-map)))
    (if entry (cdr entry) -1)))
