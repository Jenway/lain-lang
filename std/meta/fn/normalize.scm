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
      (record.field '|public| (record.get payload '|public|))
      (record.field '|attrs| (optional.value (record.get payload '|attrs|)))
      (record.field '|params| (middle.normalize-params (optional.value (record.get payload '|params|)) (list)))
      (record.field '|return| (middle.normalize-type (optional.value (record.get payload '|return|))))
      (record.field '|effects| (optional.value (record.get payload '|effects|)))
      (record.field '|body| (middle.normalize-optional-body (optional.value (record.get payload '|body|)))))))

;; fn.normalize 保留为纯函数，供 let/normalize.scm 中的统一分发器调用
