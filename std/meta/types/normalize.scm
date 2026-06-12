(meta-source "types/normalize")

;; ═══════════════════════════════════════════════════
;; 类型中间表示规范化
;; ═══════════════════════════════════════════════════

(define (middle.normalize-type raw-ty)
  (let* ((kind (raw.kind raw-ty))
         (payload (raw.payload raw-ty)))
    (cond
      ((symbol=? kind '|type.path|)
       (middle.node! '|middle.ty.path|
         (record '|middle.ty.path|
           (record.field '|name|
             (optional.value
               (record.get payload '|name|))))))
      ((symbol=? kind '|type.unit|)
       (middle.node! '|middle.ty.unit|
         (record '|middle.ty.unit|)))
      ((symbol=? kind '|type.app|)
       (middle.node! '|middle.ty.app|
         (record '|middle.ty.app|
           (record.field '|name|
             (optional.value
               (record.get payload '|name|)))
           (record.field '|args|
             (middle.normalize-types
               (optional.value
                 (record.get payload '|args|))
               (list))))))
      ((symbol=? kind '|type.raw-ptr|)
       (middle.node! '|middle.ty.raw-ptr|
         (record '|middle.ty.raw-ptr|
           (record.field '|mutable|
             (optional.value
               (record.get payload '|mutable|)))
           (record.field '|pointee|
             (middle.normalize-type
               (optional.value
                 (record.get payload '|pointee|)))))))
      ((symbol=? kind '|type.ref|)
       (middle.node! '|middle.ty.ref|
         (record '|middle.ty.ref|
           (record.field '|mutable|
             (optional.value
               (record.get payload '|mutable|)))
           (record.field '|inner|
             (middle.normalize-type
               (optional.value
                 (record.get payload '|inner|)))))))
      ((symbol=? kind '|type.slice|)
       (middle.node! '|middle.ty.slice|
         (record '|middle.ty.slice|
           (record.field '|element|
             (middle.normalize-type
               (optional.value
                 (record.get payload '|element|)))))))
      ((symbol=? kind '|type.array|)
       (middle.node! '|middle.ty.array|
         (record '|middle.ty.array|
           (record.field '|element|
             (middle.normalize-type
               (optional.value
                 (record.get payload '|element|))))
           (record.field '|len|
             (optional.value
               (record.get payload '|len|))))))
      ((symbol=? kind '|type.fn|)
       (middle.node! '|middle.ty.fn|
         (record '|middle.ty.fn|
           (record.field '|params|
             (middle.normalize-types
               (optional.value
                 (record.get payload '|params|))
               (list)))
           (record.field '|return|
             (middle.normalize-type
               (optional.value
                 (record.get payload '|return|))))
           (record.field '|effects|
             (optional.value
               (record.get payload '|effects|))))))
      (else
       (middle.node! '|middle.ty.unknown|
         (record '|middle.ty.unknown|
           (record.field '|raw-kind| kind)))))))

(define (middle.normalize-types raw-types acc)
  (if (list.empty? raw-types)
      (list.reverse acc)
      (let* ((ty (middle.normalize-type (list.first raw-types))))
        (middle.normalize-types
          (list.rest raw-types)
          (list.cons ty acc)))))
