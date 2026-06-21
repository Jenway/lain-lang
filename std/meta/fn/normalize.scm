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

;; Validate a raw type node, skipping generic params.
;; Recurses into type.app nodes to check the constructor name.
(define (validate-raw-type! raw-ty generics)
  (let* ((kind (raw.kind raw-ty))
         (payload (raw.payload raw-ty)))
    (cond
      ((symbol=? kind '|type.path|)
       (let ((name (optional.value (record.get payload '|name|))))
         ;; Skip generic params (names in the generics list)
         (if (not (list.member? generics name))
             (validate-type-exists! name))))
      ((symbol=? kind '|type.app|)
       (let ((name (optional.value (record.get payload '|name|))))
         (if (not (list.member? generics name))
             (validate-type-exists! name)))
       ;; Also validate type args (they can be generic params)
       (let ((args (optional.value (record.get payload '|args|))))
         (validate-raw-types! args generics)))
      ;; type.unit, type.raw-ptr, type.ref, type.slice, type.array — all OK
      (else #t))))

(define (validate-raw-types! raw-tys generics)
  (if (list.empty? raw-tys)
      #t
      (begin
        (validate-raw-type! (list.first raw-tys) generics)
        (validate-raw-types! (list.rest raw-tys) generics))))

(define (fn.normalize name payload middle-kind)
  ;; Extract generics and raw params for type validation
  (let* ((generics (optional.value (record.get payload '|generics|)))
         (raw-params (optional.value (record.get payload '|params|))))
    ;; Validate param types (skip generic params)
    (if (not (list.empty? raw-params))
        (let loop ((remaining raw-params))
          (if (not (null? remaining))
              (let* ((param (car remaining))
                     (param-payload (raw.payload param))
                     (ty (optional.value (record.get param-payload '|type|))))
                (validate-raw-type! ty generics)
                (loop (cdr remaining))))))
    ;; Validate return type
    (validate-raw-type! (optional.value (record.get payload '|return|)) generics)
    ;; Build the middle node
    (middle.node! middle-kind
      (record middle-kind
        (record.field '|name| name)
        (record.field '|public| (record.get payload '|public|))
        (record.field '|attrs| (optional.value (record.get payload '|attrs|)))
        (record.field '|params| (middle.normalize-params raw-params (list)))
        (record.field '|return| (middle.normalize-type (optional.value (record.get payload '|return|))))
        (record.field '|effects| (optional.value (record.get payload '|effects|)))
        (record.field '|comptime| (let ((f (record.get payload '|comptime|)))
                               (if (optional.some? f) (optional.value f) #f)))
        (record.field '|body| (middle.normalize-optional-body (optional.value (record.get payload '|body|))))))))

;; fn.normalize 保留为纯函数，供 let/normalize.scm 中的统一分发器调用
