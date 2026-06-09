(meta-source "lang/impl")

(define-pass (form-parser |impl| form)
  (let* ((cursor (syntax.form-cursor form))
         (attrs (syntax.parse-attrs cursor (list)))
         (_kw (syntax.cursor-expect-ident! cursor))
         (generics (syntax.parse-generic-params cursor))
         (interface-name (syntax.cursor-expect-ident! cursor))
         (interface (raw.node! '|type.path|
                      (record '|type.path|
                        (record.field '|name| interface-name))))
         (_for (syntax.cursor-expect-ident! cursor))
         (target (syntax.parse-type cursor))
         (where (syntax.parse-where cursor))
         (body (syntax.cursor-expect-group! cursor '|brace|))
         (_eof (syntax.cursor-expect-eof! cursor))
         (body-cursor (syntax.group-cursor body))
         (methods (interface.parse-methods body-cursor (list)))
         (node (raw.node! '|impl|
                 (record '|impl|
                   (record.field '|attrs| attrs)
                   (record.field '|generics| generics)
                   (record.field '|interface| interface)
                   (record.field '|target| target)
                   (record.field '|where| where)
                   (record.field '|methods| methods)))))
    (decl.define! '|impl| interface-name node)))

(define-pass (raw-normalizer |impl| decl)
  (middle.normalize-plain-decl decl '|middle.impl|))

