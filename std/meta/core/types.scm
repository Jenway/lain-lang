(meta-source "core/types")

(register-rule! 'resolve-type 'resolve-type)
(define-rule! 'resolve-type
  '(rule (ty)
     (match-syntax ty
       ((unit-ty)
        (type.constructor0 ty 'unit))
       ((never-ty)
        (type.constructor0 ty 'never))
       ((path-ty)
        (let ((name (syntax.ty.path-name ty)))
          (if (generic.type-param-registered? name)
            (generic.type-param name)
            (type.constructor0 ty name))))
       ((type-app-ty name args)
        (type.constructor ty name (resolve-type-list args)))
       ((array-ty element len)
        (type.array ty (resolve-type element) len))
       ((raw-ptr-ty mutable pointee)
        (type.raw-ptr ty mutable (resolve-type pointee))))))

(register-rule! 'resolve-type-list 'resolve-type-list)
(define-rule! 'resolve-type-list
  '(rule (types)
     (if (list.empty? types)
       '()
       (list.cons
         (resolve-type (list.first types))
         (resolve-type-list (list.rest types))))))

(register-type-constructor! 'unit 0 'core-unit-type)
(register-type-constructor! 'never 0 'core-never-type)
(register-type-constructor! 'bits 1 'core-bits-type)
(register-type-constructor! 'float 1 'core-float-type)
(register-type-constructor! 'addr 0 '(alias addr))
(register-string-literal-type! 'addr)

(register-type-constructor! 'Ordering 0 '(alias bits 8))
(register-memory-ordering-type! 'Ordering)

(register-type-constructor! 'Array 1 '(array))
(register-array-type! 'Array)

(register-type-constructor! 'i8 0 '(alias bits 8))
(register-type-constructor! 'i16 0 '(alias bits 16))
(register-type-constructor! 'i32 0 '(alias bits 32))
(register-type-constructor! 'i64 0 '(alias bits 64))
(register-type-constructor! 'u8 0 '(alias bits 8))
(register-type-constructor! 'u16 0 '(alias bits 16))
(register-type-constructor! 'u32 0 '(alias bits 32))
(register-type-constructor! 'u64 0 '(alias bits 64))
(register-type-constructor! 'bool 0 '(alias bits 1))
(register-type-constructor! 'f32 0 '(alias float 32))
(register-type-constructor! 'f64 0 '(alias float 64))

(register-type-constructor! 'Option 1 '(nominal))
(register-type-constructor! 'Result 2 '(nominal))
