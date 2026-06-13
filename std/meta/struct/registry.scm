;; ===========================================================================
;; std/meta/struct/registry.scm — Pure Scheme Struct Registry
;;
;; Struct types are pure Scheme records: (struct-type . <name>)
;; No L1Type*/TY_PRODUCT — C only handles bits/addr/void atoms.
;; Layout computation, field lookup, type identity: all in Scheme.
;; ===========================================================================

;; ── Global registries ──

(define *struct-registry* (list))     ;; ((name . (total-size layout)) ...)
(define *fn-return-types* (list))     ;; ((name . return-type) ...) — avoids C-side type storage

;; ── Struct type constructor / predicate ──

;; Returns a struct-type record: (struct-type . <name>)
(define (struct-type name)
  (cons 'struct-type name))

(define (struct-type? ty)
  (and (pair? ty) (eq? (car ty) 'struct-type)))

(define (struct-type-name ty)
  (if (struct-type? ty) (cdr ty)
      (error "not a struct type")))

;; ── Function return type table (keeps struct identity out of C) ──

(define (fn-return-type! name ret-ty)
  (set! *fn-return-types* (cons (cons name ret-ty) *fn-return-types*)))

(define (fn-return-type-lookup name)
  (let loop ((t *fn-return-types*))
    (if (null? t) #f
        (if (eq? (caar t) name) (cdar t)
            (loop (cdr t))))))

;; ── Internal: compute field layout ──

;; Returns (values total-size layout-list) where layout-list = ((name type-cptr offset) ...)
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
;; Returns total-size. NO FFI call — everything in Scheme.
(define (struct-register! name lowered-fields)
  (let*-values (((total-size layout) (struct--compute-layout lowered-fields 0 '())))
    (set! *struct-registry*
      (cons (cons name (cons total-size layout)) *struct-registry*))
    total-size))

;; Look up struct info: (name total-size . layout)
(define (struct-lookup name)
  (let loop ((reg *struct-registry*))
    (if (null? reg)
        (error (string-append "struct not found: " (symbol->string name)))
        (let ((entry (car reg)))
          (if (eq? (car entry) name)
              entry
              (loop (cdr reg)))))))

;; Check if a struct name is registered (non-fatal)
(define (struct-registered? name)
  (let loop ((reg *struct-registry*))
    (if (null? reg)
        #f
        (if (eq? (caar reg) name)
            #t
            (loop (cdr reg))))))

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

;; Field type — returns L1 atom cpointer (bits/addr/void)
(define (struct-field-type name field-name)
  (let* ((entry (struct-lookup name))
         (layout (cddr entry)))
    (let loop ((fields layout))
      (if (null? fields)
          (error (string-append "field not found: " (symbol->string field-name)
                                " in struct " (symbol->string name)))
          (let ((f (car fields)))
            (if (eq? (car f) field-name)
                (cadr f)  ;; type cpointer (L1 atom, not struct)
                (loop (cdr fields))))))))

;; ── Anonymous product layout (no registry) ──

;; Returns (total-size . ((offset . type) ...))
;; field-types: list of L1 atom cpointers
(define (product-layout field-types)
  (let ((n (length field-types)))
    (if (zero? n)
        (cons 0 '())
        (let loop ((tys field-types) (offset 0) (acc '()))
          (if (null? tys)
              (cons offset (reverse acc))
              (let* ((ty (car tys))
                     (size (core.type-size-in-bytes! ty))
                     ;; 64-bit alignment
                     (aligned (if (and (= size 8) (not (zero? (modulo offset 8))))
                                  (+ offset (- 8 (modulo offset 8)))
                                  offset)))
                (loop (cdr tys)
                      (+ aligned size)
                      (cons (cons aligned ty) acc))))))))
