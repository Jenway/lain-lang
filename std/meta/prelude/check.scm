;; std/meta/prelude/check.scm — Cross-form compilation validation
(meta-source "prelude/check")

;; *all-declarations* persists across forms within the same pipeline run.
;; Unlike *lain-declarations* (cleared per-form by compile-group-to-core),
;; this accumulates ALL declarations so duplicates across forms are detected.
(define *all-declarations* '())

(define (decl.lookup name kind)
  (let loop ((decls *all-declarations*))
    (if (null? decls)
        #f
        (let* ((decl (car decls))
               (d-name (car decl))
               (d-kind (cadr decl)))
          (if (and (equal? d-name name) (equal? d-kind kind))
              decl
              (loop (cdr decls)))))))

(define (decl.define-dup-checked! kind name node)
  (let ((existing (decl.lookup name kind)))
    (if existing
        (error (string-append "duplicate " (symbol->string kind)
                              " declaration `" (symbol->string name) "'"))
        (begin
          (set! *all-declarations*
                (cons (list name kind node) *all-declarations*))
          (decl.define! kind name node)))))

;; ═══════════════════════════════════════════════════════════
;; Type & Effect Constructor Registries (Scheme-side)
;;
;; register-type-constructor! and register-effect-constructor!
;; are no-ops in the bootstrap C layer, so we maintain our own
;; registries for validation.
;; ═══════════════════════════════════════════════════════════

;; ── Type constructors: ((name . arity) ...)
(define *known-type-ctors* '())

;; Simple list membership test (not in bootstrap stdlib)
(define (list.member? lst item)
  (let loop ((remaining lst))
    (if (null? remaining)
        #f
        (if (eq? (car remaining) item)
            #t
            (loop (cdr remaining))))))

(define (register-type-ctor! name arity)
  (set! *known-type-ctors*
        (cons (cons name arity) *known-type-ctors*)))

(define (type-ctor-lookup name)
  (let loop ((ctors *known-type-ctors*))
    (if (null? ctors)
        #f
        (let ((entry (car ctors)))
          (if (eq? (car entry) name)
              entry
              (loop (cdr ctors)))))))

;; Returns #t if the name is a known type (builtin, struct, or enum)
(define (type-known? name)
  (or (type-ctor-lookup name)
      (struct-registered? name)))

;; ── Effect constructors: ((name . arity) ...)
(define *known-effect-ctors* '())

(define (register-effect-ctor! name arity)
  (set! *known-effect-ctors*
        (cons (cons name arity) *known-effect-ctors*)))

(define (effect-ctor-lookup name)
  (let loop ((ctors *known-effect-ctors*))
    (if (null? ctors)
        #f
        (let ((entry (car ctors)))
          (if (eq? (car entry) name)
              entry
              (loop (cdr ctors)))))))

;; Validate that a type path refers to a known type
;; Called during type lowering.  'name' is a symbol.
(define (validate-type-exists! name)
  (if (not (type-known? name))
      (error (string-append "unknown type `" (symbol->string name) "'"))))

;; Validate effect constructors in a function's effects list.
;; 'effects' is either optional.none or a list of (effect-name <name> <args>) raw nodes.
;; Returns #t on success, errors on failure.
(define (validate-effects! effects)
  (if (optional.none? effects)
      #t
      (let loop ((remaining (optional.value effects)))
        (if (null? remaining)
            #t
            (let* ((eff (car remaining))
                   (payload (raw.payload eff))
                   (name (optional.value (record.get payload '|name|)))
                   (args (optional.value (record.get payload '|args|)))
                   (entry (effect-ctor-lookup name)))
              (if (not entry)
                  (error (string-append "unknown effect constructor `"
                                        (symbol->string name) "'"))
                  (let ((expected-arity (cdr entry))
                        (actual-arity (length args)))
                    (if (not (= expected-arity actual-arity))
                        (error (string-append "effect constructor `"
                                              (symbol->string name)
                                              "' expects "
                                              (number->string expected-arity)
                                              " type argument(s), got "
                                              (number->string actual-arity)))
                        (loop (cdr remaining))))))))))
