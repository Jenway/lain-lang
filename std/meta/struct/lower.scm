(meta-source "struct/lower")

(define (struct.lower-field-types fields acc)
  (if (list.empty? fields) (list.reverse acc)
      (let* ((field (list.first fields)) (payload (middle.payload field)))
        (struct.lower-field-types (list.rest fields)
          (list.cons (record '|struct.core-field|
                       (record.field '|name| (optional.value (record.get payload '|name|)))
                       (record.field '|type| (core.lower-type (optional.value (record.get payload '|type|))))
                       (record.field '|semantic-type|
                         (interface.middle-type-name
                           (optional.value (record.get payload '|type|)))))
                     acc)))))

(define (struct.interface-fields name lowered acc)
  (if (null? lowered)
      (reverse acc)
      (let* ((field (car lowered))
             (field-name (optional.value (record.get field '|name|)))
             (field-type (optional.value (record.get field '|semantic-type|))))
        (struct.interface-fields name (cdr lowered)
          (cons (list field-name field-type
                      (struct-field-offset name field-name))
                acc)))))

(define-pass* 'core-declarer '|middle.struct| (lambda (item)
  (let* ((payload (middle.payload item))
         (name (optional.value (record.get payload '|name|)))
         (fields (optional.value (record.get payload '|fields|)))
         (lowered (struct.lower-field-types fields (list)))
         ;; Build (name . type-cpointer) pairs — lowered produces alist records
         (field-pairs (map (lambda (f)
                             (cons (optional.value (record.get f '|name|))
                                   (optional.value (record.get f '|type|))))
                           lowered)))
    ;; Register via Scheme registry.  Public nominal identity and layout are
    ;; then handed to C only as an already-decided legacy interface record.
    (struct-register! name field-pairs)
    (let* ((public-opt (record.get payload '|public|))
           (public (and (optional.some? public-opt)
                        (optional.value public-opt))))
      (if public
          (core.declare-interface-type!
            name
            (struct-identity name)
            (list (struct-total-size name) (struct-alignment name))
            (struct.interface-fields name lowered (list)))
          unit)))))

(define (struct.lower-literal-fields block struct-name fields locals acc)
  (if (list.empty? fields) (list.reverse acc)
      (let* ((field (list.first fields)) (payload (middle.payload field))
             (fname (optional.value (record.get payload '|name|)))
             (value-expr (optional.value (record.get payload '|value|)))
             (offset (struct-field-offset struct-name fname))
             (value (core.lower-expr block value-expr
                      (struct-field-type struct-name fname) locals)))
        (struct.lower-literal-fields block struct-name (list.rest fields) locals
          (list.cons (cons offset value) acc)))))

(define-pass* 'core-expr-lowerer '|struct.literal| (lambda (block expr expected-ty locals)
  (let* ((payload (middle.payload expr))
         (name (optional.value (record.get payload '|name|)))
         (fields (optional.value (record.get payload '|fields|)))
         (total-size (struct-total-size name))
         (layout (struct.lower-literal-fields block name fields locals (list))))
    ;; Integer-based aggregate: C only sees total-size + (offset . value) pairs
    (core.aggregate-layout! block total-size layout))))

(define-pass* 'core-expr-inferer '|struct.literal| (lambda (expr locals)
  (let* ((payload (middle.payload expr))
         (name (optional.value (record.get payload '|name|))))
    ;; Pure Scheme struct-type record: (struct-type . name)
    (struct-type name))))

(define-pass* 'core-expr-lowerer '|struct.field| (lambda (block expr expected-ty locals)
  (let* ((payload (middle.payload expr))
         (base-expr (optional.value (record.get payload '|base|)))
         (field-name (optional.value (record.get payload '|field|)))
         (base-ty (core.infer-expr-type base-expr locals))
         (struct-name (struct-type-name base-ty))
         (offset (struct-field-offset struct-name field-name))
         (field-ty (struct-field-type struct-name field-name))
         ;; Struct values are physical addresses in L1.  Passing the pure
         ;; Scheme `(struct-type . name)` record through the C cpointer ABI
         ;; corrupts EXPR_FIELD.field_ty and makes text round-trips lose type
         ;; information.
         (physical-field-ty
           (if (struct-type? field-ty) (type.addr) field-ty)))
    (core.field-offset! block
      (core.lower-expr block base-expr base-ty locals)
      offset physical-field-ty))))

(define-pass* 'core-expr-inferer '|struct.field| (lambda (expr locals)
  (let* ((payload (middle.payload expr))
         (base (optional.value (record.get payload '|base|)))
         (field-name (optional.value (record.get payload '|field|)))
         (base-ty (core.infer-expr-type base locals))
         (struct-name (struct-type-name base-ty)))
    (struct-field-type struct-name field-name))))
