;; ===========================================================================
;; std/meta/struct/registry.scm — Pure Scheme Struct Registry
;;
;; Moves struct type layout computation (field offsets, alignment, total size)
;; from C (helpers.c §8) into Scheme.  The C side only stores pre-computed
;; numbers via core.declare-struct-layout! — no computation, no alignment
;; logic, no type inspection.
;; ===========================================================================

;; ── Global registry ──

(define *struct-registry* (list))

;; ── Internal: compute field layout ──

;; Returns (values total-size layout-list) where layout-list = ((name type offset) ...)
(define (struct--compute-layout fields offset acc)
  (if (null? fields)
      (values offset (reverse acc))
      (let* ((field (car fields))
             (name (car field))
             (ty (cdr field))
             (size (core.type-size-in-bytes! ty))
             ;; 64-bit alignment
             (aligned (if (and (= size 8) (not (zero? (modulo offset 8))))
                          (+ offset (- 8 (modulo offset 8)))
                          offset)))
        (struct--compute-layout (cdr fields)
                                (+ aligned size)
                                (cons (list name ty aligned) acc)))))

;; ── Public API ──

;; Register a struct: name (symbol), fields ((name . type-cpointer) ...)
;; Returns the L1Type cpointer from C.
(define (struct-register! name lowered-fields)
  (let*-values (((total-size layout) (struct--compute-layout lowered-fields 0 '())))
    ;; Register with C (thin storage, no computation)
    (core.declare-struct-layout! name total-size layout)
    ;; Also cache locally for query functions
    (set! *struct-registry*
      (cons (cons name (cons total-size layout)) *struct-registry*))
    ;; Return the type cpointer for compatibility
    (core.struct-type name)))

;; Look up struct info: (total-size . layout)
(define (struct-lookup name)
  (let loop ((reg *struct-registry*))
    (if (null? reg)
        (error (string-append "struct not found: " (symbol->string name)))
        (let ((entry (car reg)))
          (if (eq? (car entry) name)
              entry
              (loop (cdr reg)))))))

;; Total size in bytes
(define (struct-total-size name)
  (cadr (struct-lookup name)))

;; Field byte offset
(define (struct-field-offset name field-name)
  (let* ((entry (struct-lookup name))
         (layout (cddr entry)))
    (let loop ((fields layout))
      (if (null? fields)
          (error (string-append "field not found: " (symbol->string field-name)
                                " in struct " (symbol->string name)))
          (let ((f (car fields)))
            (if (eq? (car f) field-name)
                (caddr f)  ;; offset
                (loop (cdr fields))))))))

;; Field type (cpointer)
(define (struct-field-type name field-name)
  (let* ((entry (struct-lookup name))
         (layout (cddr entry)))
    (let loop ((fields layout))
      (if (null? fields)
          (error (string-append "field not found: " (symbol->string field-name)
                                " in struct " (symbol->string name)))
          (let ((f (car fields)))
            (if (eq? (car f) field-name)
                (cadr f)  ;; type cpointer
                (loop (cdr fields))))))))
