(meta-source "lang/import")

(define-pass (form-parser |import| form)
  (let* ((cursor (syntax.form-cursor form))
         (attrs (syntax.parse-attrs cursor (list)))
         (_kw (syntax.cursor-expect-ident! cursor))
         (path (syntax.parse-path cursor))
         (_semi (syntax.cursor-expect-punct! cursor '|;|))
         (_eof (syntax.cursor-expect-eof! cursor))
         (node (raw.node! '|import|
                 (record '|import|
                   (record.field '|attrs| attrs)
                   (record.field '|path| path)))))
    (decl.define! '|import| (list.first path) node)))

(define-pass (raw-normalizer |import| decl)
  (middle.normalize-plain-decl decl '|middle.import|))

