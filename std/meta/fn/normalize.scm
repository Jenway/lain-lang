(meta-source "fn/normalize")

(define (middle.normalize-param raw-param)
  (let* ((payload (raw.payload raw-param)))
    (middle.node! '|middle.param|
      (record '|middle.param|
        (record.field '|name| (optional.value (record.get payload '|name|)))
        (record.field '|type| (middle.normalize-type (optional.value (record.get payload '|type|))))))))

(define (middle.normalize-params raw-params acc)
  (if (list.empty? raw-params) (list.reverse acc)
      (let* ((param (middle.normalize-param (list.first raw-params))))
        (middle.normalize-params (list.rest raw-params) (list.cons param acc)))))

(define (fn.normalize name payload middle-kind)
  (middle.node! middle-kind
    (record middle-kind
      (record.field '|name| name)
      (record.field '|attrs| (optional.value (record.get payload '|attrs|)))
      (record.field '|params| (middle.normalize-params (optional.value (record.get payload '|params|)) (list)))
      (record.field '|return| (middle.normalize-type (optional.value (record.get payload '|return|))))
      (record.field '|effects| (optional.value (record.get payload '|effects|)))
      (record.field '|body| (middle.normalize-optional-body (optional.value (record.get payload '|body|)))))))

(define-pass (raw-normalizer |fn| decl)
  (let* ((name (decl.name decl)) (raw (decl.payload decl)) (payload (raw.payload raw)))
    (fn.normalize name payload
      (if (middle.attrs-has? (optional.value (record.get payload '|attrs|)) '|foreign|)
          '|middle.foreign-fn| '|middle.fn|))))
