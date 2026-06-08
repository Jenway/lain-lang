(meta-source "lang/interface")

(register-rule! 'interface.parse-method 'interface.parse-method)
(register-rule! 'interface.parse-methods 'interface.parse-methods)
(register-rule! 'interface.parse-form 'interface.parse-form)
(register-rule! 'interface.normalize-decl 'interface.normalize-decl)
(register-rule! 'interface.prepare-bound 'interface.prepare-bound)

(register-form-parser! 'interface 'interface.parse-form)
(register-raw-normalizer! 'interface 'interface.normalize-decl)
(register-constraint! 'Interface 'interface-predicate)
(register-interface-rule! 'Interface 'interface-requirements)

(define-rule! 'interface.parse-method
  '(rule (cursor)
     (let ((name (syntax.cursor-expect-ident! cursor))
           (params (syntax.parse-params
                     (syntax.cursor-expect-group! cursor 'paren)))
           (_arrow (syntax.cursor-expect-punct! cursor '->))
           (ret (syntax.parse-type cursor))
           (effects (syntax.parse-optional-effects cursor))
           (_semi (syntax.cursor-expect-punct! cursor ';)))
       (raw.node! 'interface.method
         (record 'interface.method
           (record.field 'name name)
           (record.field 'params params)
           (record.field 'return ret)
           (record.field 'effects effects))))))

(define-rule! 'interface.parse-methods
  '(rule (cursor acc)
     (let ((next (syntax.cursor-match-ident! cursor)))
       (if (optional.none? next)
           (begin
             (syntax.cursor-expect-eof! cursor)
             (list.reverse acc))
           (let ((method (interface.parse-method cursor)))
             (interface.parse-methods cursor (list.cons method acc)))))))

(define-rule! 'interface.parse-form
  '(rule (form)
     (let ((cursor (syntax.form-cursor form))
           (attrs (syntax.parse-attrs cursor (list)))
           (_kw (syntax.cursor-expect-ident! cursor))
           (name (syntax.cursor-expect-ident! cursor))
           (generics (syntax.parse-generic-params cursor))
           (where (syntax.parse-where cursor))
           (body (syntax.cursor-expect-group! cursor 'brace))
           (_eof (syntax.cursor-expect-eof! cursor))
           (body-cursor (syntax.group-cursor body))
           (methods (interface.parse-methods body-cursor (list)))
           (node (raw.node! 'interface
                   (record 'interface
                     (record.field 'attrs attrs)
                     (record.field 'generics generics)
                     (record.field 'where where)
                     (record.field 'methods methods)))))
       (decl.define! 'interface name node))))

(define-rule! 'interface.normalize-decl
  '(rule (decl)
     (let ((name (decl.name decl))
           (raw (decl.payload decl))
           (payload (raw.payload raw)))
       (middle.node! 'middle.interface
         (record 'middle.interface
           (record.field 'name name)
           (record.field 'payload payload))))))

(define-rule! 'interface.prepare-bound
  '(rule (name)
     (if (registry.interface-rule-registered? name)
         name
         (diagnostic.error
           name
           "interface bound requires a std/meta interface rule"
           "register an interface rule before using this bound"))))


