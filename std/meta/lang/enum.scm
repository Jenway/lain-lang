(meta-source "lang/enum")

(register-rule! 'enum.parse-variants 'enum.parse-variants)
(register-rule! 'enum.parse-form 'enum.parse-form)
(register-rule! 'enum.normalize-decl 'enum.normalize-decl)

(register-form-parser! 'enum 'enum.parse-form)
(register-raw-normalizer! 'enum 'enum.normalize-decl)

(define-rule! 'enum.parse-variants
  '(rule (cursor acc)
     (let ((name (syntax.cursor-match-ident! cursor)))
       (if (optional.none? name)
           (begin
             (syntax.cursor-expect-eof! cursor)
             (list.reverse acc))
           (let ((comma (syntax.cursor-match-punct! cursor ',))
                 (variant (raw.node! 'enum.variant
                            (record 'enum.variant
                              (record.field 'name (optional.value name))))))
             (enum.parse-variants cursor (list.cons variant acc)))))))

(define-rule! 'enum.parse-form
  '(rule (form)
     (let ((cursor (syntax.form-cursor form))
           (attrs (syntax.parse-attrs cursor (list)))
           (_kw (syntax.cursor-expect-ident! cursor))
           (name (syntax.cursor-expect-ident! cursor))
           (generics (syntax.parse-generic-params cursor))
           (body (syntax.cursor-expect-group! cursor 'brace))
           (_eof (syntax.cursor-expect-eof! cursor))
           (body-cursor (syntax.group-cursor body))
           (variants (enum.parse-variants body-cursor (list)))
           (node (raw.node! 'enum
                   (record 'enum
                     (record.field 'attrs attrs)
                     (record.field 'generics generics)
                     (record.field 'variants variants)))))
       (decl.define! 'enum name node))))

(define-rule! 'enum.normalize-decl
  '(rule (decl)
     (middle.normalize-plain-decl decl 'middle.enum)))


