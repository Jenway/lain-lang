(meta-source "module/parse")

;; ===========================================================================
;; Module / Signature / Export parsing
;;
;; First implementation goal:
;; - support top-level `let name = import("a::b");`
;; - reserve top-level `let name = signature { ... };`
;; - reserve top-level `let name = module { ... };`
;; - support `export { a, b, c }`
;;
;; This pass only builds raw/middle placeholders so the unified top-level
;; pipeline can carry these declarations. Real module/signature semantics
;; will be implemented in later phases.
;; ===========================================================================

(define (module.split-import-path str)
  (let* ((len (string-length str)))
    (let loop ((i 0) (start 0) (acc (list)))
      (if (>= i len)
          (let* ((segment (substring str start len)))
            (list.reverse
              (list.cons (string->symbol segment) acc)))
          (if (and (< (+ i 1) len)
                   (char=? (string-ref str i) #\:)
                   (char=? (string-ref str (+ i 1)) #\:))
              (let* ((segment (substring str start i)))
                (loop (+ i 2) (+ i 2)
                      (list.cons (string->symbol segment) acc)))
              (loop (+ i 1) start acc))))))

(define (module.parse-import-binding cursor attrs name)
  (let* ((group (syntax.cursor-expect-group! cursor '|paren|))
         (body (syntax.group-cursor group))
         (path-str (syntax.cursor-match-string! body)))
    (if (optional.none? path-str)
        (error "import(...) expects a string literal path")
        (let* ((_eof-body (syntax.cursor-expect-eof! body))
               (_semi (syntax.cursor-match-punct! cursor '|;|))
               (_eof (syntax.cursor-expect-eof! cursor))
               (path (module.split-import-path
                       (symbol->string (optional.value path-str))))
               (inner-payload (record '|import.binding|
                                (record.field '|attrs| attrs)
                                (record.field '|path| path)))
               (unified (raw.node! '|let|
                          (record '|let|
                            (record.field '|name| name)
                            (record.field '|type-kind| '|import-binding|)
                            (record.field '|payload| inner-payload)))))
          (decl.define-dup-checked! '|let| name unified)))))

(define (module.parse-signature-binding cursor attrs name)
  (let* ((body (syntax.cursor-expect-group! cursor '|brace|))
         (_semi (syntax.cursor-match-punct! cursor '|;|))
         (_eof (syntax.cursor-expect-eof! cursor))
         (inner-payload (record '|signature|
                          (record.field '|attrs| attrs)
                          (record.field '|body| body)))
         (unified (raw.node! '|let|
                    (record '|let|
                      (record.field '|name| name)
                      (record.field '|type-kind| '|signature|)
                      (record.field '|payload| inner-payload)))))
    (decl.define-dup-checked! '|let| name unified)))

(define (module.parse-module-binding cursor attrs name)
  (let* ((body (syntax.cursor-expect-group! cursor '|brace|))
         (_semi (syntax.cursor-match-punct! cursor '|;|))
         (_eof (syntax.cursor-expect-eof! cursor))
         (inner-payload (record '|module|
                          (record.field '|attrs| attrs)
                          (record.field '|body| body)))
         (unified (raw.node! '|let|
                    (record '|let|
                      (record.field '|name| name)
                      (record.field '|type-kind| '|module|)
                      (record.field '|payload| inner-payload)))))
    (decl.define-dup-checked! '|let| name unified)))

(define (module.parse-meta-alias-binding cursor attrs name saved-index)
  (syntax.cursor-set-index! cursor saved-index)
  (let* ((path (parse-path cursor))
         (_semi (syntax.cursor-match-punct! cursor '|;|))
         (_eof (syntax.cursor-expect-eof! cursor))
         (inner-payload (record '|meta.alias|
                          (record.field '|attrs| attrs)
                          (record.field '|path| path)))
         (unified (raw.node! '|let|
                    (record '|let|
                      (record.field '|name| name)
                      (record.field '|type-kind| '|meta-alias|)
                      (record.field '|payload| inner-payload)))))
    (decl.define-dup-checked! '|let| name unified)))

(define-pass (form-parser |let| form)
  (let* ((cursor (syntax.form-cursor form))
         (attrs (syntax.parse-attrs cursor (list)))
         (_kw (syntax.cursor-expect-ident! cursor))
         (name (syntax.cursor-expect-ident! cursor))
         (_assign (syntax.cursor-expect-punct! cursor '|=|))
         (rhs-saved (syntax.cursor-get-index cursor))
         (rhs (syntax.cursor-match-ident! cursor)))
    (if (optional.none? rhs)
        (error "top-level let currently supports import/module/signature bindings only")
        (let* ((rhs-name (optional.value rhs)))
          (cond
            ((symbol=? rhs-name '|import|)
             (module.parse-import-binding cursor attrs name))
            ((symbol=? rhs-name '|signature|)
             (module.parse-signature-binding cursor attrs name))
            ((symbol=? rhs-name '|module|)
             (module.parse-module-binding cursor attrs name))
            (else
             (module.parse-meta-alias-binding cursor attrs name rhs-saved)))))))

(define (module.parse-export-names cursor acc)
  (let* ((next (syntax.cursor-match-ident! cursor)))
    (if (optional.none? next)
        (begin
          (syntax.cursor-expect-eof! cursor)
          (list.reverse acc))
        (let* ((name (optional.value next))
               (_comma (syntax.cursor-match-punct! cursor '|,|)))
          (module.parse-export-names cursor (list.cons name acc))))))

(define-pass (form-parser |export| form)
  (let* ((cursor (syntax.form-cursor form))
         (attrs (syntax.parse-attrs cursor (list)))
         (_kw (syntax.cursor-expect-ident! cursor))
         (body (syntax.cursor-expect-group! cursor '|brace|))
         (_semi (syntax.cursor-match-punct! cursor '|;|))
         (_eof (syntax.cursor-expect-eof! cursor))
         (body-cursor (syntax.group-cursor body))
         (names (module.parse-export-names body-cursor (list)))
         (node (raw.node! '|export|
                 (record '|export|
                   (record.field '|attrs| attrs)
                   (record.field '|names| names)))))
    (decl.define! '|export| '|export| node)))

(define-pass (raw-normalizer |export| decl)
  (middle.normalize-plain-decl decl '|middle.export|))

(define *module-registry* (list))
(define *signature-registry* (list))

(define (module.register-name! name registry)
  (if (list.member? registry name)
      registry
      (list.cons name registry)))

(define (module.register-module! name)
  (set! *module-registry*
    (module.register-name! name *module-registry*)))

(define (module.register-signature! name)
  (set! *signature-registry*
    (module.register-name! name *signature-registry*)))

(define (module.declare-export-names names)
  (if (list.empty? names)
      unit
      (begin
        (core.mark-export! (list.first names))
        (module.declare-export-names (list.rest names)))))

(define-pass (core-declarer |middle.export| item)
  (meta.ensure-static-position! '|middle.export|)
  unit)

(define-pass (core-lowerer |middle.export| item)
  (meta.ensure-static-position! '|middle.export|)
  (let* ((payload (middle.payload item))
         (raw-inner (optional.value (record.get payload '|payload|)))
         (names (optional.value (record.get raw-inner '|names|))))
    (module.declare-export-names names)))

(define-pass (core-declarer |middle.signature| item)
  (meta.ensure-static-position! '|middle.signature|)
  (let* ((payload (middle.payload item))
         (name (optional.value (record.get payload '|name|))))
    (module.register-signature! name)
    (core.declare-signature! name)))

(define-pass (core-lowerer |middle.signature| item)
  (meta.ensure-static-position! '|middle.signature|)
  unit)

(define-pass (core-declarer |middle.module| item)
  (meta.ensure-static-position! '|middle.module|)
  (let* ((payload (middle.payload item))
         (name (optional.value (record.get payload '|name|))))
    (module.register-module! name)
    (core.declare-module! name)))

(define-pass (core-lowerer |middle.module| item)
  (meta.ensure-static-position! '|middle.module|)
  unit)

(define (module.declare-imported-fn-alias! alias fn-name)
  (let* ((sub (ir.sub.by-name fn-name))
         (ret-ty (ir.sub.ret-type sub))
         (param-tys (ir.sub.params sub))
         (link-name (ir.sub.link-name sub)))
    (host.new-extern alias link-name param-tys ret-ty)))

(define-pass (core-declarer |middle.meta-alias| item)
  (let* ((payload (middle.payload item))
         (name (optional.value (record.get payload '|name|)))
         (raw-inner (optional.value (record.get payload '|payload|)))
         (path (optional.value (record.get raw-inner '|path|)))
         (fn-export (import.resolve-qualified-symbol path))
         (module-export (import.resolve-qualified-module-name path))
         (signature-export (import.resolve-qualified-signature-name path)))
    (cond
      (fn-export
       (module.declare-imported-fn-alias! name fn-export))
      (module-export
       (module.register-module! name)
       (core.declare-module! name))
      (signature-export
        (module.register-signature! name)
        (core.declare-signature! name))
      (else
       (error (string-append
                "unsupported top-level let binding path: "
                (symbol->string (list.first path))))))))

(define-pass (core-lowerer |middle.meta-alias| item)
  unit)
