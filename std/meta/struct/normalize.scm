(meta-source "struct/normalize")

(define (struct.normalize-field field)
  (let* ((payload (raw.payload field)))
    (middle.node! '|middle.struct.field|
      (record '|middle.struct.field|
        (record.field '|name|
          (optional.value (record.get payload '|name|)))
        (record.field '|type|
          (middle.normalize-type
            (optional.value (record.get payload '|type|))))))))

(define (struct.normalize-fields fields acc)
  (if (list.empty? fields)
      (list.reverse acc)
      (let* ((field (struct.normalize-field (list.first fields))))
        (struct.normalize-fields (list.rest fields) (list.cons field acc)))))

(define-pass (raw-normalizer |struct| decl)
  (let* ((name (decl.name decl))
         (raw (decl.payload decl))
         (payload (raw.payload raw)))
    (middle.node! '|middle.struct|
      (record '|middle.struct|
        (record.field '|name| name)
        (record.field '|attrs| (optional.value (record.get payload '|attrs|)))
        (record.field '|generics| (optional.value (record.get payload '|generics|)))
        (record.field '|where| (optional.value (record.get payload '|where|)))
        (record.field '|fields|
          (struct.normalize-fields
            (optional.value (record.get payload '|fields|)) (list)))))))

(define (struct.normalize-literal-field field)
  (let* ((payload (raw.payload field)))
    (middle.node! '|middle.expr.struct-field|
      (record '|middle.expr.struct-field|
        (record.field '|name| (optional.value (record.get payload '|name|)))
        (record.field '|value|
          (middle.normalize-expr (optional.value (record.get payload '|value|))))))))

(define (struct.normalize-literal-fields fields acc)
  (if (list.empty? fields) (list.reverse acc)
      (let* ((field (struct.normalize-literal-field (list.first fields))))
        (struct.normalize-literal-fields (list.rest fields) (list.cons field acc)))))

(define-pass (middle-normalizer |expr.struct| raw-expr)
  (let* ((payload (raw.payload raw-expr)))
    (middle.node! '|middle.expr.struct|
      (record '|middle.expr.struct|
        (record.field '|name| (optional.value (record.get payload '|name|)))
        (record.field '|fields|
          (struct.normalize-literal-fields
            (optional.value (record.get payload '|fields|)) (list)))))))

(define-pass (middle-normalizer |expr.field| raw-expr)
  (let* ((payload (raw.payload raw-expr)))
    (middle.node! '|middle.expr.field|
      (record '|middle.expr.field|
        (record.field '|base| (middle.normalize-expr (optional.value (record.get payload '|base|))))
        (record.field '|field| (optional.value (record.get payload '|field|)))))))
