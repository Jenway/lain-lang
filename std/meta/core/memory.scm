(meta-source "core/memory")

(register-type-constructor! '|Ptr| 1 '(raw-ptr const))
(register-type-constructor! '|MutPtr| 1 '(raw-ptr mut))
(register-type-constructor! '|Ref| 1 '|ref-type|)
(register-type-constructor! '|MutRef| 1 '|mut-ref-type|)
(register-type-constructor! '|Slice| 1 '|slice-type|)

(register-raw-pointer-type! '|const| '|Ptr|)
(register-raw-pointer-type! '|mut| '|MutPtr|)
(register-raw-pointer-index-type! '|u64|)

(define-pass (intrinsic |load| block args expected-ty locals)
  (let* ((ptr-expr (list.first args))
         (ptr (core.lower-expr block ptr-expr expected-ty locals)))
    (core.load! block ptr expected-ty)))

(define-pass (intrinsic |store| block args expected-ty locals)
  (let* ((ptr-expr (list.first args))
         (value-expr (list.first (list.rest args)))
         (ptr (core.lower-expr block ptr-expr expected-ty locals))
         (value (core.lower-expr block value-expr expected-ty locals)))
    (core.store! block ptr value)
    unit))

(define-pass (intrinsic |raw-ptr-read| block args expected-ty locals)
  (let* ((ptr-expr (list.first args))
         (ptr (core.lower-expr block ptr-expr expected-ty locals)))
    (core.load! block ptr expected-ty)))

(define-pass (intrinsic |raw-ptr-write| block args expected-ty locals)
  (let* ((ptr-expr (list.first args))
         (value-expr (list.first (list.rest args)))
         (ptr (core.lower-expr block ptr-expr expected-ty locals))
         (value (core.lower-expr block value-expr expected-ty locals)))
    (core.store! block ptr value)
    unit))

;; ---------------------------------------------------------------------------
;; 类型降级: 指针/内存类型
;; ---------------------------------------------------------------------------

(define-pass (core-type-lowerer |middle.ty.raw-ptr| ty)
  (let* ((payload (middle.payload ty))
         (mutable (optional.value (record.get payload '|mutable|)))
         (pointee (core.lower-type
                    (optional.value (record.get payload '|pointee|)))))
    (type.raw-ptr mutable pointee)))

(define-pass (core-type-lowerer |middle.ty.ref| ty)
  (let* ((payload (middle.payload ty))
         (mutable (optional.value (record.get payload '|mutable|)))
         (inner (core.lower-type
                  (optional.value (record.get payload '|inner|)))))
    (type.raw-ptr mutable inner)))

(define-pass (core-type-lowerer |middle.ty.slice| ty)
  (let* ((kind (middle.kind ty)))
    (type.unsupported kind)))

(define-pass (core-type-lowerer |middle.ty.array| ty)
  (let* ((payload (middle.payload ty))
         (element (core.lower-type
                    (optional.value (record.get payload '|element|))))
         (len (optional.value (record.get payload '|len|))))
    (type.array element len)))
