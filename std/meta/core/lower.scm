(register-rule! 'core.lower-type 'core.lower-type)
(register-rule! 'core.lower-param-types 'core.lower-param-types)
(register-rule! 'core.local-lookup 'core.local-lookup)
(register-rule! 'core.bind-params 'core.bind-params)
(register-rule! 'core.lower-args 'core.lower-args)
(register-rule! 'core.lower-expr 'core.lower-expr)
(register-rule! 'core.lower-stmt 'core.lower-stmt)
(register-rule! 'core.lower-stmts 'core.lower-stmts)

(define-rule! 'core.lower-type
  '(rule (ty)
     (let ((kind (middle.kind ty))
           (payload (middle.payload ty)))
       (if (symbol=? kind 'middle.ty.unit)
           (type.unit)
           (if (symbol=? kind 'middle.ty.path)
               (let ((name (optional.value
                             (record.get payload 'name))))
                 (if (symbol=? name 'i32)
                     (type.bits 32)
                     (if (symbol=? name 'bool)
                         (type.bits 1)
                         (if (symbol=? name 'addr)
                             (type.addr)
                             (type.unit)))))
               (type.unit))))))

(define-rule! 'core.lower-param-types
  '(rule (params acc)
     (if (list.empty? params)
         (list.reverse acc)
         (let ((param (list.first params))
               (payload (middle.payload param))
               (ty (core.lower-type
                     (optional.value
                       (record.get payload 'type)))))
           (core.lower-param-types
             (list.rest params)
             (list.cons ty acc))))))

(define-rule! 'core.local-lookup
  '(rule (locals name)
     (if (list.empty? locals)
         (core.function-by-name name)
         (let ((local (list.first locals))
               (local-name (optional.value
                             (record.get local 'name))))
           (if (symbol=? local-name name)
               (optional.value
                 (record.get local 'value))
               (core.local-lookup (list.rest locals) name))))))

(define-rule! 'core.bind-params
  '(rule (function params index locals)
     (if (list.empty? params)
         (list.reverse locals)
         (let ((param (list.first params))
               (payload (middle.payload param))
               (name (optional.value
                       (record.get payload 'name)))
               (value (core.param function index))
               (local (record 'local
                        (record.field 'name name)
                        (record.field 'value value))))
           (core.bind-params
             function
             (list.rest params)
             (u64.add1 index)
             (list.cons local locals))))))

(define-rule! 'core.lower-args
  '(rule (block args param-types acc)
     (if (list.empty? args)
         (list.reverse acc)
         (let ((arg (list.first args))
               (ty (list.first param-types))
               (value (core.lower-expr block arg ty (list))))
           (core.lower-args
             block
             (list.rest args)
             (list.rest param-types)
             (list.cons value acc))))))

(define-rule! 'core.lower-expr
  '(rule (block expr expected-ty locals)
     (let ((kind (middle.kind expr))
           (payload (middle.payload expr)))
       (if (symbol=? kind 'middle.expr.number)
           (core.const-bits!
             block
             expected-ty
             (optional.value
               (record.get payload 'raw)))
           (if (symbol=? kind 'middle.expr.string)
               (core.const-string!
                 block
                 expected-ty
                 (optional.value
                   (record.get payload 'raw)))
               (if (symbol=? kind 'middle.expr.path)
                   (let ((path (optional.value
                                 (record.get payload 'path))))
                     (core.local-lookup locals (list.first path)))
                   (if (symbol=? kind 'middle.expr.binary)
                       (let ((op (optional.value
                                   (record.get payload 'op))))
                         (if (symbol=? op '+)
                             (let ((left (core.lower-expr
                                           block
                                           (optional.value
                                             (record.get payload 'left))
                                           expected-ty
                                           locals))
                                   (right (core.lower-expr
                                            block
                                            (optional.value
                                              (record.get payload 'right))
                                            expected-ty
                                            locals)))
                               (core.primitive!
                                 block
                                 'integer.add
                                 (list left right)
                                 expected-ty))
                             (core.const-bits! block expected-ty 0)))
                       (if (symbol=? kind 'middle.expr.call)
                           (let ((callee (optional.value
                                           (record.get payload 'callee)))
                                 (args (optional.value
                                         (record.get payload 'args))))
                             (let ((callee-payload (middle.payload callee))
                                   (path (optional.value
                                           (record.get callee-payload 'path))))
                               (let ((function (core.function-by-name
                                                 (list.first path))))
                                 (core.call!
                                   block
                                   function
                                   (core.lower-args
                                     block
                                     args
                                     (core.function-param-types function)
                                     (list))))))
                           (core.const-bits! block expected-ty 0)))))))))

(define-rule! 'core.lower-stmt
  '(rule (block stmt ret-ty locals)
     (let ((kind (middle.kind stmt))
           (payload (middle.payload stmt)))
       (if (symbol=? kind 'middle.stmt.return)
           (let ((value (optional.value
                         (record.get payload 'value))))
             (if (optional.none? value)
                 (core.return-none! block)
                 (core.return-value!
                   block
                   (core.lower-expr
                     block
                     (optional.value value)
                     ret-ty
                     locals))))
           (if (symbol=? kind 'middle.stmt.tail)
               (core.return-value!
                 block
                 (core.lower-expr
                   block
                   (optional.value
                     (record.get payload 'expr))
                   ret-ty
                   locals))
               unit)))))

(define-rule! 'core.lower-stmts
  '(rule (block stmts ret-ty locals)
     (if (list.empty? stmts)
         (core.return-none! block)
         (begin
           (core.lower-stmt block (list.first stmts) ret-ty locals)
           (if (list.empty? (list.rest stmts))
               unit
               (core.lower-stmts
                 block
                 (list.rest stmts)
                 ret-ty
                 locals))))))


