(meta-source "struct/lower")

(define (struct.lower-field-types fields acc)
  (if (list.empty? fields) (list.reverse acc)
      (let* ((field (list.first fields)) (payload (middle.payload field)))
        (struct.lower-field-types (list.rest fields)
          (list.cons (record '|struct.core-field|
                       (record.field '|name| (optional.value (record.get payload '|name|)))
                       (record.field '|type| (core.lower-type (optional.value (record.get payload '|type|)))))
                     acc)))))

(define-pass (core-declarer |middle.struct| item)
  (let* ((payload (middle.payload item))
         (name (optional.value (record.get payload '|name|)))
         (fields (optional.value (record.get payload '|fields|)))
         (lowered (struct.lower-field-types fields (list)))
         ;; Build (name . type-cpointer) pairs — lowered produces alist records
         (field-pairs (map (lambda (f)
                             (cons (optional.value (record.get f '|name|))
                                   (optional.value (record.get f '|type|))))
                           lowered)))
    ;; Register via Scheme registry (computes offsets, stores in C as thin cache)
    (struct-register! name field-pairs)))

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

(define-pass (core-expr-lowerer |middle.expr.struct| block expr expected-ty locals)
  (let* ((payload (middle.payload expr))
         (name (optional.value (record.get payload '|name|)))
         (fields (optional.value (record.get payload '|fields|)))
         (total-size (struct-total-size name))
         (layout (struct.lower-literal-fields block name fields locals (list))))
    ;; Integer-based aggregate: C only sees total-size + (offset . value) pairs
    (core.aggregate-layout! block total-size layout)))

(define-pass (core-expr-inferer |middle.expr.struct| expr locals)
  (let* ((payload (middle.payload expr))
         (name (optional.value (record.get payload '|name|))))
    ;; Still use C registry for type lookup (needed by type system)
    (core.struct-type name)))

(define-pass (core-expr-lowerer |middle.expr.field| block expr expected-ty locals)
  (let* ((payload (middle.payload expr))
         (base-expr (optional.value (record.get payload '|base|)))
         (field-name (optional.value (record.get payload '|field|)))
         (base-ty (core.infer-expr-type base-expr locals)))
    (core.field! block (core.lower-expr block base-expr base-ty locals)
      base-ty (core.struct-field-index-from-type base-ty field-name)
      (core.struct-field-type-from-type base-ty field-name))))

(define-pass (core-expr-inferer |middle.expr.field| expr locals)
  (let* ((payload (middle.payload expr))
         (base (optional.value (record.get payload '|base|)))
         (field-name (optional.value (record.get payload '|field|))))
    (core.struct-field-type-from-type (core.infer-expr-type base locals) field-name)))
