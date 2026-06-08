(meta-source "lang/import")

(register-rule! 'import.parse-form 'import.parse-form)
(register-rule! 'import.normalize-decl 'import.normalize-decl)

(register-form-parser! 'import 'import.parse-form)
(register-raw-normalizer! 'import 'import.normalize-decl)

(define-rule! 'import.parse-form
  '(rule (form)
     (let ((cursor (syntax.form-cursor form))
           (attrs (syntax.parse-attrs cursor (list)))
           (_kw (syntax.cursor-expect-ident! cursor))
           (path (syntax.parse-path cursor))
           (_semi (syntax.cursor-expect-punct! cursor ';))
           (_eof (syntax.cursor-expect-eof! cursor))
           (node (raw.node! 'import
                   (record 'import
                     (record.field 'attrs attrs)
                     (record.field 'path path)))))
       (decl.define! 'import (list.first path) node))))

(define-rule! 'import.normalize-decl
  '(rule (decl)
     (middle.normalize-plain-decl decl 'middle.import)))


