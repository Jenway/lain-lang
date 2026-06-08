(meta-source "lang/effect")

(register-rule! 'effect.parse-operation 'effect.parse-operation)
(register-rule! 'effect.parse-operations 'effect.parse-operations)
(register-rule! 'effect.parse-form 'effect.parse-form)
(register-rule! 'effect.normalize-decl 'effect.normalize-decl)

(register-form-parser! 'effect 'effect.parse-form)
(register-raw-normalizer! 'effect 'effect.normalize-decl)

(define-rule! 'effect.parse-operation
  '(rule (cursor)
     (let ((name (syntax.cursor-expect-ident! cursor))
           (params (syntax.parse-params
                     (syntax.cursor-expect-group! cursor 'paren)))
           (_arrow (syntax.cursor-expect-punct! cursor '->))
           (ret (syntax.parse-type cursor))
           (effects (syntax.parse-optional-effects cursor))
           (_semi (syntax.cursor-match-punct! cursor ';)))
       (raw.node! 'effect.operation
         (record 'effect.operation
           (record.field 'name name)
           (record.field 'params params)
           (record.field 'return ret)
           (record.field 'effects effects))))))

(define-rule! 'effect.parse-operations
  '(rule (cursor acc)
     (let ((next (syntax.cursor-match-ident! cursor)))
       (if (optional.none? next)
           (begin
             (syntax.cursor-expect-eof! cursor)
             (list.reverse acc))
           (let ((operation (effect.parse-operation cursor)))
             (effect.parse-operations cursor (list.cons operation acc)))))))

(define-rule! 'effect.parse-form
  '(rule (form)
     (let ((cursor (syntax.form-cursor form))
           (attrs (syntax.parse-attrs cursor (list)))
           (_kw (syntax.cursor-expect-ident! cursor))
           (name (syntax.cursor-expect-ident! cursor))
           (body (syntax.cursor-expect-group! cursor 'brace))
           (_eof (syntax.cursor-expect-eof! cursor))
           (body-cursor (syntax.group-cursor body))
           (operations (effect.parse-operations body-cursor (list)))
           (node (raw.node! 'effect
                   (record 'effect
                     (record.field 'attrs attrs)
                     (record.field 'operations operations)))))
       (decl.define! 'effect name node))))

(define-rule! 'effect.normalize-decl
  '(rule (decl)
     (middle.normalize-plain-decl decl 'middle.effect)))


