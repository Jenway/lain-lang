(meta-source "effects/form")

(define (effect.parse-operation cursor)
  (let* ((name (syntax.cursor-expect-ident! cursor))
         (params (syntax.parse-params
                   (syntax.cursor-expect-group! cursor '|paren|)))
         (_arrow (syntax.cursor-expect-punct! cursor '|->|))
         (ret (syntax.parse-type cursor))
         (effects (syntax.parse-optional-effects cursor))
         (_semi (syntax.cursor-match-punct! cursor '|;|)))
    (raw.node! '|effect.operation|
      (record '|effect.operation|
        (record.field '|name| name)
        (record.field '|params| params)
        (record.field '|return| ret)
        (record.field '|effects| effects)))))

(define (effect.parse-operations cursor acc)
  (let* ((next (syntax.cursor-match-ident! cursor)))
    (if (optional.none? next)
        (begin
          (syntax.cursor-expect-eof! cursor)
          (list.reverse acc))
        (let* ((operation (effect.parse-operation cursor)))
          (effect.parse-operations cursor (list.cons operation acc))))))

(define-pass (form-parser |effect| form)
  (let* ((cursor (syntax.form-cursor form))
         (attrs (syntax.parse-attrs cursor (list)))
         (_kw (syntax.cursor-expect-ident! cursor))
         (name (syntax.cursor-expect-ident! cursor))
         (body (syntax.cursor-expect-group! cursor '|brace|))
         (_eof (syntax.cursor-expect-eof! cursor))
         (body-cursor (syntax.group-cursor body))
         (operations (effect.parse-operations body-cursor (list)))
         (node (raw.node! '|effect|
                 (record '|effect|
                   (record.field '|attrs| attrs)
                   (record.field '|operations| operations)))))
    ;; Register in Scheme-side effect registry for validation
    (register-effect-ctor! name 0)
    ;; Phase 4: register a default product layout for this effect.
    ;; Default layout: {flag: u8, value: i32} — same structure as Throws
    ;; without an error/arg field. fn.make-effect-product-name will
    ;; substitute the function's actual return type for the value field.
    ;; Effects with declared operations will get richer layouts
    ;; from the operation's parameter types (future work).
    (if (null? operations)
        (effect.register-layout! name '|default|
          (list (ir.type.bits 8) (ir.type.bits 32))
          0    ;; flag-index: field 0 is the flag
          '()) ;; no arg indices
        unit)
    (decl.define-dup-checked! '|effect| name node)))

(define-pass (raw-normalizer |effect| decl)
  (middle.normalize-plain-decl decl '|middle.effect|))

