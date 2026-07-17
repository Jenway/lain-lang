;; ===========================================================================
;; std/meta/struct/registry.scm — Pure Scheme Struct Registry
;;
;; Struct types are pure Scheme records: (struct-type . <name>)
;; No L1Type*/TY_PRODUCT — C only handles bits/addr/void atoms.
;; Layout computation, field lookup, type identity: all in Scheme.
;; ===========================================================================

;; ── Global registries ──

(define *struct-registry* (list))     ;; ((name . (total-size layout)) ...)
(define *struct-identities* (list))   ;; ((name . canonical-identity) ...)
(define *fn-return-types* (list))     ;; ((name . return-type) ...) — avoids C-side type storage

(define (struct-registry.reset!)
  (set! *struct-registry* (list))
  (set! *struct-identities* (list))
  (set! *fn-return-types* (list)))

;; ── Struct type constructor / predicate ──

;; Returns a struct-type record: (struct-type . <name>)
(define (struct-type name)
  (cons 'struct-type name))

(define (struct-type? ty)
  (and (pair? ty) (eq? (car ty) 'struct-type)))

(define (struct-type-name ty)
  (if (struct-type? ty) (cdr ty)
      ;; Unwrap ref types
      (let* ((kind (raw.kind ty)))
        (if (or (symbol=? kind '|types.ref|)
                (symbol=? kind '|types.raw-ptr|))
            (let* ((payload (raw.payload ty))
                   (inner (optional.value (record.get payload '|inner|))))
              (struct-type-name inner))
            (error "not a struct type")))))

;; ── Function return type table (keeps struct identity out of C) ──

(define (fn-return-type! name ret-ty)
  (set! *fn-return-types* (cons (cons name ret-ty) *fn-return-types*)))

(define (fn-return-type-lookup name)
  (let ((loop #f))
  (set! loop (lambda (t)
    (if (null? t) #f
        (if (eq? (caar t) name) (cdar t)
            (loop (cdr t))))))
  (loop *fn-return-types*)))

;; ── Internal: compute field layout ──

;; Returns (values total-size layout-list) where layout-list = ((name type-cptr offset) ...)
(define (struct--compute-layout fields offset acc)
  (if (null? fields)
      (values offset (reverse acc))
      (let* ((field (car fields))
             (name (car field))
             (ty (cdr field))
             ;; If field type is a struct-type record, treat as addr-sized (8 bytes)
             ;; Otherwise it's an L1Type cpointer — ask C for its size
             (size (if (struct-type? ty) 8 (core.type-size-in-bytes! ty)))
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
    (set! *struct-identities*
      (cons (cons name
                  (string-append (core.module-prefix) "::"
                                 (symbol->string name)))
            *struct-identities*))
    total-size))

;; Register a layout recovered from a Meta-owned module interface.  Offsets
;; and nominal identity are producer decisions and therefore must not be
;; recomputed by the importing host bridge.
(define (struct-register-imported! name identity total-size layout)
  (set! *struct-registry*
    (cons (cons name (cons total-size layout)) *struct-registry*))
  (set! *struct-identities*
    (cons (cons name identity) *struct-identities*))
  total-size)

(define (struct-identity name)
  (let ((loop #f))
    (set! loop (lambda (entries)
      (if (null? entries)
          (string-append (core.module-prefix) "::" (symbol->string name))
          (if (eq? (caar entries) name)
              (cdar entries)
              (loop (cdr entries))))))
    (loop *struct-identities*)))

(define (struct-alignment name)
  (let* ((entry (struct-lookup name))
         (layout (cddr entry)))
    (let ((loop #f))
      (set! loop (lambda (fields best)
        (if (null? fields)
            best
            (let* ((ty (cadar fields))
                   (size (if (struct-type? ty)
                             8
                             (core.type-size-in-bytes! ty)))
                   (align (if (> size 8) 8 size)))
              (loop (cdr fields) (if (> align best) align best))))))
      (loop layout 1))))

;; Stable interface spelling for normalized types.  The stage-0 bridge only
;; needs a symbol; applications use the same concrete-name convention as the
;; struct registry, while pointer-like forms have the physical `addr` ABI.
(define (interface.middle-type-name ty)
  (let* ((kind (middle.kind ty))
         (payload (middle.payload ty)))
    (cond
      ((symbol=? kind '|types.path|)
       (optional.value (record.get payload '|name|)))
      ((symbol=? kind '|types.unit|) '|unit|)
      ((symbol=? kind '|types.app|)
       (let* ((name (optional.value (record.get payload '|name|)))
              (args (optional.value (record.get payload '|args|))))
         (interface.concrete-type-name name args)))
      ((or (symbol=? kind '|types.raw-ptr|)
           (symbol=? kind '|types.ref|)
           (symbol=? kind '|types.slice|)
           (symbol=? kind '|types.fn|)) '|addr|)
      (else '|addr|))))

(define (interface.concrete-type-name name args)
  (if (null? args)
      name
      (interface.concrete-type-name-loop args (symbol->string name))))

(define (interface.concrete-type-name-loop args acc)
  (if (null? args)
      (string->symbol acc)
      (interface.concrete-type-name-loop
        (cdr args)
        (string-append acc "_"
          (symbol->string (interface.middle-type-name (car args)))))))

;; Look up struct info: (name total-size . layout)
(define (struct-lookup name)
  (let ((loop #f))
  (set! loop (lambda (reg)
    (if (null? reg)
        (error (string-append "struct not found: " (symbol->string name)))
        (let ((entry (car reg)))
          (if (eq? (car entry) name)
              entry
              (loop (cdr reg)))))))
  (loop *struct-registry*)))

;; Check if a struct name is registered (non-fatal)
(define (struct-registered? name)
  (let ((loop #f))
  (set! loop (lambda (reg)
    (if (null? reg)
        #f
        (if (eq? (caar reg) name)
            #t
            (loop (cdr reg))))))
  (loop *struct-registry*)))

;; Total size in bytes
(define (struct-total-size name)
  (cadr (struct-lookup name)))

;; Field byte offset
(define (struct-field-offset name field-name)
  (let* ((entry (struct-lookup name))
         (layout (cddr entry)))
    (let ((loop #f))
  (set! loop (lambda (fields)
      (if (null? fields)
          (error (string-append "field not found: " (symbol->string field-name)
                                " in struct " (symbol->string name)))
          (let ((f (car fields)))
            (if (eq? (car f) field-name)
                (caddr f)  ;; offset
                (loop (cdr fields)))))))
  (loop layout))))

;; Field type — returns L1 atom cpointer (bits/addr/void)
(define (struct-field-type name field-name)
  (let* ((entry (struct-lookup name))
         (layout (cddr entry)))
    (let ((loop #f))
  (set! loop (lambda (fields)
      (if (null? fields)
          (error (string-append "field not found: " (symbol->string field-name)
                                " in struct " (symbol->string name)))
          (let ((f (car fields)))
            (if (eq? (car f) field-name)
                (cadr f)  ;; type cpointer (L1 atom, not struct)
                (loop (cdr fields)))))))
  (loop layout))))

;; ── Generic struct instantiation ──

;; Create a concrete struct type from a generic struct definition.
;; e.g., (struct-instantiate 'DynArray (list (struct-type 'RawToken)))
;; → registers DynArray_RawToken as a concrete struct, returns (struct-type . DynArray_RawToken)
;;
;; For now, all instantiations of a generic struct share the same physical layout
;; (since all type parameters become addr in L1). The concrete name is formed by
;; appending the lowered type names with underscores.
(define (struct-instantiate name args)
  (let* ((concrete-name (struct--concrete-name name args)))
    (if (struct-registered? concrete-name)
        (struct-type concrete-name)
        (struct--instantiate-impl name concrete-name args))))

;; Build a concrete name: DynArray + (RawToken) → DynArray_RawToken
(define (struct--concrete-name name args)
  (if (null? args)
      name
      (let ((loop #f))
  (set! loop (lambda (remaining acc)
        (if (null? remaining)
            (string->symbol acc)
            (let* ((arg (car remaining))
                   (arg-name (struct--arg-name arg))
                   (new-acc (string-append acc "_" arg-name)))
              (loop (cdr remaining) new-acc)))))
  (loop args (symbol->string name)))))

;; Get a printable name for an already-lowered type
(define (struct--arg-name ty)
  (if (struct-type? ty)
      (symbol->string (struct-type-name ty))
      "unknown"))

;; Clone a generic struct's layout, replacing generic params with concrete types.
;; Since L1 only sees bits/addr/void, all struct-type args become addr.
(define (struct--instantiate-impl generic-name concrete-name args)
  (let* ((generic-entry (struct-lookup generic-name))
         (total-size (cadr generic-entry))
         (generic-layout (cddr generic-entry))
         ;; Clone layout: replace any struct-type fields with addr for L1
         (concrete-fields
          (map (lambda (field-entry)
                 (let* ((fname (car field-entry))
                        (fty (cadr field-entry))
                        (offs (caddr field-entry))
                        ;; If field type is a struct-type, replace with addr for L1
                        (concrete-ty (if (struct-type? fty) (type.addr) fty)))
                   (list fname concrete-ty offs)))
               generic-layout)))
    ;; Register the concrete struct
    (set! *struct-registry*
      (cons (cons concrete-name (cons total-size concrete-fields))
            *struct-registry*))
    (struct-type concrete-name)))

;; Returns (total-size . ((offset . type) ...))
;; field-types: list of L1 atom cpointers
(define (product-layout field-types)
  (let ((n (length field-types)))
    (if (zero? n)
        (cons 0 '())
        (let ((loop #f))
  (set! loop (lambda (tys offset acc)
          (if (null? tys)
              (cons offset (reverse acc))
              (let* ((ty (car tys))
                     (size (core.type-size-in-bytes! ty))
                     ;; Natural alignment: align to field's own size (capped at 8)
                     (align (if (> size 8) 8 size))
                     (aligned (if (zero? (modulo offset align))
                                  offset
                                  (+ offset (- align (modulo offset align))))))
                (loop (cdr tys)
                      (+ aligned size)
                      (cons (cons aligned ty) acc))))))
  (loop field-types 0 '())))))
