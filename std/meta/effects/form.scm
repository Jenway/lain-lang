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
    (decl.define-dup-checked! '|effect| name node)))

(define-pass (raw-normalizer |effect| decl)
  (middle.normalize-plain-decl decl '|middle.effect|))

